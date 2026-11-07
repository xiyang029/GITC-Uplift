# Porting XMX from MFGAdaUnlock-RenoDx

One upstream for the XMX half: `github.com/mavismmg/MFGAdaUnlock-RenoDx`. The port started from `0d55206`;
the comparison base is `3b8889f` (1.8.2). Each sweep is recorded here: what was taken, what was left, why.

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
