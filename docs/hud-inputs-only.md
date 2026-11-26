# HUD inputs without frame generation (experimental)

`FGHudInputsOnly=1` under `[Uplift]`, or **HUD inputs only (experimental)** in the
DLSS / Reflex tab, submits `DLSSGMode::eOff` to the wrapped Streamline backend.
It does not set the generated-frame count to zero, discard generated frames,
skip NGX evaluation, or falsify a successful result/presentation count.

## Trying it in a game

1. Exit the game, replace `gitc-uplift.addon64`, then restart.
2. Keep frame generation enabled in the game's own menu.
3. Enable **HUD inputs only (experimental)**. It takes priority over multiplier,
   Dynamic MFG, XMX and the FG-preset drain while enabled.
4. For Uplift to use the tagged HUD-less image, set **UI correction** to **On**
   and select a supported Present route. Capturing tags alone does not move
   native-NR Replace/After processing to Present.
5. Check **Currently**: the backend must have accepted Off. The HUD-less and
   opacity counters must continue increasing, with **recent tags 1/1**.

Counters are lifetime non-empty tag submissions. Recent means a tag arrived
within two seconds; it does not prove the two textures belong to one frame or
meet size/format/lifetime requirements. The existing UI-correction note reports
whether a pair is actually usable. Explicitly invalidating tags clears freshness;
normal consumption preserves the last arrival time until it ages out.
The five-second log trace also includes the previous selected Present's actual
UI-correction outcome, so increasing counters are not confused with composition.

On the native Direct3D 12 Present route with FG off, active ReShade effects do
not invalidate the HUD inputs. Uplift runs NR on its HUD-less copy and composes
the HUD back in the same Present call, before ReShade applies its effects. The
Uplift technique does not defer this detached target to a later event. With FG
running, the existing restriction remains: an early HUD-less copy cannot be
given to FG when it would miss the effects applied to the displayed frame.

The backend's real last `numFramesActuallyPresented` answer and observed NGX FG
Evaluate call count are reported separately. An Evaluate call is not a GPU
dispatch measurement; accepted Off alone is not a measured power saving.

## Failure and restoration

Some games stop producing HUD-less/opacity inputs when the backend is Off.
This mode cannot manufacture those images or force game render passes to run.
If tags stop, disable the experiment; do not infer success from the menu toggle.

Backend refusal is returned and displayed honestly, without falling back to
an enabled request. Unknown options ABI versions are refused without submitting
a guessed structure. Toggle the game FG setting or the experiment to retry.

Disabling replays the game's latest known request, including a game-side Off;
an old enabled request must not revive FG. Requests with caller-owned extension
chains are forwarded during the call but are not cached for later replay. In
that case toggle game FG to supply a new request. A rejected restore keeps the
accepted-Off state visible until the backend accepts a later game request.

The experiment does not eliminate the game's cost of preparing HUD inputs.
Whether inputs survive and how much power is saved require an in-game test.

## Regression

`uplift_fg_hud_inputs_regression` compiles the real Streamline wrapper against
a controlled backend, covering options ABI versions 1–5, repeated requests,
override priority, live switching, game-side Off, refusal/retry, unknown ABI,
extension-chain lifetime, real presentation counts, config and Chinese text.
`uplift_hud_present_regression` covers actual D3D12 tag freshness, FG-off HUD
composition with active effects/techniques, same-Present trigger timing and
restoration of ordinary placement, as well as native-NR status.
