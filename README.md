# ngcd-dc — Neo Geo CD emulator for the Sega Dreamcast

[Русская версия](README.ru.md)

A low-level Neo Geo CD (CDZ) emulator for the Sega Dreamcast, built with
KallistiOS. It runs the real CDZ BIOS and unmodified games: the LC8951
CD-ROM decoder, the CD drive protocol, DMA, the LSPC video chip, the YM2610
and the sound Z80 are emulated. The machine's behaviour follows NeoCD /
neocd_libretro (Fabrice Martinez), rewritten in C. The 68000 runs on FAME
SH4, the Z80 on RAZE-SH4, and the picture is drawn by the Dreamcast's PVR.

BIOS files and games are not included.

## Status

| Game | Status |
|---|---|
| Samurai Shodown / Samurai Spirits | runs |
| Samurai Shodown II | runs |
| Samurai Shodown III | runs |
| Samurai Shodown IV | runs in the PC build and the SH4 code path under qemu; not tried on a Dreamcast |
| Shinsetsu Samurai Spirits: Bushidō Retsuden (English translation 1.10) | runs |
| Metal Slug | runs; use `cpu=18` (see [NGCD.CFG](#ngcdcfg)) to avoid the game's own slowdown |
| Neo Turf Masters | runs |

"Runs" means tested on a Dreamcast or in Flycast: menus, loading, gameplay.

## Features

- Real CDZ BIOS; no BIOS call hooks, no per-game patches.
- 240p video (320x240 progressive), the 320x224 picture shown 1:1, so
  shadows and effects that flicker every other frame look as on the real
  console.
- PVR rendering: a cache of 4096 16x16 sprite tile textures in video
  memory, each frame is a list of textured rectangles; the fix layer is an
  8-bit texture using the PVR palette, only changed 8x8 cells are rewritten.
- CD audio from the Dreamcast disc: CD-DA tracks are converted to IMA ADPCM
  (`.NGA`) beforehand and streamed by a background thread.
- Sound latency ~30–40 ms (YM2610 at 22050 Hz, resampled to 44100 Hz and
  mixed with the CD audio; the stream is refilled by its own thread).
- Saves: the CDZ's 8 KB backup RAM goes to the first VMU (`NGCDBAK`),
  ~2 s after it changes. Shared by all games, as on the CDZ.
- Two players on two controllers.
- Optional overclocked 68000 per game (`NGCD.CFG`).
- Several games on one disc with a menu ([anthology](#several-games-on-one-disc)).

## Building

Requires KallistiOS (tested with 2.3.0) with its environment set up
(`KOS_BASE`), plus `m4` (the RAZE core is generated from its m4 sources).

    make clean && make RELEASE=1     # release: ngcd.elf, no console output
    make                             # debug: KOS banner, perf log every 2 s
    make CPU_MHZ=18                  # 68000 at 18 MHz unless NGCD.CFG says otherwise

Run `make clean` when switching between release and debug, or after
changing `CPU_MHZ`. Then `ngcd.elf` → scramble → `1ST_READ.BIN`.

## Preparing a disc

One game per disc (or several, see below). A Neo Geo CD image (CUE + BIN:
a MODE1/2352 data track and CD-DA tracks) is converted by `tools/ngcdconv`:

    cc -O2 -o ngcdconv tools/ngcdconv/ngcdconv.c
    ./ngcdconv "game.cue" out

It writes:

- `TOC.BIN` — track and index table;
- `DATA.ISO` — the data track, 2048-byte sectors;
- `TRKnn.NGA` — CD-DA tracks as IMA ADPCM 44.1 kHz stereo, one block per
  sector (playback can start at any sector).

| File in the disc root | From | Size |
|---|---|---|
| `1ST_READ.BIN` | `ngcd.elf`, scrambled | |
| `NEOCD.BIN` | CDZ BIOS `neocd.bin` (either byte order) | 512 KB |
| `NG_LO.ROM` | `000-lo.lo` / `ng-lo.rom`; optional, generated if missing | 64 KB |
| `TOC.BIN`, `DATA.ISO`, `TRKnn.NGA` | `ngcdconv` | |
| `NGCD.CFG` | optional settings | |

### NGCD.CFG

A text file in the game's folder, `key=value` lines:

    cpu=18

`cpu` — 68000 clock in MHz: 12 (the real console, default), 15, 18, 21 or
24. Games that slow down in heavy scenes on the real hardware (Metal Slug
with bosses and many explosions) slow down much less. Timers, video, sound
and CD keep the real speed; only the 68000 gets more cycles per frame. The
cost is Dreamcast CPU time: in Metal Slug a frame's emulation takes ~20 %
longer at 18 MHz and ~40 % at 24 MHz. 18 MHz is a good choice; at 24 MHz the
heaviest scenes may skip frames.

## Several games on one disc

A menu drawn with the BIOS font chooses the game; every game lives in its
own folder. There is no way back from a game to the menu (reset the
console).

    make clean && make RELEASE=1 anthology

`build/anthology/` gets `1ST_READ.BIN` (the menu, scrambled) and
`NGCD.BIN` (the emulator, a plain binary). Disc root:

```
1ST_READ.BIN  IP.BIN  MENU.TXT  NGCD.BIN
NEOCD.BIN  NG_LO.ROM          BIOS, shared by all games
SSI/    TOC.BIN DATA.ISO TRKnn.NGA [NGCD.CFG]
SSII/   ...
SSIII/  ...
```

`MENU.TXT` (example in `menu/MENU.TXT`):

```
@SAMURAI SHODOWN ANTHOLOGY
SSI|Samurai Spirits
SSII|Samurai Spirits II
SSIII|Samurai Spirits III
```

`folder|title[|binary]`, the binary defaults to `NGCD.BIN`; `@` sets the
header, `#` starts a comment. The menu loads `NGCD.BIN`, writes the game's
folder (`/cd/SSII/`) into the image after the `NGCD-DISC-DIR=` marker and
starts it. The emulator then reads the game's files from that folder and
looks for `NEOCD.BIN` / `NG_LO.ROM` there first, then in the root.
`NGCD.BIN` written to a disc as `1ST_READ.BIN` without the menu uses the
disc root, as usual.

## Controls

| Dreamcast | Neo Geo |
|---|---|
| D-pad | joystick |
| A / B / X / Y | A / B / C / D |
| Start | Start |
| L trigger | Select |
| Start+A+B+X+Y | quit |

The second controller is player 2.

## Source layout

```
src/core/   Neo Geo CD core, portable C
  ngcd.c      frame loop, timers, interrupts, sound
  mem.c       68000 memory map, DMA, 0xE00000 window
  cd.c        LC8951, drive protocol, TOC
  video.c     PVR rectangle builder (ngcd_build_quads) and a software
              renderer (PC build only)
  ym2610.c    YM2610 (MAME fm.c via neocd_libretro, converted to C)
  nga.h       CD audio format
src/dc/     Dreamcast front end
  main.c         main loop, video, controllers, audio thread
  machine_ngcd.c disc files, CD audio thread, VMU, sound mixing, NGCD.CFG
  cpu_fame.c     68000 on FAME
  z80_raze.c     Z80 on RAZE
  video_pvr.c    PVR rendering, tile cache, fix layer
src/cpu/    SH4 assembler cores: fame/ (68000), raze/ (Z80, m4 sources)
host/       PC build (Musashi + a C Z80) for comparing with a reference
menu/       anthology menu and an example MENU.TXT
tools/      ngcdconv
docs/       NOTES.md — developer notes (in Russian): decisions, bugs found,
            measurements
```

The debug build prints every 2 s: `ngcd/frame us: …` (emulation, Z80,
YM2610, drawing and audio time, CD sectors, audio underruns),
`ngcd/pvr: …` (rectangles, tiles converted, cache overflows, dropped
rectangles) and `perf/frame: …` (emulation, drawing, PVR wait, frames shown
out of 120).

## PC build

`host/` builds the same core with Musashi instead of FAME and a C Z80
instead of RAZE. It writes frames (PPM) and audio (WAV); this is how the
core was compared with neocd_libretro frame by frame.

    cd host && make
    ./ngcd_host neocd.bin ng-lo.rom|- disc_folder frames every out_dir [script]

`script` is a list of button presses `frame:keys,frame:keys`, keys
`U D L R a b c d S s` (S Start, s Select), each held for 8 frames.
`NGCD_CPU_MHZ=18` in the environment sets the 68000 clock.

## Fixes to third-party cores

- **FAME** (`src/cpu/fame/fame.s`): `DIVS` flagged an overflow on almost any
  division with a remainder (sprites disappeared in games), `CHK` compared
  unsigned. Both instructions were rewritten.
- **RAZE**: generated without `NO_PC_LIMIT`, so the Z80 program counter
  wraps at 16 bits again. With the flag Neo Turf Masters crashed while the
  Z80 ran garbage during its program upload.
- **qemu-sh4** (used for testing only): versions 8.2 and 9.2 set the T flag
  wrongly in `rotl` / `rotr`, which breaks FAME under qemu.

## Known limitations

- Vertically shrunk sprites: the PVR stretches tile rows over a rectangle,
  which can differ from the real picture by one line.
- More than 9000 rectangles in a frame, or a tile cache full of tiles used
  by the frame: the extra rectangles are not drawn (`rects dropped` in the
  log). Not seen in the tested games.
- Sprite table changes in the middle of the picture (raster effects) are
  not drawn line by line; the frame uses the tables as they are at its end.
- A ~0.05 s gap in the sound when the CD audio track changes.
- Fine dithering (one-pixel checkerboards, e.g. Samurai Shodown III's life
  bars) shows rainbow artefacts over composite / RF, as any sharp 1:1
  picture does; S-Video, VGA or RGB are clean.
- Faster CD reading exists in the core (`cd_speed_mul`) but is off: not
  verified that games behave correctly with it.

## Credits and licences

See [THIRD_PARTY.md](THIRD_PARTY.md) for the full list and terms.

- Machine behaviour after NeoCD / neocd_libretro by Fabrice Martinez
  (LGPL-3.0); the core is rewritten in C.
- YM2610 — Jarek Burczynski and Tatsuyuki Satoh (MAME `fm.c`) via
  neocd_libretro, GPL-2.0-or-later.
- FAME SH4 2.1 — Oscar Orallo Pelaez, SH4 assembler from the MAME4ALL
  Dreamcast sources.
- RAZE Z80 core, by Richard Mitton (richard.mitton@bigfoot.com); SH4 version
  by Oscar Orallo.
- Musashi (PC build only) — Karl Stenerud, MIT.
- Z80 (PC build only) — Nicolas Allemand (superzazu), MIT.
- PVR rendering inspired by NEO4ALL's GL path.

The project as a whole is distributed under the GNU General Public License
v3.0 or later ([LICENSE](LICENSE)), as required by the GPL-2.0-or-later
YM2610 code and the LGPL-3.0 neocd_libretro work it follows. The FAME and
RAZE cores keep their own terms (see THIRD_PARTY.md); in particular,
programs using RAZE may not be sold without written permission from its
author.

## History

The emulator started as a port of a single game, Shinsetsu Samurai
Spirits: Bushidō Retsuden, inside dc-poker (arcade ports for the
Dreamcast), and was split out into its own project when the core turned out
to be general. See [CHANGELOG.md](CHANGELOG.md).
