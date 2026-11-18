"""Rebuild a feed-forward 4-bit asset set (ffn_v2_*.bin + ffn_v2.txt) from a shipped runtime.

Both networks this project supports keep their feed-forward as two dense matrices per block --
1024 wide, 4096 feed-forward -- stored FP8 in a weight resource of nvngx_dlssnr.dll: Model A in
WEIGHTS_HT blocks 31..38, Model B in WEIGHTS_CC_SMALL_FP8 blocks 11..18. This reads those matrices,
quantises them to the 4-bit block-scaled format the local ffn_v2 kernels expect, and writes the
file the add-on loads.

The layout arithmetic lives in nvfp4_layout.py; the reasoning is in docs/nvfp4-modelb-ffn.md.

    python tools/nvfp4/make_ffn_assets.py --model B --output nvfp4-out

--model A rebuilds Model A's set from the same runtime weights, which is how the pipeline is checked:
the result must correlate ~0.9956 with the FP8 it came from, i.e. E2M1's own noise floor, the same
band NVIDIA's own file reaches. --check instead unpacks NVIDIA's file and compares it, which is what
proves the layouts.
"""
import argparse
import hashlib
import struct
from pathlib import Path

import numpy as np

import nvfp4_layout as L

WIDTH = 1024                 # the network width both models share
FFN = 4096                   # the feed-forward expansion
BLOCKS = 8                   # blocks 1024 -> 4096 -> 1024
STRIDE = 4_720_640           # w1 | w2 | gain of one block in the asset file
WQ_BYTES = 2_359_296         # 4096 * 1024 * 4 bits / 8, one matrix
GAIN_BYTES = 2_048           # 1024 fp16 channel gains
DIVISOR = 6.0                # e2m1's largest magnitude, i.e. the code the block scale divides by
SCALE_FLOOR = 2.0 ** -9      # e4m3's smallest subnormal
SCALE_CEIL = 448.0           # e4m3's largest finite value
HEADROOM = 175.5             # where amax / (DIVISOR * out) wants to land; A's files use 146..254

MODELS = {
    'A': dict(resource='WEIGHTS_HT', first_block=31, folder='modelA'),
    'B': dict(resource='WEIGHTS_CC_SMALL_FP8', first_block=11, folder='modelB'),
}


def quantise(W, out):
    """Quantise one canonical matrix; returns the 2,359,296-byte block and statistics."""
    N, K = W.shape
    scaled = W / out
    amax = np.abs(scaled).reshape(N, K // 16, 16).max(axis=2)     # one scale per 16 K
    raw = np.clip(amax / DIVISOR, SCALE_FLOOR, SCALE_CEIL)
    codes = L.e4_encode(raw)
    # decode the scale exactly as the kernel will, then quantise against it
    signed = codes.astype(np.int32) & 0x7F
    exponent, mantissa = (signed >> 3) & 0xF, signed & 7
    decoded = np.where(exponent == 0, mantissa * SCALE_FLOOR,
                       (1.0 + mantissa / 8.0) * np.power(2.0, exponent - 7))
    scale = np.repeat(decoded, 16, axis=1)
    q = L.e2_encode(scaled / scale)

    nibble, where = L.masks(N, K)
    buffer = np.zeros(WQ_BYTES, dtype=np.uint8)
    buffer[nibble[:, 0::2]] = q[:, 0::2] | ((q[:, 1::2] & 0x0F) << 4)   # k even -> low half
    buffer[where[:, 0::16]] = codes

    rebuilt = (L.E2M1[q & 7] * np.where(q & 8, -1.0, 1.0)) * scale * out
    return buffer, dict(scale_min=float(decoded.min()), scale_max=float(decoded.max()),
                        clipped_hi=int((raw >= SCALE_CEIL).sum()),
                        clipped_lo=int((raw <= SCALE_FLOOR).sum()),
                        corr=float(np.corrcoef(rebuilt.ravel(), W.ravel())[0, 1]),
                        relerr=float(np.linalg.norm(rebuilt - W) / np.linalg.norm(W)))


def choose_out(W):
    """The per-matrix scale multiplier that centres the block scales in E4M3's range."""
    return float(np.abs(W).reshape(W.shape[0], W.shape[1] // 16, 16).max(axis=2).max()
                 / (DIVISOR * HEADROOM))


def read_block(resource, by_name, block):
    """One block's two canonical matrices and its channel gains, from the runtime's resource."""
    expand = by_name['block%d.layer0.layer' % block]
    contract = by_name['block%d.layer1.layer' % block]
    # the expand record carries 16 bytes of trailing padding, the contract record the gains
    if expand['bytes'] not in (FFN * WIDTH, FFN * WIDTH + 16):
        raise SystemExit('block%d expand is %d bytes, not %dx%d' % (block, expand['bytes'], FFN, WIDTH))
    if contract['bytes'] != WIDTH * FFN + GAIN_BYTES:
        raise SystemExit('block%d contract is %d bytes, not %dx%d plus %d of gain'
                         % (block, contract['bytes'], WIDTH, FFN, GAIN_BYTES))
    w1 = L.matrix(resource, expand['off'], FFN, WIDTH)
    w2 = L.matrix(resource, contract['off'], WIDTH, FFN)
    tail = contract['off'] + WIDTH * FFN
    gain = np.frombuffer(resource[tail:tail + GAIN_BYTES], dtype='<f2').astype(np.float32)
    return w1, w2, gain


def build(dll, model, output, verify_only):
    spec = MODELS[model]
    resource = L.resources(dll.read_bytes())[spec['resource']]
    by_name = {r['name']: r for r in L.records(resource)}

    blocks, assembled = [], bytearray(STRIDE * BLOCKS)
    print('%-4s %-9s %-9s %-11s %s' % ('blk', 'runtime', 'W1 amax', 'out1', 'W1 corr / W2 corr'))
    for i in range(BLOCKS):
        runtime_block = spec['first_block'] + i
        w1, w2, gain = read_block(resource, by_name, runtime_block)
        out1, out2 = choose_out(w1), choose_out(w2)
        packed1, stats1 = quantise(w1, out1)
        packed2, stats2 = quantise(w2, out2)
        base = i * STRIDE
        assembled[base:base + WQ_BYTES] = packed1.tobytes()
        assembled[base + WQ_BYTES:base + 2 * WQ_BYTES] = packed2.tobytes()
        assembled[base + 2 * WQ_BYTES:base + STRIDE] = gain.astype('<f2').tobytes()
        blocks.append(dict(index=i, runtime_block=runtime_block, out1=out1, out2=out2,
                           amax1=float(np.abs(w1).max()), amax2=float(np.abs(w2).max()),
                           stats1=stats1, stats2=stats2,
                           gain_min=float(gain.min()), gain_max=float(gain.max()),
                           gain_mean=float(gain.mean())))
        print('%-4d %-9d %-9.4f %-11.9g %.6f / %.6f' % (i, runtime_block, np.abs(w1).max(), out1,
                                                         stats1['corr'], stats2['corr']))
    payload = bytes(assembled)
    digest = hashlib.sha256(payload).hexdigest()

    print()
    print('%-4s %-9s %-9s %-9s %-9s %-9s %s' % ('blk', 'out1', 'W1 rel', 'out2', 'W2 rel',
                                                'scale lo..hi', 'clamps hi/lo'))
    for b in blocks:
        print('%-4d %-9.6g %-9.5f %-9.6g %-9.5f %-9.4g..%-4.4g %d/%d %d/%d'
              % (b['index'], b['out1'], b['stats1']['relerr'], b['out2'], b['stats2']['relerr'],
                 b['stats1']['scale_min'], b['stats1']['scale_max'],
                 b['stats1']['clipped_hi'], b['stats2']['clipped_hi'],
                 b['stats1']['clipped_lo'], b['stats2']['clipped_lo']))
    print()
    print('%d bytes, sha256 %s' % (len(payload), digest))
    if verify_only:
        return payload, blocks

    manifest = ['kernels stages 2 steps 2']
    for b in blocks:
        base = b['index'] * STRIDE
        def bits(value):
            return '0x' + struct.pack('>f', np.float32(value)).hex()   # the manifest is big-endian
        manifest.append('block %d w1 %d w2 %d gain %d tx 0x3f800000 out1 %s th 0x3f800000 out2 %s'
                        % (b['index'], base, base + WQ_BYTES, base + 2 * WQ_BYTES,
                           bits(b['out1']), bits(b['out2'])))
    manifest.append('bytes %d sha256 %s' % (len(payload), digest))
    manifest.append('small_direct max_tokens 640 file ffn08_k4peek.cubin')

    output.mkdir(parents=True, exist_ok=True)
    (output / 'ffn_v2_trained.bin').write_bytes(payload)
    (output / 'ffn_v2.txt').write_text('\n'.join(manifest) + '\n', newline='\n')
    print('wrote %s and %s' % (output / 'ffn_v2_trained.bin', output / 'ffn_v2.txt'))
    return payload, blocks


def check_against_factory(dll, assets):
    """Unpack NVIDIA's own 4-bit file and compare it with Model A's runtime weights.

    This is the check that proves the two layouts: if either masks() or deswizzle() is wrong, the
    correlation collapses to noise instead of E2M1's ~0.9956 ceiling.
    """
    factory = Path(assets) / 'ffn_v2_trained.bin'
    manifest = Path(assets) / 'ffn_v2.txt'
    if not factory.exists() or not manifest.exists():
        print()
        print('skipped the factory check: %s or %s is missing' % (factory, manifest))
        return
    raw = factory.read_bytes()
    resource = L.resources(dll.read_bytes())['WEIGHTS_HT']
    by_name = {r['name']: r for r in L.records(resource)}
    print()
    print('factory %s, %d bytes' % (factory, len(raw)))
    print('%-4s %-3s %-10s %-9s %s' % ('blk', 'w', 'corr', 'relerr', 'amax 4-bit / fp8'))
    for line in manifest.read_text().splitlines():
        if not line.startswith('block '):
            continue
        field = line.split()
        index = int(field[1])
        for which, (N, K), offset, out_field in (
                ('w1', (FFN, WIDTH), int(field[3]), 11),
                ('w2', (WIDTH, FFN), int(field[5]), 15)):
            out = struct.unpack('>f', bytes.fromhex(field[out_field][2:]))[0]   # big-endian
            layer = 0 if which == 'w1' else 1
            record = by_name['block%d.layer%d.layer' % (31 + index, layer)]
            truth = L.matrix(resource, record['off'], N, K)
            rebuilt = L.unpack4(raw, offset, N, K, out)
            corr = np.corrcoef(rebuilt.ravel(), truth.ravel())[0, 1]
            rel = np.linalg.norm(rebuilt - truth) / np.linalg.norm(truth)
            print('%-4d %-3s %-10.6f %-9.5f %.5f / %.5f'
                  % (index, which, corr, rel, np.abs(rebuilt).max(), np.abs(truth).max()))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--dll', type=Path, default=Path(r'C:\Optiscaler\nvngx_dlssnr.dll'),
                    help='the DLSS NR runtime whose weight resource is read')
    ap.add_argument('--model', choices=sorted(MODELS), default='B')
    ap.add_argument('--output', type=Path, default=Path('nvfp4-out'),
                    help='where ffn_v2_trained.bin and ffn_v2.txt are written')
    ap.add_argument('--check', type=Path, metavar='ASSETS',
                    help="Model A's deployed folder, to reproduce NVIDIA's factory file")
    args = ap.parse_args()
    build(args.dll, args.model, args.output, verify_only=False)
    if args.check:
        check_against_factory(args.dll, args.check)


if __name__ == '__main__':
    main()
