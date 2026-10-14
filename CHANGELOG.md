# Changelog

PC-build frames and audio were checked byte for byte against the previous
revision after every change that was not meant to alter emulation.

## r11
- Anthology: the L+R+Start return to the menu was removed (it crashed in
  `arch_shutdown` on a real console); `MENU.BIN` is no longer needed on the
  disc. Reset the console to get back to the menu.

## r10
- Several games on one disc: `menu/menu.c` (BIOS font, reads `MENU.TXT`),
  every game in its own folder. The menu writes the folder into the loaded
  emulator image after the `NGCD-DISC-DIR=` marker.
- The emulator looks for the game files in that folder, for
  `NEOCD.BIN` / `NG_LO.ROM` in the folder and then in the disc root.

## r9
- Sound: effects (menu and character selection) were ~150 ms late and,
  after a selection followed by disc reading, repeated several times. The
  stream is now refilled by its own thread every ~5 ms, the AICA ring is
  halved and the YM2610 queue is shorter: ~30–40 ms latency; when the main
  loop stalls the YM output goes quiet instead of repeating.
- The core's YM2610 sample queue is single-producer / single-consumer.

## r8
- 68000 clock setting: `cpu=` in `NGCD.CFG`, `make CPU_MHZ=`, `NGCD_CPU_MHZ`
  for the PC build. 18 MHz removes most of Metal Slug's own slowdown.
  Default 12 MHz (unchanged emulation).

## r7
- Neo Turf Masters: the background flashed for a frame (grass instead of
  sky, water instead of grass). Writes for the next frame that start in the
  last rows of the picture no longer reach the current frame.

## r6
- The fix layer is an 8-bit twiddled texture using the PVR palette; only
  changed 8x8 cells are rewritten and a palette change only rewrites 256
  palette entries. Before, the whole layer was redrawn and uploaded on any
  change (up to 6.6 ms per frame during loading).

## r5
- The software renderer was removed from the Dreamcast build (the R
  trigger no longer switches it, no software fallback frame); 512 KB of
  video memory and 224 KB of RAM freed.

## r4
- RAZE generated without `NO_PC_LIMIT` (Neo Turf Masters crashed).
- Fast DMA into the 0xE00000 window (frames of up to 0.3 s while loading).

## r1–r3
- Split out of dc-poker (where it was built as `ssrpg.elf`, revision r13)
  as a standalone Neo Geo CD emulator: new layout, `ngcd.elf`, `ngcd/…` log
  prefixes. Emulation unchanged.
