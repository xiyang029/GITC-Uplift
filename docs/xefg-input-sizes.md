# What XeFG does with input sizes

XeFG (Intel's frame generation, the `libxess_fg.dll` 1.3.1.78 this add-on drives) validates the shape of
every input before it generates, and refuses the frame with `-4`
(`XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT`) when a rule is broken. The refusal is per frame and
silent, so the counter that gives it away is the `xmx rates` log line: `refused` climbs with `asked`
while `generated` stays at zero. The picture stutters because the hook then copies the back buffer into
the interpolated frame -- and the frame rate goes *up*, because nothing was computed. `-4` on its own is
not a symptom: it is a normal answer to a frame XeFG cannot use, and the mode is expected to survive it.

## The rule that bit us: depth and motion vectors must be the same size

Intel's own words (`intel/xess`, `doc/xess_fg_developer_guide_english.md`):

- Inputs: *"Depth buffer ... **Buffer size must be the same as the motion vectors buffer**."*
- Resource Tagging: *"motion vectors and depth have the same `resourceSize`"*, and *"HUD-less, UI, and
  back buffer have the same `resourceSize`"*.

Measured 2026-11-03, 2048x1152 swap chain, format 24, one session per row:

| SR setting | depth | motion vectors | generates |
| --- | --- | --- | --- |
| DLAA 1.0 | 2048x1152 | 2048x1152 | yes |
| performance 0.5 | 1024x576 | 1024x576 | yes |
| quality 0.67 | 1368x772 | **1367x769** | **no**, every frame for a whole session |

The two sizes at 0.67 differ by one column and three rows. That is enough: the size check is exact.
The settings whose two sizes already agree generated at full rate (0.5: `asked 739 generated 739
refused 0` over 2s), which is what says the mismatch, and not the resolution itself, is the trigger.

## Two explanations that were measured and are wrong

Do not reintroduce either of them.

- **An odd row or column.** Trimming 1367x769 to 1366x768 changed nothing: depth stayed 1368x772, the
  mode stayed at 100% `refused`, and the tensor-core launch counter stayed flat.
- **An integer ratio to the swap chain.** 0.67's 1367x769 is 1.5:1 and works as soon as the two agree;
  0.5's exact 1:2 was never the reason it worked. Low-res motion vectors at any upscaler ratio are
  supported by design -- that is the recommended arrangement.

## How it is handled

`src/xmx/generator.hpp`: the copies keep the shapes they were taken at, and `MatchDepthAndMotion` hands
XeFG the area both inputs share (`policy::InputArea` reads the game's own `DLSSG.Depth*Subrect*` /
`DLSSG.MVecs*Subrect*` declaration when there is one, `policy::SharedArea` takes the smaller width and
height and keeps each input's own origin). XeFG compares these `Input` fields rather than the textures,
so no copy is ever trimmed. Taken from upstream 1.8.2 (`3b8889f`, "XMX in Dying Light 2, Alan Wake 2
and Phantom Blade Zero"), which fixed the same refusal for Dying Light 2 and Phantom Blade Zero. That commit was
still upstream `develop`'s tip on 2026-11-05.

Only a pair within a percent of each other is shared this way. A depth of a genuinely different
resolution is left alone on purpose. At 0.67 the game also hands over a 1368x1024 depth for a few
seconds at a time (below); that is a 4:3 picture against the motion vectors' 16:9, so declaring a shared
area would give XeFG two inputs that agree on their size and disagree on their geometry -- a wrong
picture instead of a refusal, and much harder to notice.

## Open: the short-lived 1368x1024 depth

One session (09:57:05-09:57:19 on 2026-11-03) logged 24 frames whose depth was 1368x1024 paired with
1367x769 motion vectors: 40 refused frames inside a 2s window that otherwise generated everything. Not
explained yet. It looks like a pass in the game that resolves depth at another geometry (1024 = 1152 x 8/9)
rather than a mip of the 1368x772 buffer. Whoever picks this up should first find out what the player was
doing in those seconds -- an upscaler change, a menu, or normal play decides whether it is worth chasing.

## Reading it out of a log

`C:\Optiscaler\ReShade.log`, written by `xess_mod::WriteLog`. The three lines that answer "is XMX
generating, and if not why":

- `xmx arguments ...` -- every distinct set of resources the mode hands XeFG, with their sizes. One line
  per change, not per frame.
- `xmx refused execute refused: ... previous ... | next ...` -- what XeFG was given when it said no,
  also one line per distinct layout.
- `xmx rates over 2000ms asked N ... generated N ... refused N | pairs N ... | execute Nus avg` -- the
  per-2s truth, unaffected by the 48-line cap the generator's own log has.

The log reaches tens of megabytes in a session, so read its tail in node rather than through a content
search: grep against it fails past the 20 MB cap.

## Against upstream

Every rule here came from upstream `MFGAdaUnlock-RenoDx` @ `3b8889f`, so "what does upstream do" is that commit,
not this repository. Compare against `develop`, never `main` (which only ever takes `README.md` edits).
Last checked 2026-11-05: identical, so all of the above is current. `AGENTS.md` has the one-line check.
