# AGENTS.md

## Shell and tools

- Use PowerShell 7 (`pwsh`); do not switch to `cmd.exe` or Windows PowerShell 5.1. Invoke the repository build script as documented below.
- Search with `rg` or `fd`, not PowerShell's native search equivalents. For large files, locate code first and read only the relevant area (about 30 lines around a match).
- In PowerShell, use backticks for escaping and `-eq` for string equality. Use `$true`/`$false` for booleans and `$env:NAME = "value"` for environment variables. Use Python for complex commands instead of dense one-liners.

## Change discipline

- Before deleting anything, list the exact targets and wait for user confirmation. After an approved deletion, build and test.
- For bug reports, state the location and trigger condition first; limit changes to the confirmed issue.
- Do not edit vendored code to silence pre-existing warnings.

## Build verification

- From the repository root, run `.\build.cmd release` in PowerShell 7. It must exit with code 0 and produce a `gitc-uplift.addon64` newer than the edits. Shaders build in the same run. `debug` and `release-x86` are also available.
- For `.hpp`- or `.hlsli`-only changes, touch a `.cpp` in the same target before building; localized `/showIncludes` prevents Ninja from tracking these dependencies.

## XMX compatibility

Preserve the inputs to `libxess_fg.dll` 1.3.1.78:

- In `src/xmx/vendor/`, only `#include` lines may differ from upstream.
- In `src/xmx/generator.hpp`, keep `InitParams`, `Input`, `FrameData`, and `ExecuteParams` byte-identical, including comments. Do not change their `static_assert`s or the `k*Rva`/`kKnownTimestamp` constants.
- Keep upstream's `init.*`, `execute.*`, and `data.*` assignments before `kInitRva`/`kExecuteRva` unchanged. Optimize around the calls: lifetime, barriers, copies, and `src/xmx/hook.cpp`.

## References

- [XMX upstream port log](docs/xmx-upstream-port.md)
- [XeFG input sizes](docs/xefg-input-sizes.md): read before changing input sizes or frame-generation eligibility.
