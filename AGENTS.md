# AGENTS.md

## Build verification

- From the repo root, run `.\build.cmd release`. It must exit 0 and produce a `gitc-uplift.addon64` newer than the edit. Shaders build in the same run; `debug` and `release-x86` are also available.
- For `.hpp`- or `.hlsli`-only changes, touch a `.cpp` in the same target before building: localized `/showIncludes` prevents Ninja from tracking these dependencies.
- Do not edit vendored code to silence pre-existing warnings.

## XMX compatibility

Preserve the inputs to `libxess_fg.dll` 1.3.1.78:

- In `src/xmx/vendor/`, only `#include` lines may differ from upstream.
- In `src/xmx/generator.hpp`, keep `InitParams`, `Input`, `FrameData`, and `ExecuteParams` byte-identical, including comments. Do not change their `static_assert`s or the `k*Rva`/`kKnownTimestamp` constants.
- Keep upstream's `init.*`, `execute.*`, and `data.*` assignments before `kInitRva`/`kExecuteRva` unchanged. Optimize around the calls: lifetime, barriers, copies, and `src/xmx/hook.cpp`.

## References

- [XMX upstream port log](docs/xmx-upstream-port.md)
- [XeFG input sizes](docs/xefg-input-sizes.md): read before changing input sizes or frame generation eligibility.
