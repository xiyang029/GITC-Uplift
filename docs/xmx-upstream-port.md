# Porting XMX from MFGAdaUnlock-RenoDx

One upstream for the XMX half: `github.com/mavismmg/MFGAdaUnlock-RenoDx`. The port started from `0d55206`;
the comparison base is `8b854ff` (the 1.10.1 source; everything after it is README/CI -- see the
2026-11-22 sweep). Each sweep is recorded here: what was taken, what was left, why.

## Sweep 2026-11-06: `3b8889f` -> `efc1fe7` (1.8.4, 35 commits)

`main` and `develop` were both at `efc1fe7` (the "compare `develop`, `main` only takes README edits" rule
died with 1.8.3). **The ABI layer did not move**: nothing under `xemfg/vendor/`, and `xmx_generator.hpp`'s
`InitParams`/`Input`/`FrameData`/`ExecuteParams` plus their five `static_assert`s are byte-identical.
Everything below is outside that block. All 35 commits are accounted for.

### Taken (4)

- `9f0a24e` -- a copy on a compute list whose source is in a graphics state uses
  `NON_PIXEL_SHADER_RESOURCE`: `src/xmx/generator.hpp`'s `kGraphicsOnlyStates`, `EffectiveSourceState()`,
  `Capture`.
- `6de60fc` -- an `Init` refusal is remembered per `ContextShape` instead of retried every frame; a shape
  that works clears the failure text: `src/xmx/generator.hpp`'s `State::refused`/`refusedShape`,
  `ClearFailure()`, `Prepare`.
- `f9ced61` -- the log names the `libxess_fg`/`libxell` files that actually ran: `src/xmx/generator.hpp`'s
  `RuntimePath()`, called in `LoadContext`.
- `7dbb337` -- three starts in a row that never reached the clean-exit branch turn XMX off for one start:
  `src/addon/removal_latch.hpp`/`.cpp` (`XmxCrashMarkerPath`, read/write/delete), `src/addon.cpp`
  (`AddonState::safe_mode`, the `AddonInit` latch block, the delete in DllMain).

**`9f0a24e` is half taken on purpose.** Its direct-list half reads a resource's state from `tag_states.hpp`,
a table an `slSetTag` hook fills. Uplift hooks neither Streamline nor the DLSS-G provider -- it observes
NGX's own core (`src/nr/snippet.cpp`) -- so nothing here could fill that table and `tag_states.hpp` does not
exist; a direct list keeps the state its caller declared. Upstream needed it because Streamline 2.14.16
handed its own depth/motion/HUD-less resources over in a non-`COPY_DEST` state, roughly 15,000 debug-layer
errors in 35 s.

**`7dbb337` has no panel here.** Safe mode is silent plus a `nr::Logf(WARN)` line. It keeps its own marker
(`<exe-stem>-<hash>.xmx-crash`, `<pid> <streak>`) beside the placement latch; reaching three consumes the
count so the next start begins from zero, and the ini keeps the user's setting -- `ApplyProcessSettings`
refuses to push an enabled XMX while `AddonState::safe_mode` is set, so the panel switch cannot turn it back
on for that session.

### Nothing to port: the behaviour is already here (2)

- `6de60fc`'s other half, Intel's 2x model when the multi-frame model refuses the game: this port already
  falls back at `src/xmx/generator.hpp:345` ("multi-frame model refused, 2x only").
- `6ea8175` Dynamic MFG open with XMX: the count already comes from the game's `DLSSG.MultiFrameCount`
  (`src/xmx/generator.hpp:764`; `src/xmx/model.hpp:15` says why the 3/4x choice is the game's). Upstream's
  change opened it to RTX 40 -- nothing is gated here.

### Not applicable: this port has no provider-module hooks (6)

`de8de31` (upstream's own 1.8.2 MFG regression, in code this port does not have), `8d83359` (hooks only from
the resident addon copy), `7f7de3b` (NGX preset hook only once resident), `f93dc56` (hook NVIDIA's generator
at DLSS-G on), `cb84564`/`e807f08` (pre-Ada admission by rewriting the provider's architecture answer).
Upstream hooks the provider module and has a ReShade startup copy to reason about; Uplift has neither, so
there is no stale hook to avoid and no provider answer to rewrite. None of the six has an NGX-layer
equivalent missing here.

### Not applicable: decisions this port does not make (2)

- `c2fc9b6` caps the forced multiplier at 2x on Streamline older than 2.7 -- the forced multiplier is not
  this port's mechanism.
- `c485e28` GenericGate byte-scans a provider binary for the Blackwell `0x1b0` gate (exactly one
  `cmp r32,imm32` (`81 F8+r`) plus one `cmp eax,imm32` (`3D`)) -- it patches a provider, not NGX.

### Not code (21)

Game lists, docs, plans, release notes and version strings, plus one README merge -- none of them touch
code. The 21 shas, checked one by one on 2026-11-06: `60150c1`, `4e800fb`, `83092f0`, `7b302e1`, `9c46a11`,
`79b4429`, `a514f0b`, `1f6da81`, `af9343d`, `678f10f`, `08b49a0`, `2ccafb5`, `daba3d9`, `ce51e74`,
`c9377fb`, `4b17340`, `f037a50`, `78b6e6c`, `67a890a`, `d5f9415`, `cd4dff5`.

## Sweep 2026-11-07: `efc1fe7` -> `2928eeb` (1.8.6, 9 commits)

`main` and `develop` both moved to `2928eeb`. **The ABI layer did not move**: `xemfg/vendor/` and
`xmx_generator.hpp`'s struct block are byte-identical again. Two commits carried code; one was ported.

### Taken (1)

- `e4d4aad` (the XMX half) -- the ported multi-frame model refuses the jittered-motion flag
(`DLSSG.MvecJittered`, bit 4), so Death Stranding 2 and HZD Remastered failed `init` and fell back to
Intel's 2x model. Upstream now strips the flag before `init` when `maxFrames > 1`: `src/xmx/generator.hpp`'s
`kFlagMotionJittered`, `JitterFlagStripped()`, and the `Prepare` init block (the log's `init` line now
reports the flags actually handed over, plus the frame count). The motion vectors still carry the camera
jitter -- about a render pixel at most; upstream saw no artifact in Death Stranding 2.

  Two deliberate deviations, both checked against this port's init path:

  - Upstream's switch is `[DLSSG] XmxJitter=0`, read with `GetPrivateProfileIntW`. This port has no ini
of its own, so the switch became `XmxJitter=0` under the existing `[Uplift]` section of `ReShade.ini`,
read once per process via `reshade::get_config_value` in `PushFgRuntimeChoices` (`src/addon.cpp`) and
pushed to `src/xmx/host.hpp`'s `SetJitterStrip`/`JitterStrip` (atomic, relaxed -- the same pattern as the
panel's switch). `0` restores the pre-1.8.6 behaviour: the multi-frame model is asked with the flag it
refuses and Intel's 2x model runs instead.
  - Upstream resets `init.flags` to the game's values after a stripped init so its same-frame 2x retry
re-reads them. This port refuses the shape instead (`refused`/`refusedShape`) and lets a later call
rebuild, so there is no second reader and the reset would be a dead store -- omitted.

### Not applicable: provider-module code this port does not have (2)

- `dfefd3e` -- DRS ceiling NvAPI hook, driver-only provider diagnostics, RTX 20/30 pre-Ada `slInit`
admission, panel wording: all of it lives in the mfgunlock addon's DLSS-G/NvAPI layer; this port hooks
none of it.
- `e4d4aad`'s remainder -- the `gameXefgModules`/ERSS entries in `profiles.json` and the tests are the
upstream XeFG-caller judgement (its provider now picks which XeFG module runs per game); this port
embeds the provider, so there is no foreign caller to route. The native_bridge DXGI factory-hook
diagnostics hang off a hook path this port does not walk.

### Not code (7)

`2928eeb`, `7654df9`, `ec1ba21` (README updates), `2643da7`, `ed5d8a7` (1.8.6 release notes),
`22415f1`, `36302f5` (1.8.5 release notes) -- version strings and docs, checked one by one on 2026-11-07.

## Sweep 2026-11-08: `2928eeb` -> `1226d4f` (1.9.0, 23 commits)

`main` is tagged 1.9.0. **The ABI layer did not move again**: `xemfg/vendor/` and `xmx_generator.hpp`'s
struct block are byte-identical, and no commit in the range touches a path matching `*xmx*`. Nothing was
taken. All 23 commits are accounted for, and none of them is XMX code: for the first time a whole upstream
release lands outside the half this port carries.

### Not applicable: the pre-Ada provider (8)

1.9.0 added NVIDIA's own frame generation for RTX 20/30 -- `eb27d59` (nefh's kernel preparation: `ampere/`
`cubin_trial.hpp`, `ptx_retarget.hpp`, `fatbin*.hpp`, `endpoint_*.hpp`, `provider_profile.hpp`, `gpu_arch.hpp`),
`7917987`, `86309cd` (nefh's temporal correction on the retargeted kernel), `9dc04d9`, `aee1f7c` (early
loading), `81ac24c` (gate refusal step/file wording), `63b03c8` (its tests). This port has no Ampere path at
all: it hands the black boxes to `libxess_fg.dll` and never retargets a kernel. The one transferable habit is
`81ac24c`'s "say which step failed and in which file" -- this port's `refused`/`refusedShape` already names
the shape, and the XeFG gate has no per-file step to name.

### Not applicable: provider-module and panel code this port does not have (8)

- `0fb97c2` (RTX 50 dynamic MFG provider version, `slInit` diagnostics), `7f4391b` (support report, driver
  profile), `28e5d96` (duplicate addon files, stale early-load entry), `7729014` (`native_notices.rc.in`
  FILEVERSION) -- all live in the mfgunlock addon's provider/NvAPI/installation layer; this port embeds the
  provider and installs no notices resource.
- `f25d6e3` (frame generation chain card, `fg_chain.hpp`), `fce5739`, `26efd00` (which Streamline plugin the
  game uses), `550728c` (the panel's own colour), `57de3a3` (collapsed by default) -- the panel's chain card.
  `fg_chain.hpp`'s idea (name the link that is not working) is what the `refused`/`refusedShape` log lines
  already do here, minus the card.
- `501f74e` (panel controls for `PreAdaAdmission`/`AutoEarlyLoad`), `7c294c4` (read `ReShade.ini` by its
  attributes) -- controls and a startup check for the pre-Ada path above.

### Not code (7)

`1226d4f`, `4e179c8`, `f6d158a` (README), `d81220d`, `1685291` (1.9.0 notes: validation, the RTX 3080
report's cause), `6b65add` (version, notes, README), `7108a77` (`tools/Prepare-Release.ps1`) -- version
strings and docs, checked one by one on 2026-11-08.

## Sweep 2026-11-17: `1226d4f` -> `c25358c` (1.10.0, 18 commits)

`main` and `develop` both moved to `c25358c` (tag 1.10.0); `xmx/proto/xmx-vram` was added. **The ABI layer did not move again**: `xemfg/vendor/` and `xmx_generator.hpp`'s struct block are byte-identical -- `kKnownTimestamp` is still `0x69CB0F4D`, `kInitRva` `0x1871C0`, `kExecuteRva` `0x185800`, and the five `static_assert`s are unchanged. No commit in the range touches a path matching `*xmx*` in this port's tree. Nothing was taken. All 18 commits are accounted for; none is XMX code this port carries.

### Not applicable: provider-module, Streamline host and pre-Ada code (6)

- `921fb4b` (`SL_LOG_*` environment), `e40ac6b` (`ForceFrameGeneration`), `3f79733` (Vulkan DLSS-G provider), `2b4735a`'s Streamline/SR-only host -- this port embeds the XeFG provider and observes NGX's own core; it never loads `sl.interposer` or routes through a Vulkan/DLSS-SR host, so there is no Streamline start to hook or feature list to edit.
- `45b9729` (architecture answer by caller module), `e5c73fc` (NGX requirements answers, provider export patching, crash dump) -- rewriting the pre-Ada answer or a requirements record is provider-module work; the 1.8.4, 1.8.6 and 1.9.0 sweeps already ruled this class out. Device-removed detection, the one piece that sounds generic, is already here in `src/addon.cpp:971`.

### Not applicable: panel and i18n (3)

`241ad58` (one file per page), `eb4de3b` (six-language tables), `8d837bd` (General-page switches). The port has no panel of its own; `refused`/`refusedShape` already name the failing shape in the log, which is the panel's chain-card idea from `f25d6e3` in the 1.9.0 sweep.

### Not applicable: vendored ABI (1)

`3b3cd4c` -- the dashdogy MFG Unlocker v1.7.0 -> v1.7.1 three-way merge rewrites `sdk_pacing.cpp/.h`, `vk_host.cpp`, `xvk.h`, `cuda_route.cpp` and adds VK tensor/pacer files, all under `xemfg/vendor/`. Read-only black box per `AGENTS.md`; no change handed to it.

### Not taken: VRAM copy counters (1)

`603a4fa` added `g_copy_bytes`/`g_copy_resources` outside the ABI block in upstream's `xmx_generator.hpp` and logged them in its per-10s `Summarize`. The counters are monotonic session totals, but this port's generator log reports per-interval deltas (`ReportRates`, `src/xmx/generator.hpp:201`) -- the wrong semantics for that line, so they were not ported. The transferable result is the measurement itself: 2560x1440 4x on an RTX 4070 SUPER, no FG 1267 MiB, NVIDIA DLSS-G +580 MiB, XMX +975 MiB, of which XMX's own 11 copy resources are 160 MiB; a 3-frame instead of 5-frame XeFG context changed nothing. Recorded in `docs/xefg-input-sizes.md`. No capacity option was added upstream either.

### Not taken: HUD composition (1)

`2b4735a`'s HUD fix sets `init.uiMode = 0` and `execute.uiComposition` in `xmx_generator.hpp`. Both fields already exist in this port's ABI block (`src/xmx/generator.hpp:59`, `:79`) but are never assigned -- correctly, because upstream 1.9.0 also left them untouched and `AGENTS.md` forbids changing what is handed to the black box. Adopting 1.10.0's assignments would alter black-box input, and its `OwnHudlessForThisFrame` additionally depends on a Streamline-host capture this port does not have. Skip.

Re-checked on 2026-11-17 while sweeping 1.10.1: `AGENTS.md`'s wording is "keep upstream's assignments
exactly" (do not invent your own), so moving to upstream's own assignments is not what it forbids, and
`OwnHudlessForThisFrame` falls back to the game's `DLSSG.HUDLess` when the Streamline host has nothing,
which this port already captures (`src/xmx/generator.hpp:527-547`). The decision still stands, but the
1.10.1 sweep below states the reasons it is still deferred.

### Not code (6)

`8f47ce0` (version), `54aaee8` (compat matrix/CI/issue form), `5055ce3`, `685afbe`, `765669e`, `c25358c` (plans, release notes, README) -- none touches the XMX code path.

## Sweep 2026-11-17: `c25358c` -> `395ed32` (1.10.1, 1 commit)

`main` moved on to `395ed32` (tag 1.10.1); `c25358c` is its parent, so this is the single commit the
1.10.0 sweep above left. **The ABI layer did not move**: `xmx_generator.hpp`'s struct block is
byte-identical -- `kKnownTimestamp` is still `0x69CB0F4D`, `kInitRva` `0x1871C0`, `kExecuteRva`
`0x185800`, and the five `static_assert`s are unchanged. This commit touches no file under
`xemfg/vendor/` at all, so the vendor delta recorded in the 1.10.0 sweep is unchanged by it. Nothing
was taken; the commit's XMX work is one item, deferred on purpose below.

### Deferred: the HUD-composition guard (1)

`395ed32` is upstream's repair of the HUD composition `2b4735a` introduced, and it is the piece that
makes that feature adoptable by a mod. `xmx_generator.hpp` (44 lines) gains `Report::hud`,
`SetHudState`, `same_size`/`same_family` and the decision

```
execute.uiComposition = next.hudless && previous.hudless && UiCompositionWanted() ? 1u : 0u;  // [DLSSG] XmxUiComposition, default 0
hud_inputs = execute.uiComposition == 0 && next.hudless && previous.hudless && HudCompositeWanted();  // [DLSSG] HudFix, default 1
composite  = hud_inputs && same_family(next.hudless, next.backbuffer) && same_family(previous.hudless,
             previous.backbuffer) && !Tripped() && !failed && same_size(next..) && same_size(previous..);
```

When `composite` holds, `execute.previous.backbuffer`/`execute.next.backbuffer` become the HUD-less
copies, XeFG interpolates those, and `hudcomposite::Apply` then paints the real frame's pixels (where
they differ from the HUD-less colour by more than 3/255) over the generated frame. `xmx_policy.hpp`
(45 lines) adds `FormatFamily` and `HudGuard` -- a 16-frame window, 12 of whose measured frames over a
30%-of-screen difference share trip it, and once tripped it stays tripped for the session; one pixel in
sixteen is counted, from a counter buffer read back 6 passes behind. `xmx_hud_composite.hpp` (92 lines)
gains that readback plus `Measure`/`Tripped`, and `tests/xmx_policy_tests.cpp` covers the guard. The log
line is `HUD: on: XeFG interpolates the HUD-less color; the real frame's HUD is composed over it` or
`HUD: off: ... NN% of the screen differed; RenoDX or another post-process after it?`.

Not taken now, for three reasons:

- **The payoff is outside this port's main configuration.** The composite needs the frame XeFG sees to
  be the game's own colour, so it engages with NR off or with NR at Present, and is expected to trip off
  whenever NR has already run on that frame (NR stage = After DLSS, `src/ui/settings_schema.cpp:1126-1130`),
  because the HUD-less colour the game hands over is not the NR'd image. It then falls back to today's
  1.9.0 behaviour by itself, but it would pay off only for users who do not use the stage this add-on
  exists for. Not measured on hardware.
- **Upstream changed this logic twice in two days** (`2b4735a`, then this commit). Porting it now adds a
  file to re-sweep every pass while XMX here is still an opt-in experimental switch
  (`src/ui/settings.hpp:299`, off by default); one or two sweeps will settle its shape.
- **It changes what is handed to the black box** (the HUD-less colour as the interpolated back buffer,
  plus `uiComposition`, and `uiMode` from 1.10.0) on a path this port has never exercised, and it cannot
  be validated without a DLSS-G game that supplies `DLSSG.HUDLess`. A mistake there sits behind the crash
  latch (`src/addon/removal_latch.cpp`).

Revisit when XMX stops being experimental, when HUD ghosting is reported, or when upstream leaves this
code alone for a sweep or two. If taken, take 1.10.0 and 1.10.1 together -- the composite without this
guard is exactly the bug this commit fixes. The porting shape is already clear: measure the difference
first from the two copies `Capture` already makes (`src/xmx/generator.hpp:527-547`); compile the
composite shader through this repo's `shaders/*.hlsl`/dxc path rather than upstream's
`LoadLibraryW("d3dcompiler_47.dll")`; insert the pass at `src/xmx/generator.hpp:936`, where the
generated output is copied into `DLSSG.OutputInterpolated`, reading `state.outputs[frameIndex - 1]`
(already `UNORDERED_ACCESS`) and the port's own copies (already `kXefgInputState` =
`NON_PIXEL_SHADER_RESOURCE`, exactly what `Apply` asks for); add `FormatFamily`/`HudGuard` to
`src/xmx/policy.hpp`; gate it behind a setting, or an `[Uplift]` ini key following `XmxJitter`. Acceptance:
`.\build.cmd release`, `HUD: on` with NR off and with NR at Present in one DLSS-G game, `HUD: off: ...`
with NR at After DLSS, no crash latch.

### Not applicable: provider module, panel, Streamline files and i18n (1 commit)

`addon.cpp`, `addon_provider.inl`, `sl_module.hpp`, `sl_host.hpp`, `swapchain_format_guard.hpp`,
`fg_chain.hpp`, `framecount.hpp`, `force_fg.hpp`, `preada_admission.hpp`, `hud_capture.hpp`,
`xemfg/dependency_manager.*`, `xemfg/sl_vulkan.cpp`, `xemfg/native_*`, `streamline_catalog.json`,
`overlay_page_general.inl`, `i18n/string_keys.hpp` and the six `strings_*.inl` tables, plus
`tools/Repair-XeDependencies.ps1`, `tools/sl_sample.py` and the golden PNGs -- the provider-module /
Streamline-host / panel line of work the 1.10.0 sweep already ruled out. The three `xmx_*` files above
are the only XMX files in the commit.

## Sweep 2026-11-22: `395ed32` -> `8b854ff` (5 commits, no XMX source; vendor re-vendored)

`395ed32` (1.10.1) -> `8b854ff` is `f5a1b4f`, `5a3d273`, `8b854ff` (README and the per-method results),
`8c953f4` and `d3b26f5` (CI skip for the provider-identity test). The files are `README.md`,
`docs/compat/compat.json`, `docs/compat/matrix.md`, `.github/workflows/build.yml` and
`tests/provider_identity_tests.cpp` -- **nothing under `src/addons/mfgunlock/`**. The XMX source
comparison base therefore stays 1.10.1; the 1.10.0 and 1.10.1 sweeps above are still the whole story for
`src/xmx/generator.hpp`, `hook.*` and `policy.hpp`.

On request, the entire `xemfg/vendor/` tree was re-vendored from `8b854ff` anyway. That is the delta the
1.10.0 sweep recorded as "Not applicable: vendored ABI" (`3b3cd4c`, the dashdogy MFG Unlocker v1.7.0 ->
v1.7.1 merge) and it was declined there because `AGENTS.md` makes the black box read-only. The user lifted
that rule for `src/xmx/vendor/` on 2026-11-22, so it came over.

### Taken: the rewritten tensor-core route (`cuda_route.cpp/.h`, new `cuda_layers.h`)

The route is now a different program, not a patched one: `cuda_layers.h` (generated by
`generate_cuda_layers.py`) carries the layer table out of the .cpp, and the state it used to keep in one
`Recording` is a `ListState` (`layer`, `heap`, `tables[2]`, `tableMask`, `constants[12]`,
`constantMask`) reached through `LayerOf(pso)`, with `LaunchOn(list, const ListState&, x, y, z)`,
`FillLaunch`, `DispatchShapeOk`, `CanRecord(list, reason)` and `ClassifyList(list, shared, runtimeClass,
writable)`. Pipeline states are tracked by generation (`NewPipelineToken`/`PipelineScope`/
`ForgetPipelines`, `HeldLayer{state, token, layer}`) so a released PSO cannot leave a stale layer bound.
Provider calls are recognized by walking the stack past the wrapper's image (`gWrapperBegin`/`gWrapperEnd`,
`FromWrapper`, `FromProvider`, `Origin`), pipeline streams are parsed (`StreamRefused`), and refusal
counters (`gListRefusals`, `gPsoRefusals`) replace the per-line experimental logs. `Install` gains
`adoptProviderLists = true`, plus `SetLayerMask`/`LayerEnabled`, `AdapterCapability` and `Report`.

### Taken: the rest of `xemfg/vendor/` (comment and API level)

`host_unlock.*`, `runtime_layout.*`, `port/*`, `adapter_discovery.h`, `image_slots.h`,
`overlay_native.h`, `ampere_provider_arch.h`, `runtime_payload.*`, `sha256_util.h`: the experiment
codenames are gone from the comments (`dev.40`/`dev.46`/`V1.5.1`/`V1.6.1`/`beta.7-xell.1`/`xefg134.1`/
`#490 scene.1`), the test-only APIs are gone (`OutputRing::Restore()/Applied()`,
`Hdr10Flag::Restore()/Applied()`, `Unlock::GatesXeLL()` -- this port has no caller for any of them), and
`adapter_discovery.h` replaces `DriverNgxRuntimePath` with `DriverFilePath(candidate, file, systemCopy)`.
Only `#include` lines differ from upstream, as `AGENTS.md` allows: `cuda_route.cpp`'s
`../adapter_discovery.h` and `../overlay_native.h` are same-directory here, its `../../native_trace.hpp`
is the dropped-file comment (that header is empty upstream), and `port/port_support.h` includes
`xmx/host.hpp` instead of `../xess_mod.h`.

### The tensor cores still do not run on an Agility SDK wrapper

The rewrite keeps the wall this port already documented: the route refuses a table that is not the
runtime's or one the provider created itself ("a wrapper's or another module's table never matches", and
"the wrapper's tables are never written: hooks copied from it into the game's own list tables would also be
refused"). ReShade64.dll builds exactly such a table in front of the game's list, and Agility SDK 618/619
additionally keeps every list's class table private, so Naraka (618) and The Witcher 3 (619) stay at 0
launches / 0 fallbacks and the DXIL port keeps running. Nothing in this sweep was measured on hardware.

### Taken: the same-frame 2x retry (generator, 1.8.3)

The 1.10.0 sweep left this out as "no second reader here". It is in now, because it is upstream's own
behaviour and it turns "XMX does nothing in this game" into 2x: when `Init` fails and `maxFrames` was above
1, `src/xmx/generator.hpp`'s `Prepare` starts a second context with the game's real flags and
`maxFrames = 1`, then logs `the multi-frame model refused these inputs; Intel's 2x model runs instead`.
Nothing new is handed to the black box that upstream does not already hand it.

### Still not taken

- **HUD composition and its policy helpers** (`2b4735a` + `395ed32`'s guard): the 1.10.1 sweep's three
  reasons stand unchanged (payoff only without NR at After DLSS, upstream changed it twice in two days, and
  it changes what the black box is handed). `FormatFamily` and `HudGuard` exist only to serve that pass, so
  they are still not in `src/xmx/policy.hpp`; adding them alone would be dead code.
- **`PlanSlot`/`Bypassed` and the multi-slot install model** (`xmx_hook.hpp`, `kSlots = 4`,
  ReShade-resident pinning): that file is upstream's provider-module hook model. This port installs one hook
  on NGX's own core from `src/addon.cpp` and has no module to re-scan.
- **`LoadContext`'s already-loaded-module preference**: a game's own `libxess_fg.dll` is a different
  build, and the RVAs in `src/xmx/generator.hpp` belong to 1.3.1.78 (`kKnownTimestamp`). This port keeps
  loading its own copy and refuses anything else, which is what makes the port safe.
- **The 10 s `Summarize` and the copy-byte counters**: this port's rates line is a 2 s delta
  (`ReportRates`), and monotonic session totals would be the wrong semantics in it (1.10.0 sweep).
- **`tagstates::StateOf` and the compute-list `kGraphicsOnly` downgrade**: this port has
  `EffectiveSourceState`; `docs/xefg-input-sizes.md` records what it does and does not cover.
- **`MatchDepthAndMotion`'s one-percent gate**: upstream 1.10.1 dropped it and always shares the declared
  area. This port keeps the gate on purpose (a 4:3 depth against 16:9 motion vectors) -- see
  `docs/xefg-input-sizes.md`.

### Acceptance

`.uild.cmd release` exit 0 on 2026-11-22, deployed to `F:\Optiscaler\gitc-uplift.addon64`
(11,060,736 bytes). The only warning is the pre-existing `C4458` in `runtime_layout.h`. Not run in a game
in this pass.

## The next sweep

```
git clone --depth 1 --filter=blob:none --sparse https://github.com/mavismmg/MFGAdaUnlock-RenoDx.git /tmp/ref
git -C /tmp/ref sparse-checkout set --no-cone "/src/addons/mfgunlock/" "/tests/"
git -C /tmp/ref fetch --unshallow
git -C /tmp/ref log --oneline 3b8889f..<new-head>
```

Filenames triage the log: `xemfg/vendor/` and `xmx_generator.hpp`'s struct block are ABI code (read, never
edit); `*.md`, `docs/`, `tests/` and version strings are not a port; anything in `addon_*.inl`,
`*_hook.hpp`, `tag_states.hpp` or `generic_gate.hpp` hangs off the provider-module hooks this port does not
have -- check for an NGX-layer equivalent before assuming it is portable. Date every sweep.
