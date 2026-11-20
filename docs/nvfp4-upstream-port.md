# Porting the NVFP4 / DLSSNR half from OptiScaler-DLSSNR-PreSR-Multipass

One upstream for the 4-bit NR half: `github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass`, the `v0.9.1` line.
The local port (commit `aaa57bd3aa0fad46e98d908e4db3752ad6121d56`) was made against upstream
`8bc0978430a9406da1a46668dd6046637c88412f`. Each sweep is recorded here: what was taken, what was left, why.

## Sweep 2026-11-18: `8bc097843` -> `d3299ce5` (the v0.9.1 tip)

`d3299ce5b28421df12ac1ca268978d459b227057`'s parent is exactly `8bc097843`: it is the immediate next
commit, not a tip several releases ahead. Message: "Fix NR set-up after a discarded command list, buffer
lifetimes and 4-bit model checks". Pure correctness/hardening (+728/-34): **no new model, preset or
user-visible feature**. It has four themes; only ③ and ④ were taken.

### Taken: ③ weight-file completeness (`SetComplete`)

Upstream checks, per evaluation, that every weight file of the set actually used is present before the
pass is allowed to run. Local equivalents:

- `src/nvfp4/NrSwin4.cpp` / `.h`: `bool SetComplete(unsigned set, std::string* missing)` checks
  `s.manifest.loaded`, `n < kMaxSets`, and for a numbered set `chosen.present`,
  `!chosen.fp8.empty()` and a blob (or `trainedIsFirst` for set 1). Reports
  `swin4/swin4.txt`, `swin4/swin4_trained.*` or `swin4/swin4_set<n>.*`.
- `src/nvfp4/NrNet4.cpp` / `.h`: same signature, loops `f < kFamilies` skipping `PrePost` and
  families without `groups`; checks `m.loaded`, `m.trained`/`trainedIsFirst` for the default set,
  and for a numbered set `set < kMaxSets && !m.blob.empty() &&
  s.sets[set].blob[f].size() == m.blob.size()`. Reports `<family>4/<family>4*.bin`.
- `src/nvfp4/NrFfnV2.cpp` / `.h`: same signature; `Enable()` pre-scans `modelA` for
  `ffn_v2_trained.bin` and `ffn_v2_set<n>.bin` (n >= 2) into `State::trainedFile` / `setFile`;
  `SetComplete()` reads those. Reports `modelA/ffn_v2_*.bin` when `modelFolders` is on.

### Taken: ④ mixed-model / fault detection (`Faults`)

The pre-existing "our kernels did not run" test cannot see "our kernels ran with another set's weights".
Each family now counts the latter and exposes `unsigned long long Faults()`:

- FFN: `State::faults` (failed pairs and set-selection fallbacks), with `SetAt(State&,
  const ModelWeights&, const Options&)` replacing both inline set-selection expressions in
  `RecordExpand` / `RecordContract`; `++s.faults` at the top of `Fail()`;
  `Faults() = s.faults + s.passedThrough`.
- Net: `State::substituted`, incremented in `Replace()` when a launch got records of another set
  (or trained records with none present); `Faults() = s.substituted + sum(s.passed)`.
- Swin: `State::ioPassed`, incremented on the input/output-block path when enabled and not paused;
  `Faults() = s.passed + s.ioPassed`.

Host wiring in `src/nvfp4/NrNvfp4.cpp`: `Work` gains `swinFaults/netFaults/ffnFaults`, read in
`Read()`; `BeginEvaluation()` now **chooses the set first** (Swin's `UseSet` returns the
effective set, passed to Net and FFN), then checks all three `SetComplete()` calls, then
`PauseAll(!gOurs || !gSetMissing.empty())`; `OursRan()` adds three comparisons against `gBefore`
after the existing "nothing ran" tests, reporting "part of this pass ran with the runtime's own
weights (fused blocks: ...)", or "part of this pass ran without our kernels or weights (split blocks
and transformer: ... / feed-forward: ...)".

### Not ported: ① command-list / lifetime rework

Upstream's Proxy / GpuLifetime / NrPassBorrow changes address a discarded-command-list setup and object
lifetimes in its own proxy layer. This port hooks NvAPI with MinHook and brackets each evaluation in
`src/nr/feature.cpp`; it has no Proxy or GpuLifetime to rework. No NGX-layer equivalent is missing
here, so nothing was taken.

### Deferred: ② FFN idle-buffer retirement

Upstream raises `kMaxSessions` 6 -> 8 and adds `RetireIdleSizes` / `FreeRetired` to release
buffers of resolutions no longer used. Skipped for now: the local `kMaxSessions = 6` already caps the
held buffers at roughly 6 x 50 MB at 4K, and upstream's retirement is tied to ①'s lifetime rework.
Porting it needs a new cross-layer call wired into `src/bridge/d3d12_side.cpp:279` (`D3D12Side::Retire`)
and risks freeing buffers still referenced by recorded command lists. Revisit only if measured VRAM
growth from resolution switching justifies it; **do not change the held-session cap as part of ③④**.

## Verification

Pass condition: `./build.cmd release` exits 0 and `build/release/gitc-uplift.addon64` is newer
than the edit (the addon lives under `build/release/`, not the repository root). This toolchain's
`cl` prints `/showIncludes` in localized form, so Ninja records no header dependencies: after a
header-only edit, touch a `.cpp` of the same target before building or the build proves nothing.
All edits above were made with release exits 0 and a fresh addon; the only warnings are pre-existing
C4996 (`_wgetenv` / `getenv`). `build.cmd`'s working-tree modification is pre-existing and unrelated.

## Against upstream

The comparison bases are the two shas at the top of this file. No other upstream commit has been
reviewed for this half yet; the next sweep should start at `d3299ce5` and record shas the same way.

## Sweep 2026-11-20: `fca8f4c1` (FP8 NR models) and `e468b65d` (presets / calibrator)

Selective port of `fca8f4c19307d32983b71d703b416ee4294ced55` from `codex/standalone-current`.

- `NrFp8` wraps NvAPI outside the 4-bit layers to see the original launch sequence. It uses NVIDIA's FP8 CUDA-on-D3D12 kernels (RTX 40/50) with weights from `nvfp4/fp8/fp8_set<n>.txt` / `.bin`. Assets are runtime-only. Missing assets or a launch mismatch refuse the pass without falling back or disabling the 4-bit models.
- Local presets **18-21** select FP8 sets 1-4 (2x, 3x, 2x v2, 3x v2) on runtime Model A. Upstream uses 17-20, but local **17** remains Model B feed-forward on runtime Model B. FP8 has no Vulkan path.
- `uplift_nr` builds `NrFp8.cpp` with the existing libraries. The settings schema exposes all four FP8 choices.

Not ported from `e468b65dd68051fa896738d8874688c770e41703`:

- The slider calibrator needs held-frame trials and NR-output readback; comparing successive game frames would not measure the same input.
- Named presets need a local settings snapshot and UI. Upstream's `Config` has fields with no local equivalent.
