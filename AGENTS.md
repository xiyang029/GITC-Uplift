# AGENTS.md

## Tools
- PowerShell 7 (`pwsh`) only; never `cmd.exe` or PowerShell 5.1.
- When searching with rg/fd, if the search term contains regex special characters (( ) [ ] { } * ? | ^ $ . ), it may be mistakenly treated as syntax, causing injection or incorrect matches. The simplest way: add -F to disable regex, wrap it in single quotes, and add -- to prevent option injection, i.e., rg -F -- 'pattern' file; for large files, use rg -F -C 15 -- 'pattern' bigfile to only read about 30 lines around the matched line.
- Prefer Python for complex commands. PowerShell: backticks escape, `-eq` for strings, `$true`/`$false`, `$env:NAME = "value"`.

## Change discipline
- List exact deletion targets and wait for confirmation; build and test afterwards.
- Bug reports: state location and trigger first, fix only the confirmed issue.
- Never edit vendored code to silence pre-existing warnings.

## Build
- From the repo root: `.\build.cmd release`. Must exit 0 and produce a `gitc-uplift.addon64` newer than the edits; shaders build in the same run. Also `debug`, `release-x86`.
- `.hpp`/`.hlsli`-only changes: touch a `.cpp` in the same target first, since localized `/showIncludes` hides them from Ninja.

## XMX (`libxess_fg.dll` 1.3.1.78 ABI)
- `src/xmx/vendor/`: only `#include` lines may differ from upstream.
- `src/xmx/generator.hpp`: `InitParams`, `Input`, `FrameData`, `ExecuteParams` byte-identical (comments too); do not touch their `static_assert`s or `k*Rva`/`kKnownTimestamp`.
- Keep upstream `init.*`/`execute.*`/`data.*` assignments before `kInitRva`/`kExecuteRva` unchanged; optimize lifetime, barriers, copies, and `src/xmx/hook.cpp` around the calls.

## References
- `docs/xmx-upstream-port.md` — XMX upstream port log.
- `docs/xefg-input-sizes.md` — read before changing input sizes or frame-generation eligibility.
