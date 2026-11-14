# Desktop design

The native interface prioritizes dense tables and inspectable evidence. Its layout uses four dockable panels. Session controls stay in the main menu, and the Inspector remains visible beside the allocation table or memory map.

## Tokens

| Role | Value |
| --- | --- |
| Window background | RGB 14, 18, 22 |
| Foreground | RGB 224, 235, 240 |
| Secondary text | RGB 153, 173, 186 |
| Heading accent | RGB 112, 214, 204 |
| Checkmarks and selected-tab overline | RGB 97, 212, 199 |
| Incomplete history | Amber, with explicit text |
| Body | Installed Segoe UI, 15 logical pixels |
| Hex data | Installed Consolas, 14 logical pixels |
| Control radius | 3 logical pixels |
| Window padding | 12 by 10 logical pixels |
| Item spacing | 8 by 6 logical pixels |

Fonts and style sizes scale to the initial Win32 DPI. Later DPI changes scale the current fonts and style. Dock sizes and table settings persist under `%LOCALAPPDATA%/Heapglass/layout.ini`. State text accompanies color. Tables use clipped rows and horizontal scrolling for wide metadata. No fabricated live rows appear in normal operation.

## Text coverage

The build enables `IMGUI_USE_WCHAR32`. The Segoe UI body atlas requests Latin, Greek, Cyrillic, Hebrew, Arabic, general punctuation and CJK ideograph ranges. At the same body size, it merges CJK glyphs from the first installed font in this order: `msyh.ttc`, `YuGothR.ttc`, `malgun.ttf`, `simsun.ttc`. If installed, Segoe UI Emoji supplies U+1F600 through U+1F6C5. Glyph availability depends on the installed fonts. Consolas remains the Hex + ASCII font.

## Capacity and stack controls

Limits & rules offers rolling sample retention by default or stop at capacity. The Coverage section shows sample evictions, skipped samples, collector losses, target queue depth and queued analysis alongside retained bytes and the selected retention mode. Budget exhaustion displays an explicit sampling-stopped message with the available recovery actions.

Optional stack capture accepts zero through eight frames and is disabled by default. Coverage shows the active frame count and reports native unwinder validation failure. The Inspector's Stack tab lists captured addresses and cached session-scoped symbol evidence, including allocations with no byte samples. Offline sessions use their own saved evidence. Missing names remain unresolved. Hover details expose module-relative and build identity fields with lookup-time provenance.

UI checks queue keyboard and mouse events through ImGui. Native dialog and synthetic DPI checks run on an unshown private desktop without global desktop input.

## Allocation sites and snapshot growth

The Investigation panel adds Allocation sites and Snapshot growth tabs within the existing dock layout. Both use clipped, resizable tables with frozen site columns and headers. Site groups sort by live requested bytes. Selecting a group filters the central allocation table and opens an example in the Inspector; a visible Clear site filter action restores other records.

Growth places byte delta, final bytes, final block count and evidence before the horizontally scrolled details. The summary keeps coverage and the number of repeated-growth sites visible. Text distinguishes uncertain observations, overflow and repeated growth. These reports use the existing body font, table styling and state colors. Group identities use saved addresses; symbol details appear on hover when available.

The sidebar offers metadata-only capture, a timed observation interval, and one pinned baseline. Active timing displays its current interval; its interval slider is disabled until the timer stops. Historical truncation and incomplete allocation coverage use separate text. The limits modal initializes an editable draft from the active settings on opening, preserves exact byte values, and discards cancelled edits.
