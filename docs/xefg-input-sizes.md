# What XeFG does with input sizes

XeFG (the `libxess_fg.dll` 1.3.1.78 this add-on drives) validates every input's shape before generating and
refuses the frame with `-4` when a rule is broken. The refusal is per frame and silent: the tell is the
`xmx rates` log line, where `refused` climbs with `asked` while `generated` stays at zero, and the picture
stutters because the hook then copies the back buffer (the frame rate even goes *up*). A `-4` alone is not a
symptom; the mode is expected to survive it.

## The rule that bit us: depth and motion vectors must be the same size

Intel's developer guide: depth's "buffer size must be the same as the motion vectors buffer", and the tagged
resources of a group share a `resourceSize`. Measured 2026-11-03 at a 2048x1152 swap chain: settings whose
two sizes agree generated everything; quality 0.67 -- depth 1368x772 against motion vectors **1367x769** --
refused every frame for a whole session. One column and three rows are enough: the check is exact.

## Two explanations that were measured and are wrong

Do not reintroduce either.

- **An odd row or column.** Trimming 1367x769 to 1366x768 changed nothing: depth stayed 1368x772, the mode
  stayed 100% refused, the tensor-core launch counter stayed flat.
- **An integer ratio to the swap chain.** 0.67's 1367x769 is 1.5:1 and works as soon as the two agree; 0.5's
  exact 1:2 was never the reason it worked. Low-res motion vectors at any upscaler ratio are supported.

## How it is handled

`src/xmx/generator.hpp` copies each input at its own shape, and `MatchDepthAndMotion` hands XeFG the area
both share (`policy::InputArea` reads the game's own `DLSSG.Depth*Subrect*`/`DLSSG.MVecs*Subrect*`
declaration when there is one; `policy::SharedArea` takes the smaller width and height, keeping each input's
origin). XeFG compares `Input` fields rather than textures, so no copy is trimmed. Ported from upstream 1.8.2,
which fixed the same refusal for Dying Light 2 and Phantom Blade Zero.

Only a pair within a percent of each other is shared; a genuinely different resolution is left for XeFG to
refuse. At 0.67 the game also hands over a 1368x1024 depth for a few seconds at a time (below) -- a 4:3
picture against the motion vectors' 16:9, so a declared shared area would give XeFG two inputs that agree on
size and disagree on geometry: a wrong picture instead of a refusal, and much harder to notice.

## Open: the short-lived 1368x1024 depth

One session (2026-11-03, 09:57:05-09:57:19) logged 24 frames whose depth was 1368x1024 against 1367x769
motion vectors: 40 refused frames inside a 2s window that otherwise generated everything. Unexplained; it
looks like a pass resolving depth at another geometry (1024 = 1152 x 8/9) rather than a mip of the 1368x772
buffer. Whoever picks it up should first find what the player was doing in those seconds.

## Reading it out of a log

`C:\Optiscaler\ReShade.log`, written by `xess_mod::WriteLog`. Three lines answer "is XMX generating, and if
not why": `xmx arguments` (every distinct resource set handed to XeFG, one line per change), `xmx refused
execute refused: ... previous ... | next ...` (what XeFG said no to), and `xmx rates over 2000ms asked N ...
generated N ... refused N | pairs N ... | execute Nus avg` (the per-2s truth, unaffected by the 48-line cap on
the generator's own log). The log reaches tens of megabytes: read its tail in node, since a content search
fails past the 20 MB cap.

## Against upstream

Every rule here came from upstream `MFGAdaUnlock-RenoDx` @ `3b8889f`; `docs/xmx-upstream-port.md` carries the
sweep log and the current comparison base.
