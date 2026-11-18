"""Weight resources of the DLSS NR runtime, and the two matrix layouts its assets use.

Everything here is arithmetic recovered by measurement against a shipped runtime; the reasoning is
written down in docs/nvfp4-modelb-ffn.md. Three facts:

  * a weight resource is one flat run of records, each "8-byte name length | name | 8-byte body
    length | body | footnote", where the body starts with an 8-byte payload length and the payload
    is followed by (type, layout, ndimension, dimensions[]);
  * the runtime keeps a matrix FP8 code-swizzled in 512-byte slices, 16 output channels by 32 K,
    slice order N-fastest (deswizzle);
  * the 4-bit asset files keep it block-scaled per 16 input channels in 4608-byte blocks, with the
    scale quantised to E4M3 and multiplied by the manifest's out (masks, unpack4).

Nothing here is specific to one network: it is offsets and bit fields.
"""
import struct

import numpy as np

RCDATA = 10
PE32_PLUS = 0x20b


def resources(data):
    """Named RCDATA resources of a PE image, as {name: payload}."""
    def u16(p): return struct.unpack_from('<H', data, p)[0]
    def u32(p): return struct.unpack_from('<I', data, p)[0]

    if data[:2] != b'MZ':
        raise ValueError('Not a PE image')
    nt = u32(60)
    if data[nt:nt + 4] != b'PE\x00\x00' or u16(nt + 24) != PE32_PLUS:
        raise ValueError('Expected a PE32+ image')
    sections = nt + 24 + u16(nt + 20)

    def offset(rva, size):
        for i in range(u16(nt + 6)):
            p = sections + i * 40
            va, raw_size, raw = u32(p + 12), u32(p + 16), u32(p + 20)
            if va <= rva and rva + size <= va + raw_size:
                return raw + rva - va
        raise ValueError('Resource outside a file-backed section')

    base = offset(u32(nt + 24 + 112 + 16), 1)

    def entries(relative):
        p = base + relative
        count = u16(p + 12) + u16(p + 14)
        if count > 4096:
            raise ValueError('Resource directory too large')
        return [struct.unpack_from('<II', data, p + 16 + 8 * i) for i in range(count)]

    def name_of(value):
        p = base + (value & 0x7fffffff)
        return data[p + 2:p + 2 + 2 * u16(p)].decode('utf-16-le')

    found = {}
    for kind, branch in entries(0):
        if kind & 0x80000000 or kind != RCDATA:
            continue
        for name, language in entries(branch & 0x7fffffff):
            if not name & 0x80000000:
                continue
            leaves = entries(language & 0x7fffffff)
            if len(leaves) != 1:
                raise ValueError('Ambiguous language for ' + name_of(name))
            rva, size = struct.unpack_from('<II', data, base + (leaves[0][1] & 0x7fffffff))
            p = offset(rva, size)
            found[name_of(name)] = data[p:p + size]
    return found


def records(blob):
    """Walk one weight resource into per-matrix records, offsets relative to the blob."""
    def u64(p): return struct.unpack_from('<Q', blob, p)[0]

    total = u64(0)
    if total != len(blob):
        raise ValueError('Resource self-size %d does not match %d bytes' % (total, len(blob)))
    out, cursor = [], 8
    while cursor < total:
        length = u64(cursor)
        cursor += 8
        name = blob[cursor:cursor + length].decode('ascii')
        cursor += length
        body_length = u64(cursor)
        cursor += 8
        body = cursor
        payload_length = u64(body + 8)
        payload = body + 20
        footnote = payload + payload_length
        type_id, layout_id, ndims = struct.unpack_from('<IIQ', blob, footnote)
        dims = list(struct.unpack_from('<' + 'I' * ndims, blob, footnote + 16))
        out.append({'name': name, 'off': payload, 'bytes': payload_length,
                    'dims': dims, 'type': type_id, 'layout': layout_id})
        cursor = body + body_length
    return out


def _tables():
    decode = np.zeros(256, dtype=np.float64)
    for code in range(256):
        sign = -1.0 if code & 0x80 else 1.0
        exponent, mantissa = (code >> 3) & 0xF, code & 0x7
        if exponent == 15 and mantissa == 7:
            decode[code] = np.nan
        elif exponent == 0:
            decode[code] = sign * mantissa * 2.0 ** -9
        else:
            decode[code] = sign * (1.0 + mantissa / 8.0) * 2.0 ** (exponent - 7)
    positive = []
    for code in range(127):        # code 127 is NaN; np.argmin would silently return it
        exponent, mantissa = (code >> 3) & 0xF, code & 0x7
        if exponent == 0:
            value = mantissa * 2.0 ** -9
        else:
            value = (1.0 + mantissa / 8.0) * 2.0 ** (exponent - 7)
        positive.append((value, code))
    positive.sort()
    return (decode,
            np.array([v for v, _ in positive], dtype=np.float64),
            np.array([c for _, c in positive], dtype=np.uint8))


E4, E4_VALUES, E4_CODES = _tables()
E2M1 = np.array([0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0])


def e4_encode(x):
    """Nearest E4M3 code of x (sign included), saturating at the format's ends."""
    x = np.asarray(x, dtype=np.float64)
    a = np.clip(np.abs(x), E4_VALUES[0], E4_VALUES[-1])
    index = np.clip(np.searchsorted(E4_VALUES, a), 1, len(E4_VALUES) - 1)
    low, high = E4_VALUES[index - 1], E4_VALUES[index]
    nearest = np.where(np.abs(a - low) <= np.abs(high - a), index - 1, index).astype(np.int64)
    return (E4_CODES[nearest] | np.where(x < 0, 0x80, 0)).astype(np.uint8)


def e2_encode(v):
    """Nearest E2M1 nibble of v (sign included)."""
    v = np.asarray(v, dtype=np.float64)
    index = np.abs(np.abs(v)[..., None] - E2M1).argmin(axis=-1)
    return (index | np.where(v < 0, 8, 0)).astype(np.uint8)


def deswizzle(blob, off, N, K):
    """The runtime's FP8 matrix [N][K] as raw byte codes."""
    buffer = blob if isinstance(blob, np.ndarray) else np.frombuffer(blob, dtype=np.uint8)
    n = np.arange(N, dtype=np.int64)[:, None]
    k = np.arange(K, dtype=np.int64)[None, :]
    kk = k & 31
    return buffer[off + (k >> 5) * (N // 16) * 512 + (n >> 4) * 512
                  + (4 * (n & 7) + ((kk >> 1) & 3)) * 16 + ((n >> 3) & 1) * 8
                  + 2 * (kk >> 3) + (k & 1)]


def matrix(blob, off, N, K):
    """The runtime's FP8 matrix [N][K] as float64, NaN codes as zero."""
    return np.nan_to_num(E4[deswizzle(blob, off, N, K)], nan=0.0)


def masks(N, K):
    """Where the nibble and the block scale of [N][K] sit in a 4-bit asset."""
    n = np.arange(N, dtype=np.int64)[:, None]
    k = np.arange(K, dtype=np.int64)[None, :]
    n64 = n & 63
    block = ((n >> 7) * (K // 64) + (k >> 6)) * 4608 + ((n >> 6) & 1) * 2304
    nibble = (block + (n64 >> 4) * 512
              + (4 * (n64 & 7) + ((k & 31) >> 3)) * 16
              + (((n64 >> 3) & 1) * 2 + ((k & 63) >> 5)) * 4 + ((k & 7) >> 1))
    scale = block + 2048 + ((n64 & 7) * 8 + (n64 >> 3)) * 4 + ((k >> 4) & 3)
    return nibble, scale


def unpack4(raw, off, N, K, out):
    """Rebuild a float matrix from a 4-bit asset block: codes * block scale * out."""
    record = raw if isinstance(raw, np.ndarray) else np.frombuffer(raw, dtype=np.uint8)
    nibble, scale = masks(N, K)
    packed = record[off + nibble[:, 0::2]]
    codes = np.empty((N, K), dtype=np.uint8)
    codes[:, 0::2] = packed & 0x0F
    codes[:, 1::2] = (packed >> 4) & 0x0F
    values = E2M1[codes & 7] * np.where(codes & 8, -1.0, 1.0)
    return values * np.repeat(E4[record[off + scale[:, 0::16]]] * out, 16, axis=1)
