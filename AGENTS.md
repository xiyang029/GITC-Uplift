## Verifying a change

Build every change before calling it done. From the repository root:

```
.\build.cmd release
```

It fetches the dependencies, configures the `release` preset and compiles the add-on; a zero exit and a
`gitc-uplift.addon64` newer than the edit are the pass condition. `debug` and the `-x86` presets build the same way
(`.\build.cmd debug`, `.\build.cmd release-x86`).

Shaders are compiled by the same build: `shaders/*.hlsl` go through the Windows SDK's `dxc.exe` into
`build/<preset>/generated/shaders/*.h`, so a shader edit is verified by the same command with no separate step.

Known trap (build.cmd's own note): this toolchain's `cl` prints `/showIncludes` in the localized form, so Ninja records
no header dependencies and a header-only edit keeps its stale objects. After touching only a `.hpp`/`.hlsli`, touch a
`.cpp` of the same target before building, or the build proves nothing.

## The XMX black box is read-only

`src/xmx/vendor/` and the Intel ABI in `src/xmx/generator.hpp` (`InitParams`, `Input`, `FrameData`,
`ExecuteParams`, their `static_assert`s, the `k*Rva`/`kKnownTimestamp` constants) describe
`libxess_fg.dll` 1.3.1.78. **Never change what is handed to those black boxes.**

- `src/xmx/vendor/`: only `#include` lines may differ from upstream.
- ABI structs: byte-identical, comments included.
- `init.*`/`execute.*`/`data.*` before `kInitRva`/`kExecuteRva`: upstream's assignments exactly.
- Optimise around it: lifetime, barriers, copies, `src/xmx/hook.cpp`.

Upstream is `github.com/mavismmg/MFGAdaUnlock-RenoDx` @ `0d55206` (not kept locally). Fetch it to compare:

```
git clone --depth 1 --filter=blob:none --sparse https://github.com/mavismmg/MFGAdaUnlock-RenoDx.git /tmp/ref
git -C /tmp/ref sparse-checkout set --no-cone "/src/addons/mfgunlock/xemfg/vendor/" "/src/addons/mfgunlock/xmx_generator.hpp"
```

Expected differences, nothing else: `#include` lines in `port/model_port.h`, `port/shader_port.h`;
`port/port_support.h`'s `../xess_mod.h` becoming `xmx/host.hpp`; `cuda/cuda_route.cpp`'s two include paths and its
dropped empty `native_trace.hpp`. `generator.hpp` came from `xmx_generator.hpp` there; its struct block and five
`static_assert`s stay byte-identical, everything outside them is ours.
