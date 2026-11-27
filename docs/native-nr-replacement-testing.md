# Native NR replacement checks and same-handle probe

## What is implemented

Replacement logs the effective optional inputs as "Native NR inputs: ..." when
their status changes. Depth, motion, control mask, backbuffer, UI and UI alpha
are reported independently. Reasons include missing input, invalid region,
an alias of the independent output, denied SRV access, panel-disabled motion,
unsupported FOV conversion format, and remap failure.
"Motion=... -> zero-motion" means the existing zero-vector fallback is used.
"game"/"remapped" describes the binding; it is not a claim that the model used it.
An unavailable UI input combination also disables native UI correction.
The ReShade barrier trace is diagnostic only: it does not observe raw D3D12
barrier calls, implicit promotions/decays, or other queues. At the native NGX
evaluate boundary, core and optional resource states follow the DLSS-NR input
contract (shader-read inputs and UAV output); regions and resource capabilities
are still checked before replacing the evaluate. Color/Output aliasing uses the
Output UAV contract, with the prepass transitioning the shared texture for its read.

The regression uses an identity evaluator on a real D3D12 device. It checks
separate and overlapping Color/Output regions, state restoration, failure
handling, and missing/stale optional inputs. The FOV oracle uses patterned depth,
masks, independent backbuffer/UI/alpha images and nonzero signed motion scales.
It checks mapped sample locations and previous-frame motion endpoints against a
double-precision density-integral inverse solved with bisection, including
off-screen endpoints. This proves GPU plumbing, not neural-model image quality.

## Build and run

Use PowerShell 7 from the repository root. After the normal release build,
enter the Visual Studio x64 developer environment and run:

~~~powershell
cmake --build build/release --target uplift_native_nr_regression uplift_native_nr_handle_probe
.\build\release\uplift_native_nr_regression.exe
.\build\release\uplift_native_nr_handle_probe.exe --self-test --frames 30
~~~

The probe's self-test uses GPU copies instead of NR. Its timing numbers measure
the fixture only and must never be presented as DLSS-NR performance results.

## What the real-runtime probe establishes

The probe creates **one** native feature at 1280x720, then evaluates it at
1280x720, 640x360 with separate Color/Output, 640x360 with aliased Color/Output,
and 1280x720 again. The handle and its create-time Width/Height remain unchanged.
A call-local parameter view overrides Color, Output, motion, their regions and
Reset; all other reads reach the original parameter block. Neither the original
block nor its unknown keys are overwritten.

The pre-pass converts a synthetic linear image to the existing model domain;
the post-pass extracts NR's change and upsamples/composes it back onto the
original image. No FOV, game hook, multi-pass or UI mode is added by this probe.
Each phase resets history once and discards four warm-up frames. GPU timestamps
report median pre/NR/post/total and p95 total milliseconds; available native
allocation statistics are also printed. This isolated benchmark submits and
waits for each sample, so it does not measure in-game queueing or input latency.

Supply an existing compatible runtime, and an NGX core if required:

~~~powershell
.\build\release\uplift_native_nr_handle_probe.exe --runtime 'D:\path\nvngx_dlssnr.dll' --core 'D:\path\_nvngx.dll' --frames 60
~~~

No runtime DLL is downloaded or assumed to be present. A missing or incompatible
runtime is a failed prerequisite, not a successful native test. A GPU error,
failed evaluate, constant/untouched or non-finite model output fails the probe.
Even a real-runtime pass only establishes this runtime/feature/configuration's
same-handle size and Color/Output-alias behavior: it does not validate a
game-owned handle or prove visual parity, arbitrary dynamic sizes, FOV support,
or a latency advantage.

Do not infer lower cost from smaller bound textures alone. Compare the NR and
total columns, repeat the run, and inspect the full-return phase for state/history
problems. The full-size total also includes this tool's wrapper: compare the
small-size total against full-size NR alone when estimating whether NR savings
could pay for the wrapper. The game's actual colour preparation can differ.

The panel's merged native option uses panel controls for the game's DLSS-NR and
applies the selected work resolution around that same game-owned handle when its
resource shapes, regions and formats are supported. Unsupported inputs leave the
native evaluate unchanged while parameter overrides remain active. A real-runtime
pass does not replace the required game-owned-handle and in-game validation.

## Game comparison still required

Compare original NR, replacement at full size, replacement at a fixed smaller
size, then replacement with FOV. Start with one pass, no extra face pass, and
matched runtime/model/settings where possible. Record which inputs actually
reached NR. Test SDR/HDR, camera and object motion, occlusion reveal, thin
geometry, UI transparency, resolution/FOV changes, pause/resume and scene cuts.
Use matched, warmed sequences; compare temporal artifacts, not screenshots alone.
Record GPU stage/total timings, frame-time distribution and memory. Measure
end-to-end input latency separately before making an input-latency claim.
