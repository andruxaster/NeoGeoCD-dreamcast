# Changelog

PC-build frames and audio were checked byte for byte against the previous
revision after every change that was not meant to alter emulation.

## r16
- PlayStation 2 trial build (`Makefile.ps2`, `src/ps2/`): the same core
  with FAME C (`src/cpu/famec/`, from PicoDrive, with the Neo Geo's own
  interrupt vectors) and the PC build's Z80; software renderer into a GS
  texture (`NGCD_PAL_GS`: palette in GS CT16 order), audsrv sound with the
  Dreamcast build's resampler, libpad, stdio files, backup RAM on the
  memory card, NGCD.CFG `video`, `tv`, `filter`. Dreamcast build unchanged.

## r15
- Raster timer: when a register write arms a timer to fire before the end
  of the 68000 slice being run, the slice now ends there. Before, the
  raster interrupts until the slice end were lost: Neo Turf Masters'
  lake started 0-6 lines late, a different line from frame to frame (the
  picture jittered). Now its split line is the same in every frame and
  the same as in MAME (checked with a write trace on both). Samurai
  Shodown II/III/IV, Metal Slug, Riding Hero: PC-build pictures byte for
  byte the same as r14 over 20000 frames.
- PC build: `NGCD_FROM` (first frame to dump), `NGCD_RLOG`/`NGCD_RLOGN`
  (raster log), `NGCD_WT` (LSPC write trace).

## r14
- Raster effects. Sprite table (SCB2-4) words changed while the picture is
  drawn are logged with their line; in frames where the game also takes
  raster interrupts during the picture, the sprites they belong to (with
  their sticky chains) are drawn per run of lines with the tables of that
  run, the others once as before (both in the PVR rectangles and in the
  software renderer). Riding Hero's road and Neo Turf Masters' lake and sky
  now match MAME; frames without raster interrupts are drawn as in r13
  (Samurai Shodown II, Metal Slug: PC-build pictures byte for byte the
  same). SH4 cost of building the rectangles in Riding Hero's race: 0.22M
  instructions a frame instead of 0.10M.
- Shrunk sprites with the 96-per-line limit reached: rows outside the
  range being drawn are no longer walked.
- Debug log: raster frames in `ngcd/pvr`.

## r13
- `NGCD.CFG`: `region=jp|us|eu` (the BIOS reads the console's region;
  Samurai Shodown II shows its US title with `us`), `video=640` (640x480
  screen with every pixel 2x2, for VGA and emulators that smooth a 320-wide
  screen; textures stay point sampled), `stick=0|1` (analog stick as the
  joystick, on by default).
- PC build: `NGCD_REGION=0|1|2`.

## r12
- The 68000's wait loops are skipped: a two-instruction loop that only
  reads RAM (`tst`/`btst`/`cmpi` + `beq`/`bne`, `bclr` + `beq`) found at the
  end of a slice is not executed until the 68000's interrupt line changes;
  running slices are at most 1/32 frame. `idle=0` in `NGCD.CFG`
  (`NGCD_IDLE=0` for the PC build) turns it off. Metal Slug 2 at 24 MHz:
  1.13M SH4 instructions per frame on average instead of 2.02M (1.39M at
  12 MHz without skipping). Emulation timing changes slightly, so PC-build
  frames differ from r11 in games with such loops.
- Debug log: 68000 clock and skipped wait slices in `ngcd/frame`.

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
