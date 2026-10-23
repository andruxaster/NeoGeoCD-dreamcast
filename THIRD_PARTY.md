# Third-party code and terms

| Component | Files | Author | Terms |
|---|---|---|---|
| NeoCD / neocd_libretro (behaviour reference) | `src/core/*.c` follow its design; rewritten in C | Fabrice Martinez | LGPL-3.0 |
| YM2610 (MAME `fm.c`) | `src/core/ym2610.c`, `ym2610.h` | Jarek Burczynski, Tatsuyuki Satoh (via neocd_libretro) | GPL-2.0-or-later, see the file header |
| FAME SH4 2.1 (68000) | `src/cpu/fame/fame.s` (DIVS and CHK fixed), `fame_api.h` | Oscar Orallo Pelaez; SH4 assembler from the MAME4ALL Dreamcast sources | the FAME author's terms (not included in the MAME4ALL archive) |
| RAZE-SH4 (Z80) | `src/cpu/raze/raze.s.m4`, `raze.inc`, `raze.reg`, `raze.html` (unchanged, from MAME4ALL `src/raze/faze`); `raze_api.h` | Richard Mitton; SH4 rewrite by Oscar Orallo | see `raze.html`: use in any program, but programs may not be sold without written permission from Richard Mitton; the documentation must mention "RAZE Z80 core, by Richard Mitton (richard.mitton@bigfoot.com)" |
| FAME C 1.24 (68000, PS2 build) | `src/cpu/famec/` (PicoDrive's copy; Neo Geo vectors via `famec_irq_vector`, PicoDrive's Sega CD hack off) | Oscar Orallo Pelaez; PicoDrive changes by notaz, irixxxx | the FAME author's terms, `src/cpu/famec/fame.html` |
| Musashi (68000, PC build only) | `host/musashi/` | Karl Stenerud | MIT, `host/musashi/LICENSE.txt` |
| Z80 (PC and PS2 builds) | `host/z80.c`, `host/z80.h` | Nicolas Allemand (superzazu) | MIT, `host/LICENSE.z80` |

RAZE Z80 core, by Richard Mitton (richard.mitton@bigfoot.com).

The rest of the code (`src/dc/`, `src/core/` glue, `menu/`, `tools/`,
`host/main.c`, `host/cpu_musashi.c`, `host/z80_zazu.c`) is part of ngcd-dc
and distributed under the GNU GPL v3.0 or later (`LICENSE`).

Note: fame.html allows FAME to be distributed freely in unmodified form; the
C version here carries PicoDrive's changes and two small ones of ours (the
vector of the pending interrupt, the Sega CD TAS hack switched off), as
PicoDrive itself ships a modified FAME C.

Note: the FAME and RAZE terms add conditions (attribution, no sale without
permission) that go beyond the GPL. They are kept as their authors set
them, as in MAME4ALL, which combines the same cores; anyone
redistributing ngcd-dc commercially needs the RAZE author's permission and
should check the FAME terms.

No BIOS, ROM or game data is included. `neocd.bin` (the CDZ BIOS) and the
games must come from the user's own hardware and discs.
