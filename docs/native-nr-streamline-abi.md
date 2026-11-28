# Native DLSS-NR live options

Verified locally on 2026-11-28 against Naraka's
`D:/Naraka/program/Streamline/dlss5/sl.dlss_nr.dll`. This is a private,
version-gated adapter, not a claim about a published NVIDIA DLSS-NR SDK.

Provider identity: PE timestamp `0x6ac513cc`, SizeOfImage `0x63000`.
`slGetPluginFunction` returns RVA `0x40180` for `slDLSSNRSetOptions`.
The function accepts `const sl::ViewportHandle&` and an options reference;
it copies the 40-byte viewport, links options at `next`, and calls set-data
at RVA `0x3ff20`. The existing pinned Streamline headers supply BaseStructure
and ViewportHandle; their x64 sizes are asserted.

Options GUID: `29dfdfe0-273a-4e72-b492-2dc823d5b1ad`. The base is 32 bytes:
next at `+0`, GUID at `+8`, size_t version at `+0x18`. The set-data function
reads `+0x38` only for version >= 2, normalizes to version 4 and stores a
per-viewport 0x50-byte copy. Version 2 ends at `+0x44` (size 0x48); version 3
adds a uint32 at `+0x44` (size 0x48); version 4 adds a uint32 at `+0x48`
(size 0x50). Only versions 2-4 are accepted by Uplift.

The options `+0x38` word goes to the evaluation packet `+0x10c`: caller
RVA `0x3ecfd` -> stack `rbp+0x13c`, packet starts at `rbp+0x30`;
RVA `0x42eea` preserves that offset in the backend packet. Backend RVA
`0x3ab23` compares it with the cached preset and releases the old feature
on a change. RVA `0x3adbe` passes it directly to NGX key
`DLSSNR.Hint.Render.Preset`. Thus the provider owns feature recreation.

In the supplied `nvngx_dlssnr.dll`, the preset-key read at RVA `0x17cfd`
is followed by `btr ecx, 0x1f` at `0x17d1a`. Bit 31 is stripped before
network selection. Uplift preserves that bit from the game while selecting
its existing network IDs 1 (Legacy) or 2 (Small FP8). All other panel IDs
restore the last unmodified game preset, including its flag.

Both existing slGetFeatureFunction observers wrap only this verified
provider. Game calls are copied, never written in place. A panel change
replays cached original game options once before public slEvaluateFeature;
it does not manually replace NGX handles. Unknown versions/providers are
forwarded unchanged. Extension chains are used only within the live game
call; replay waits for the next game call if either chain contains a borrowed
next pointer. Only the evaluated NR viewport's options are replayed. A
successful NR `slFreeResources` drops that viewport's cached options; all
cached pointers/options are cleared before `slShutdown`. A
rejected override restores game options and is not retried for the same
selection until it changes. If no original game options were observed,
the log reports waiting; no options structure is fabricated.

Diagnostics: `Native NR preset:` covers panel selection, actual Streamline
submission, and NGX create input/result. A successful SetOptions means only
that options were accepted; a subsequent successful NGX create is needed
to verify recreation. GPU output still requires an in-game check.
