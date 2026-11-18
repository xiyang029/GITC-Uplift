# How Model B's feed-forward became a local 4-bit model

Preset `NVFP4: B feed-forward` (17) runs a Model B pass with exactly one learned box replaced by our own NVFP4
kernels: the transformer feed-forward (ViT blocks 11-18's `FfnExpand` / `FfnContract`). Everything else in
that pass -- fused Swin, split-Swin, attention, projection -- stays the runtime's own. Upstream does the same
and says so in its README: "a Model B pass follows the transformer feed-forward box alone, the only part that
exists for Model B".

This file records where those 4-bit assets came from and what the two scripts in `tools\nvfp4\` do. Model A's
folders (`modelA`, `swin4`, `split4`, `vit4`, `prepost4`) are not reconstructible this way and were not
touched.

## 1. Where the weights come from

Both models' weights are PE resources of the same DLL, `C:\Optiscaler\nvngx_dlssnr.dll` (310.9.16):

| resource | model | size | records |
| --- | --- | --- | --- |
| `WEIGHTS_HT` | A | 147,695,410 B | one per layer, e.g. `block31.layer0.layer` |
| `WEIGHTS_CC_SMALL_FP8` | B | 113,893,343 B | 77, e.g. `block11.layer0.layer` |

A record is a length-prefixed name, a length-prefixed body, then a 16-byte footer of `(type, layout, ndim,
dims[])`. `dims[0]` is `bytes / 2` (a uint16 element count), **not** a channel count. Two records per block
matter:

- `layer0` -- 4,194,320 B = `4096*1024` + 16 of padding: the expand matrix `[N=4096][K=1024]`.
- `layer1` -- 4,196,352 B = `1024*4096` + 2,048: the contract matrix `[N=1024][K=4096]` plus 1,024 fp16
  channel gains (the same place Model A's `gain` came from).

The bodies are FP8 E4M3 **in the runtime's own swizzle**, which is why they cannot be fed to a GEMM directly.

## 2. The three layouts

None of this is published. Each was recovered by matching candidate byte orders against a second, independent
description of the same matrix, and kept only when the match was near-perfect.

### 2.1 runtime FP8 -> canonical `[N][K]` E4M3 (`deswizzle`)

One 512-byte slice holds 16 output channels x 32 K; slices run N-fastest inside each K group of 32:

~~~
kk   = k & 31
sl   = (k >> 5) * (N // 16) + (n >> 4)          # slice, N fastest
lane = 4 * (n & 7) + ((kk >> 1) & 3)            # 16-byte lane
byte = 2 * (kk >> 3) + (k & 1)                  # byte in the lane, K parity innermost
src  = base + sl * 512 + lane * 16 + ((n >> 3) & 1) * 8 + byte
~~~

### 2.2 canonical -> `ffn_v2` file layout (`masks`)

The 4-bit file is a fixed permutation of the same matrix: nibbles first, one E4M3 group scale per 16 K. The
unit is a 4,608-byte block of two 2,304-byte halves (chosen by `(n >> 6) & 1`); each half is 2,048 B of codes
for a 64x64 `(n, k)` tile plus 256 B of scales (4 per output row x 64 rows):

~~~
n64 = n & 63
blk = ((n >> 7) * (K // 64) + (k >> 6)) * 4608 + ((n >> 6) & 1) * 2304
nb  = blk + (n64 >> 4) * 512
          + (4 * (n64 & 7) + ((k & 31) >> 3)) * 16
          + (((n64 >> 3) & 1) * 2 + ((k & 63) >> 5)) * 4
          + ((k & 7) >> 1)                        # packed nibble byte
sb  = blk + 2048 + ((n64 & 7) * 8 + (n64 >> 3)) * 4 + ((k >> 4) & 3)   # scale byte
~~~

Write `nb` for even `k` only, one nibble per k: even k low nibble, odd k high nibble. Both `[4096][1024]` and
`[1024][4096]` come to exactly 2,359,296 bytes, which is the invariant `NrFfnV2.cpp:46` encodes.

### 2.3 the two per-block output scales

Each manifest `block` line ends with `out1` / `out2`, one float per matrix as 8 hex digits. **The hex is a
big-endian bit pattern** (`struct.pack(">f", value).hex()`); the C++ side reads it back with `FloatFromBits`
(`NrFfnV2.cpp:225-229`). Read as little-endian it yields 4.38e-42, a mistake made twice while deriving this.

`out` is the matrix's outer scale, applied after the per-16-K group scales. NVIDIA publishes no rule, only a
band: under Model A's factory values the reconstructed scale codes reach `imax = 136.53 .. 253.56` with zero
high clamps, i.e. the largest group sits just under E4M3's 448 ceiling. So the rule is `pick_out(W) = amax(W)
/ (6.0
* 175.5)`.

## 3. Quantisation

The kernels are the shipped `ffn08_k4peek.cubin`, unchanged -- only the weights differ, so the *encoding* has
to be exactly what they expect. `swin4.cu`'s `block_scale()` states it: `fminf(fmaxf(amax *
0.16666667163372040f, 0.001953125f), 448.0f)`.

- scale per **16** consecutive K: `amax = |W| reshaped [N][K/16][16] max over the last axis`.
- raw scale = `clip(amax / (6 * out), 2^-9, 448)`, rounded to the nearest of the **127 non-NaN** E4M3 positive
  codes, then sign-folded.
- weights = nearest of E2M1 `{0, .5, 1, 1.5, 2, 3, 4, 6}` after `W / (scale * out)`; sign in bit 3.

Two traps, both of which silently produced garbage:

1. `np.argmin` over an E4M3 table that still contains NaN returns index 127 (the NaN code) for every input. The
   tables must enumerate `range(0, 127)` only.
2. Rounding must use the *decoded* scale (the E4M3 code read back), not the raw value, or the nibble indices stop
   lining up with what the kernel multiplies.

## 4. The two scripts

Nothing else is needed beyond numpy and the DLL.

### 4.1 `tools\nvfp4\nvfp4_layout.py` -- the layouts as code

Pure arithmetic on bytes, no network-specific constant anywhere in it:

| function | what it does |
| --- | --- |
| `resources(data)` | parses a PE32+ resource table by hand (machine `0x8664`, magic `0x20b`, type `RCDATA`), returning `{name: payload}` -- no `pefile` |
| `records(blob)` | walks one weight resource into `name / off / bytes / dims / type / layout`, offsets relative to the blob; asserts the blob's own length, so a wrong base fails loudly |
| `deswizzle` / `matrix` | section 2.1: byte codes, then the E4M3 decode to float64 (NaN code -> 0) |
| `masks(N, K)` | section 2.2: for every `(n, k)`, the nibble byte and the scale byte |
| `unpack4(raw, off, N, K, out)` | the inverse of `masks` + quantisation: rebuilds the matrix a kernel would see |
| `e4_encode` / `e2_encode` + tables | the two code tables; `E4_VALUES` / `E4_CODES` cover `range(0, 127)` only, i.e. skip the NaN code |

### 4.2 `tools\nvfp4\make_ffn_assets.py` -- the builder

~~~
%PY% tools\nvfp4\make_ffn_assets.py --model B --output nvfp4-out --check C:\Optiscaler\nvfp4\modelA
~~~

`--dll` defaults to the runtime above, `--model` picks one row of `MODELS`, `--output` is where the two asset
files go, `--check` adds the verification below.

| model | resource | runtime blocks | deployed as |
| --- | --- | --- | --- |
| `A` | `WEIGHTS_HT` | `block31` .. `block38` | `nvfp4\modelA` |
| `B` | `WEIGHTS_CC_SMALL_FP8` | `block11` .. `block18` | `nvfp4\modelB` |

In order: `read_block()` takes `layer0` as `[4096][1024]`, `layer1` as `[1024][4096]` and the 1,024 fp16 gains
off its tail (record sizes asserted -- expand 4,194,304 or 4,194,320, the 16 extra bytes being padding, and
exactly 4,196,352 for contract -- so a differently-shaped block stops the run instead of writing a bad asset);
`choose_out()` picks the outer scale; `quantise()` returns one 2,359,296-byte matrix plus the statistics it
prints; eight blocks are assembled at `STRIDE = 4,720,640` (`2,359,296 + 2,359,296 + 2,048`), hashed, written
as `ffn_v2_trained.bin`; then `ffn_v2.txt` is written with `kernels stages 2 steps 2`, eight `block` lines
(`w1` at `i * STRIDE`, `w2` at +2,359,296, `gain` at +4,718,592, `tx` / `th` both `0x3f800000` = 1.0, the two
big-endian `out` patterns), `bytes 37765120 sha256 ...` and `small_direct max_tokens 640 file
ffn08_k4peek.cubin`.

The `_trained` name matters: `NrFfnV2.cpp:260-261` sets `trainedIsFirst` when a folder has
`ffn_v2_trained.bin` and no plain `ffn_v2.bin`, loading it as the top of the weight buffer (set 1). Model A's
folder is laid out the same way, which is why the builder writes that name even though it is the folder's only
set.

### 4.3 the check that proves the layouts

`--check` never touches Model B. It reads **Model A's factory** `nvfp4\modelA\ffn_v2_trained.bin`, rebuilds
each matrix from the file's own offsets and `out` values with `unpack4`, reads the same matrix out of
`WEIGHTS_HT` with `deswizzle`, and correlates. All 16 matrices land in `corr 0.995644 .. 0.996433`, `relerr
0.08439 .. 0.09326`, with the 4-bit and FP8 `amax` agreeing to ~1% (e.g. `0.56396 / 0.56250`). A wrong layout
would give noise.

0.9956 at 0.092 relative error is E2M1's own noise floor -- the ceiling, not a shortfall. The same run settles
the pairing: **factory file block `i` is runtime block `31 + i`**, which is why `--model B` writes `11`..`18`.

`--model A` (rebuilding A's file rather than reading NVIDIA's) gives `corr 0.99549 .. 0.99585`, the same band,
but a **different file**: sha256 `54f93c03...` against the factory `ead29390...`. That is expected and costs
nothing, since `out` only has to land in the headroom band and NVIDIA's own choices are coarser (every factory
`out` is a short binary fraction, e.g. `0.0005340576171875 = 1.09375 * 2^-11`, each within a factor of 1.8 of
ours). Only Model B's output has a specific expected content -- the file in section 5 -- and the builder
reproduces it byte for byte.

`descriptor.json` / `descriptor-small.json` (dumps of the runtime's network descriptors taken with the
upstream capture tool) were an input, not an output: they said which block is the ViT and what `N` / `K` are.
Those numbers are constants in the builder now, so a descriptor is only needed again if a future runtime
reshapes the FFN.

## 5. What came out

`ffn_v2_trained.bin` as the builder writes it: **37,765,120 bytes**, sha256
`eddc2881f07ea417afc4fcea718d90088fb64f0bd7a030e47710c7bcb7c1479d`.

| blk | runtime | W1 amax | out1 | W1 corr | W2 amax | out2 | W2 corr |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | 11 | 0.6250 | 5.9354e-4 | 0.995656 | 0.1094 | 1.0387e-4 | 0.995894 |
| 1 | 12 | 0.4688 | 4.4516e-4 | 0.995591 | 0.1562 | 1.4839e-4 | 0.995815 |
| 2 | 13 | 0.4062 | 3.8580e-4 | 0.995499 | 0.1406 | 1.3355e-4 | 0.995639 |
| 3 | 14 | 0.4375 | 4.1548e-4 | 0.995689 | 0.1406 | 1.3355e-4 | 0.995634 |
| 4 | 15 | 0.2812 | 2.6709e-4 | 0.995522 | 0.1250 | 1.1871e-4 | 0.995680 |
| 5 | 16 | 0.3438 | 3.2645e-4 | 0.995611 | 0.1094 | 1.0387e-4 | 0.995834 |
| 6 | 17 | 0.3750 | 3.5613e-4 | 0.995504 | 0.1094 | 1.0387e-4 | 0.995821 |
| 7 | 18 | 0.4375 | 4.1548e-4 | 0.995688 | 0.1406 | 1.3355e-4 | 0.995595 |

`relerr` stays in 0.0918 .. 0.0959; gains span roughly 0.23 .. 1.00 per block. High clamps are zero
everywhere; the only low clamps are all-zero 16-K groups in W2 (4, 4 and 901 of 262,144 in blocks 4, 5 and 7),
quantised to the 2^-9 floor exactly as Model A's factory file does (A has 7..773 of them). Scale codes span
2.25 .. 176.0.

## 6. Deploying it

Three files in `C:\Optiscaler\nvfp4\modelB\`. The add-on resolves `nvfp4` under its own asset directory
(`src/addon/nvfp4_hook.cpp:91`), so the folder sits beside the runtime and next to `modelA`:

| file | size | note |
| --- | --- | --- |
| `ffn_v2_trained.bin` | 37,765,120 | the file above |
| `ffn_v2.txt` | 999 | the manifest above |
| `ffn08_k4peek.cubin` | 140,128 | **Model A's, copied unchanged** -- same kernel, different weights |

No Model A file was modified and no other folder is required: `NrFfnV2.cpp:768` decides "this model has 4-bit
files here" by the existence of `<folder>/ffn_v2.txt`, and `NrFfnV2.cpp:562` tells A from B by the launch
count up to the first expand (`58` vs `20`).

Menu: `NVFP4: B feed-forward`. The path, in order -- `src/ui/settings_schema.cpp:17-25` lists the preset (17)
next to `NVFP4` (4), `NVFP4: 2x in one` (16) and `NVFP4: 3x in one` (15); `src/nr/feature.cpp:44` asks the
runtime for **preset 2**, not 1 (getting this wrong is silent: the pass runs Model A and the FFN kernels are
never seen, launch 58 instead of 20); `src/nvfp4/NrNvfp4.cpp:236` sets `gFeedForwardOnly`, `:240-243` picks
the weight set, `:250-253` checks only `NrFfnV2::SetComplete(kDefaultSet, &file, 1)` (model index 1 =
`modelB`), `:132-135` pauses Swin and Net through `PauseForEvaluation(paused, feedForwardOnly)`, and the six
"did our kernels run" comparisons at `:289-299` are guarded by `!gFeedForwardOnly` so a Model B pass is not
failed for letting the rest of the network through.

Expected `ReShade.log` line:

~~~
running: this pass is Model B, its feed-forward in our 4-bit kernels | active, Model B, 640 tokens, trained weights, small-map contracts 8
~~~

Measured here (RTX 5070 Ti, 1920x1080): one evaluation = 80 launches (79 network + `cg2r_copy_kernel`), of
which **16 are the feed-forward's** -- 8 expands (one `..._ffn_expand_publish_fp8`, seven
`..._ffn_expand_chained_fp8`) and 8 `..._ffn_contract_chained_fp8`. The pass reports
`hnet-crazy-cuckoo-small-fp8`, `raw_tensors=77`, `total_aligned=113,907,200 bytes`. Upstream measured this
box's gain at 0.13-0.14 ms per pass, against 0.05-0.12 for the same box on Model A -- the reason the cost of
going further is poor.

## 7. Why this stops at the feed-forward

The same three layouts were **not** recovered for the fused Swin, split-Swin, ViT and prepost families, so
those stay the runtime's. The blocking evidence, in short:

- Three independent oracles for "is this the right 4-bit mapping" all failed to separate right from wrong: our
  encoder's invariant ("every group of 16 has max |q| = 6") holds 1.000 for our files but only 0.574 for NVIDIA's;
  the "no group exceeds 6 x scale" violation count is 0 for both right and wrong mappings; the correlation between
  `amax` and the stored scale is -0.47, i.e. negative.
- Exhaustive search over 720 K-bit permutations x 2 polarities x 24 table orders tops out at 0.983 -- close, never
  1.000.
- The decoded 4-bit Swin weights have `absmean 54.04 / absmax 2688.0` against the runtime FP8's
  `0.0827 / 0.75` -- a factor of 650-3600, and `corr 0.0074` against the canonical matrix. That is not a
  permutation error a search can fix; a per-tensor transform is in the way.
- There is no ground truth (NVIDIA ships 4-bit asset *files* for A, and none for B's other families) and no CUDA
  toolchain here to disassemble the cubins for their constants (`nvcc` / `nvdisasm` / `cuobjdump` / `nvrtc`
  all absent, and `nvidia-cuda-nvdisasm-cu12` is not installable).

The remaining upside was estimated at 0.5-1 ms per pass and could only be judged by eye; upstream made the
same call, replacing Model B's feed-forward and nothing else.

## 8. Running the pipeline again

`PY` below is the bundled interpreter
(`C:\Users\31825\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe`, numpy required). One
command rebuilds and re-verifies, two copies deploy:

~~~
%PY% tools\nvfp4\make_ffn_assets.py --model B --output nvfp4-out --check C:\Optiscaler\nvfp4\modelA
copy nvfp4-out\ffn_v2_trained.bin C:\Optiscaler\nvfp4\modelB\
copy nvfp4-out\ffn_v2.txt        C:\Optiscaler\nvfp4\modelB\
~~~

That is all deployment is, on top of Model A's `ffn08_k4peek.cubin` already in that folder. The check must
still print ~0.9956 for Model A, or one of the layouts has changed. The only numbers specific to this network
are the block range (`MODELS`: `A: 31`, `B: 11`) and the matrix shapes (`WIDTH = 1024`, `FFN = 4096`); a
future Model B that reshapes the FFN stops `read_block`'s size assertions rather than producing a wrong asset.
