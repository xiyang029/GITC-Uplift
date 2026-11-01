# Notes for agents

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
