# NR pipeline correctness and high-ROI work — 2026-11-21

Scope: the existing DX12 native, DX11→DX12 bridge, Vulkan native/bridge,
helper IPC and NVFP4 resource paths. Changes preserve model kernels, weights,
precision, pass settings and the protected XMX upstream/Intel ABI. This is an
implementation and focused regression report, not a claim of measured FPS or
latency gains in a game.

## Correctness changes

| Area | Implemented change | Validation / boundary |
| --- | --- | --- |
| DX12 depth capture | Extract the actual depth subrect through a typed SRV into R32F; reject MSAA, unreadable formats and DENY_SHADER_RESOURCE instead of issuing an illegal partial depth copy or inventing a COMMON state. | D32, D16 and D24S8 GPU extraction, nonzero origin, D3D12 validation. |
| Present input provenance | Depth/MV require producer submission proof; a producer submitted in an earlier frame on the consumer's own queue (the native call returned by then), a stamped same-queue submission, or completed foreign-queue work counts. Depth direction comes from the captured producer and depth pairs with matching-frame inputs. Replaying a recording invalidates its old producer proof even after the original token was pruned. | Fake timeline/proof cases cover submit/stamp, cross-queue completion, discard and replay; real game capture remains necessary. |
| Foveated input | Do not reuse unmapped depth or an old DLSS motion copy for a foveated frame; try the current Launchpad source where supported. | Current-frame selection and source-path checks; foveated game testing remains necessary. |
| Skips/history | Check the effective pass slice and its own controls, budget, zero intensity and pending reset before expensive work; a later active pass is no longer skipped because pass 1 is zero. Reset both Look histories when mapping changes. | Slice/control regression; Vulkan Present/After/Pre and DX12 entry-point checks. |
| Executable recordings | Keep resources alive after the first GPU completion until the game resets/destroys the executable list. Time alone never retires a Vulkan event. Do not host-reset an event on replay. | Lifetime, discard and reset/completion-order regressions. |
| Descriptor/query reuse | Separate execution completion from recording references. A slot checks its own recording owner; the ring searches past pinned slots. Timing can collect a completed sample while its executable list still owns the queries. | Regression retains an earlier completed executable list, reuses unrelated slots and checks reset-before-completion. |
| Vulkan synchronization | GPU queue ordering uses a producer/consumer queue-pair timeline semaphore; retirement is sealed on the matching consumer. Failed signals never enqueue an unsatisfiable wait. | Real dual-queue submissions, reversed direction, wrong-consumer retirement, failed signal/wait, CPU fallback, synchronization validation. |
| Helper publication | Fill status and fence values before arming the GPU completion reply event. A resize/disable invalidates combined RUN before its input fence is consumed. | Real helper process, GPU debug layer, two deliberately unsignalled invalidated handoffs. |

Resource retirement remains deliberately conservative across cumulative timeline
marks. An executable list that a game retains without Reset can keep retired
resources alive. The descriptor-ring fix does not weaken that resource protection.
Unsubmitted earlier recordings can also conservatively delay reuse until their
submission or discard is observed.

## Implemented high-ROI optimizations

| Priority | Change | Work / storage removed | Compatibility rule |
| --- | --- | --- | --- |
| 1 | Skip inactive effective slices before copies and allocations. | Avoid unused capture, conversions and model work on those frames. | Preserve show-mask behavior and owe a temporal reset after skipping. |
| 2 | DX12 After DLSS compose directly into the game UAV. | Remove a source-copy allocation and copy when the source can be read through its UAV. | Require compatible single-subresource format and typed UAV-load support; otherwise keep copy path. |
| 3 | DX11 Before upscaling decode into the shared UAV and sample the shared mask directly. | Remove the private output copy and redundant mask copy. | Shared-UAV creation failure keeps the existing render-target/private-copy path. |
| 4 | Vulkan FP16 Present uses copy directly. | Remove the staging image and both format blits. | Applicable to the FP16 copy format. |
| 5 | Vulkan known-format integer/FP32 Present blits directly between the game image and FP16 work image. | Remove staging and two full-image copies while retaining the two original FP16 conversions. | Require the known actual image format to equal the copy format. sRGB/unknown mutable format keeps raw-copy staging. |
| 6 | Fuse Change into Compose on compatible full-resolution paths. | Remove the standalone Change dispatch and its image. | Preserve or reconstruct the original FP16-rounded model input; reduced/foveated/Look paths retain independent Change when needed. |
| 7 | Allocate zero motion only when needed; clear it in the encode dispatch. | Avoid unused motion storage and a separate clear pass. | Do not trust a CPU cleared flag for a discarded/replayed recording. |
| 8 | Reuse intermediate images independently by their actual dimensions/format/kind. | Avoid recreating model A/B, source copy, private output and motion images when only an unrelated resource changes. | Create replacements before committing; failed allocation preserves the usable set. |
| 9 | Store only the two low-history levels that Look stabilization reads. | Reduce persistent history-image payload. | Preserve the selected level sizes and original FP16 operations. |
| 10 | Retire tracked NVFP4 scratch safely; retain current and one recent spare size. | Remove the previous six-size cache retention for recording-tracked sessions. | Recording/execution leases retain in-flight scratch; untracked callers keep the original six-size policy. No kernel changes. |
| 11 | Batch DX12 input capture and After DLSS NR. | One recording token and one final compute-state replay for adjacent work. | Preserve tokens on skips/exceptions; restore game state at scope exit. |
| 12 | Replace the normal Vulkan cross-queue CPU wait with GPU Signal/Wait. | Remove the CPU fence wait on supported native queues. | Same queue needs no extra submission. Unknown queues or unsupported timelines retain the capped CPU fallback. |
| 13 | Combine steady FENCED helper FRAME + RUN. | One IPC request/reply exchange per eligible frame instead of two. | First handshake, settings/target/mask changes, retries/drains and late replies keep the normal path; FinishPresent sends FRAME if Run is not reached. |

For scale, a tightly packed 3840×2160 image is 31.64 MiB at 4 bytes/pixel,
63.28 MiB at 8 bytes/pixel or 126.56 MiB at 16 bytes/pixel. These are logical
image payloads, not measured committed-VRAM savings: allocation alignment, old
in-flight recordings and driver behavior affect the actual peak.

## Diagnostics

- DX12 overlay-requested samples expose copy/remap, encode/meter, model
  evaluation, compose/Look and total GPU time. The panel's NR GPU time row shows
  the evaluation and the whole chain side by side ("8.10 · 8.92 ms"); the
  TRACE breakdown still names every part. Samples are collected after completion
  without a wait;
  failed recordings never resolve unwritten queries. TRACE prints the breakdown
  at most once per second when samples are requested.
- TRACE-only client counters report FRAME/RUN/FRAME_RUN CPU request round trips,
  calls, pending/gone outcomes, mean/max and cumulative busy frames in aggregated
  windows. With TRACE disabled the extra clocks and aggregation are skipped.
- CPU IPC round trip and same-queue GPU total are different measurements. Neither
  is input-to-display latency or total frame latency.

## Focused regressions

The regression executables are EXCLUDE_FROM_ALL. They never replace the production
NR host or ship in the ordinary release target. GPU cases ran on NVIDIA GeForce
RTX 5070 Ti.

- uplift_nr_regression: recording lifetimes/batching, exception restore, per-slice
  controls, compact history sizes; 256 bit-exact DX12 fusion/in-place cases across
  8/10/16/32-bit outputs, SDR/HDR10/scRGB/linear, exposure, alpha, mask and
  nonzero subrect; 16 bit-exact full-atlas/compact Look history comparisons
  covering radius, motion reprojection, reset and shaped output; three depth
  formats; producer-proof replay; phase-query completeness. D3D12 debug
  layer checks GPU submissions.
- uplift_nvfp4_scratch_regression: cache eviction, isolated device keys, scope
  deduplication/nesting, retain failure recovery, reset/completion order and the
  legacy untracked fallback. Does not execute a real NVFP4 model.
- uplift_helper_protocol_regression: real helper child, shared textures/fences,
  96 FENCED/SHARED_CPU handoffs and two invalidated combined handoffs, reply
  publication and D3D12 validation. The helper uses the test-only identity host.
- uplift_vk_nr_regression: 32 bit-exact fusion cases; actual production Present
  staging/direct comparisons in RGBA8/RGB10A2/RGBA16F/RGBA32F with held-byte
  checks; dual-queue ordering and failure branches. Vulkan synchronization
  validation is enabled. The NR host is test-only identity.

Build release with `build.cmd release`. To build focused regressions, enter the
VS x64 environment, configure the release preset, then run:

```powershell
cmake --preset release
cmake --build --preset release --target uplift_nr_regression uplift_nvfp4_scratch_regression uplift_helper_protocol_regression uplift_vk_nr_regression
& ./build/release/uplift_nr_regression.exe
& ./build/release/uplift_nvfp4_scratch_regression.exe
& ./build/release/uplift_helper_protocol_regression.exe ./build/release/uplift_helper64_identity_regression.exe
& ./build/release/uplift_vk_nr_regression.exe
```

The localized `/showIncludes` header-dependency trap still applies: touch affected
consumer .cpp files before rebuilding after header-only edits. Final verification
must rebuild all affected consumers, not reuse stale objects.

IPC protocol is now version 16. Deploy the matching add-on and helper together;
mixing a version-15 helper with this add-on is unsupported.

Local verification completed with exit code 0 for the full release rebuild,
release-x86 build, four regression executables and git diff --check. The protected
src/xmx/vendor tree and src/xmx/generator.hpp have no diff. Logs are under
build/release: nr-final-release-build.log, nr-final-x86-build.log,
nr-final-regression.log, nvfp4-final-regression.log, helper-final-regression.log
and vk-final-regression.log.

## Remaining validation boundary

No real-game/real-NR-model before/after performance capture or temporal visual
comparison has been performed for this change set. Pixel equivalence validates
the listed color/composition paths, not every vendor model or all content.
DX11 shared-UAV fallback, real game command-list replay, device recreation,
multiple queue families and unusual mutable/sRGB swap chains need game/driver
coverage. No claimed millisecond, FPS, or peak-VRAM improvement is inferred from
the microtests. Changes to model precision, resolution, pass count or the
read-only Intel ABI are outside this quality-preserving optimization scope.

## Quality follow-up

Game resets now pass through on both native backends; the 250 ms suppression
could discard a second independent cut. (The D3D11 bridge has no game-reset path
of its own, so nothing there changed.) Resolved colour
primaries now invalidate each stage's model/Look history alongside encoding and
input scale. Equivalent Auto/BT.709 settings preserve history. The D3D12 and
Vulkan identity-runtime regressions cover basis changes and consecutive resets.

Two shader alternatives were tested in isolation on the RTX 5070 Ti; neither
probe changed a production shader. Per-tap Look rejection fixes a synthetic guide
cancellation case, but three repeated 1080p flat-input runs cost 3.4-3.5% more
GPU time for that pass (about 1 microsecond). Direction-preserving change clipping
cost 5.7-5.9% more (about 1.4 microseconds). These full-resolution kernel probes
are not game or whole-chain timings; neither alternative is included. Probe
sources and logs are under ignored build/nr-quality. MV jitter conventions for
native NR before/after SR remain unverified; no blanket correction was applied.
