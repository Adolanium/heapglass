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
