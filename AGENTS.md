## Verifying a change

Build every change before calling it done. From the repository root:

```
.\build.cmd release
```

A zero exit and a `gitc-uplift.addon64` newer than the edit are the pass condition; `debug` and `release-x86` build
the same way (`.\build.cmd debug`, `.\build.cmd release-x86`). Shaders compile in that same run (`shaders/*.hlsl`
through the SDK's `dxc.exe` into `build/<preset>/generated/shaders/*.h`), so a shader edit needs no separate step.

Trap: this toolchain's `cl` prints `/showIncludes` in the localized form, so Ninja records no header dependencies and
a header-only edit keeps its stale objects. After touching only a `.hpp`/`.hlsli`, touch a `.cpp` of the same target
before building, or the build proves nothing.

## The XMX black box is read-only

`src/xmx/vendor/` and the Intel ABI in `src/xmx/generator.hpp` (`InitParams`, `Input`, `FrameData`,
`ExecuteParams`, their `static_assert`s, the `k*Rva`/`kKnownTimestamp` constants) describe
`libxess_fg.dll` 1.3.1.78. **Never change what is handed to those black boxes.** In `src/xmx/vendor/` only
`#include` lines may differ from upstream; the ABI structs stay byte-identical, comments included; `init.*`,
`execute.*` and `data.*` before `kInitRva`/`kExecuteRva` keep upstream's assignments exactly. Optimise around it:
lifetime, barriers, copies, `src/xmx/hook.cpp`.

Upstream is `github.com/mavismmg/MFGAdaUnlock-RenoDx` @ `3b8889f` (1.8.2; the port started from `0d55206`,
kept locally at `%TEMP%/ref`). To compare:

```
git clone --depth 1 --filter=blob:none --sparse https://github.com/mavismmg/MFGAdaUnlock-RenoDx.git /tmp/ref
git -C /tmp/ref sparse-checkout set --no-cone "/src/addons/mfgunlock/xemfg/vendor/" "/src/addons/mfgunlock/xmx_generator.hpp"
```

Compare against **`develop`**, never `main`: the code is on `develop`, and `main` has only ever taken
`README.md` edits. Measured 2026-11-05: `develop` == `3b8889f` (identical), `main` 5 `README.md` commits ahead of it.

```
curl -s "https://api.github.com/repos/mavismmg/MFGAdaUnlock-RenoDx/compare/3b8889f...develop" | grep -E '"status"|"filename"'
```

`identical` means there is nothing to port. When it moves, the filenames triage it: `src/addons/mfgunlock/xemfg/vendor/`
and `xmx_generator.hpp` are ABI code and need the sparse clone above; `README.md` and docs are not a port. Date every check.

Expected differences, nothing else: `#include` lines in `port/model_port.h`, `port/shader_port.h`;
`port/port_support.h`'s `../xess_mod.h` becoming `xmx/host.hpp`; `cuda/cuda_route.cpp`'s two include paths and its
dropped empty `native_trace.hpp`. `generator.hpp` came from `xmx_generator.hpp` there; its struct block and five
`static_assert`s stay byte-identical, everything outside them are ours.

Never hand XeFG DLSS-G's own `OutputInterpolated` buffer. It was tried twice and stuttered both times: once with a
barrier, once with the UAV flag checked first and no barrier at all, with every frame generating and every counter
healthy. The frames go into textures of ours, created in `UNORDERED_ACCESS`, and are copied back.

## XeFG takes depth only at the motion vectors' size

Depth and motion vectors of different sizes are refused per frame with `-4`, and the refusal is silent: the panel says
`not generating`, the rates log shows `generated` at zero while `refused` climbs with `asked`, and the frame rate
rises because the mode then copies the back buffer.

XeFG compares the `Input` fields, not the textures, so `src/xmx/generator.hpp`'s `MatchDepthAndMotion` copies both
at their own shape and hands over the area they share (`policy::InputArea`/`SharedArea`, reading the game's own
`DLSSG.Depth*Subrect*`/`DLSSG.MVecs*Subrect*` declaration when there is one). A pair more than 1% apart is another
picture, not a rounding, and is left for XeFG to refuse. Do not replace that with an odd-size or an integer-ratio
check: both were measured and both are wrong.

## Where the rest is written down

- `docs/xefg-input-sizes.md` -- read before touching input sizes, the `arguments`/`refused`/`rates` log lines, or
  anything that decides whether a frame generates. Holds the measurements, the two wrong explanations, the unexplained
  1368x1024 depth, and how to read those log lines.
