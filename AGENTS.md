# AGENTS.md

## Verifying a change

`.\build.cmd release` from the repository root. Pass condition: exit 0 **and** `gitc-uplift.addon64` newer
than the edit. `debug` and `release-x86` build the same way; shaders (`shaders/*.hlsl` through the SDK's
`dxc.exe`) compile in the same run.

Trap: this toolchain's `cl` prints `/showIncludes` in the localized form, so Ninja records no header
dependencies and a header-only edit keeps its stale objects. After touching only a `.hpp`/`.hlsli`, touch a
`.cpp` of the same target before building, or the build proves nothing.

Warnings from vendored code (XeSS, vendor headers, third-party) are pre-existing; never "fix" one by editing
the vendored source.

## The XMX black box is read-only

`src/xmx/vendor/` and the Intel ABI in `src/xmx/generator.hpp` (`InitParams`, `Input`, `FrameData`,
`ExecuteParams`, their `static_assert`s, the `k*Rva`/`kKnownTimestamp` constants) describe
`libxess_fg.dll` 1.3.1.78. **Never change what is handed to those black boxes**: in `src/xmx/vendor/` only
`#include` lines may differ from upstream, the ABI structs stay byte-identical (comments included), and
`init.*`/`execute.*`/`data.*` before `kInitRva`/`kExecuteRva` keep upstream's assignments exactly.
Optimise around it: lifetime, barriers, copies, `src/xmx/hook.cpp`.

## Where the rest is written down

- `docs/xmx-upstream-port.md` -- the XMX upstream (`MFGAdaUnlock-RenoDx`) sweep log: what each 1.8.3/1.8.4
  change became here, and why the provider-module ones do not apply. Read before porting.
- `docs/xefg-input-sizes.md` -- how XeFG judges input sizes, and the two explanations that were measured and
  are wrong. Read before touching input sizes or anything deciding whether a frame generates.
