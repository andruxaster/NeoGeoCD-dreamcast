# NGCD-DC

A standalone **Neo Geo CD emulator for Sega Dreamcast**, version 0.1.0.

The emulator uses an actual Neo Geo CD BIOS and emulates the 68000, Z80,
LC8951, CD drive, DMA, LSPC, and YM2610. Support is not limited to Samurai
Spirits RPG, although that game was the primary focus of the original version.
There is no game selection menu yet: at startup, the emulator loads a single
prepared disc from `/cd/`.

## Features

- Dreamcast: FAME SH4 68000 and RAZE-SH4 Z80; the FAME fixes from r13 are retained.
- PVR sprite rendering, a cache of 4,096 16×16 textures, and a separate FIX layer.
  Automatically falls back to software rendering if the texture cache or
  rectangle list overflows. The R button toggles between PVR and software rendering.
- 320×240 video mode with a 320×224 image, without filtering or scaling.
- Two controllers; 8 KB of backup RAM stored on the first VMU in `NGCDBAK`.
- YM2610 at 22,050 Hz, interpolated to 44,100 Hz; stereo CD music encoded as
  IMA ADPCM, read on a separate thread with approximately 0.85 seconds of buffering.
- A PC test harness using Musashi and superzazu Z80, with PPM frame and WAV output.

## Building for Dreamcast

You need KallistiOS, an SH4 toolchain, and a configured KOS environment.
The same KOS APIs as in the original r13 are used; the exact minimum required
version has not been determined. Build from the project root:

```sh
source /opt/toolchains/dc/kos/environ.sh
make -j4
```

The output is `ngcd-dc.elf`. The debug build prints emulation, rendering,
and audio timings, PVR statistics, and audio underrun information.

```sh
make -j4 RELEASE=1          # quiet build without profiling
make bin RELEASE=1         # also generate a scrambled 1ST_READ.BIN
```

Debug and release object files are kept separately in `build/dc-debug` and
`build/dc-release`, so `make clean` is not needed when switching between these
modes. Run `make clean` first if you change the toolchain or `EXTRA_CFLAGS`.
The `make release` target is equivalent to `make RELEASE=1`.

`make bin` requires a built `KOS_BASE/utils/scramble/scramble` executable.
You can specify a different path with `SCRAMBLE=/path/to/scramble`.
`1ST_READ.BIN` is a bootable executable, **not a ready-to-use CDI image**.

## Preparing a Neo Geo CD Disc

The converter accepts CUE + BIN images with a `MODE1/2352` data track and
`AUDIO` tracks, either as a single combined BIN or separate BIN files per track.

Linux / another environment with a C compiler:

```sh
make tools
./tools/ngcdconv/ngcdconv "/path/to/Game.cue" disc
```

Windows PowerShell — a prebuilt x86-64 converter is carried over from r13:

```powershell
.\tools\ngcdconv\ngcdconv.exe "D:\NeoGeoCD\Game.cue" disc
```

To rebuild the Windows converter using MinGW-w64:

```sh
make -C tools/ngcdconv windows
```

The converter produces `TOC.BIN`, `DATA.ISO`, and `TRKnn.NGA`. NGA uses lossy
IMA ADPCM compression; blocks are independent, with one block per CD sector.
The file formats are fully compatible with r13, so previously prepared discs
do not need to be converted again.

Place the following files in the root of the future Dreamcast boot disc filesystem:

| File | Purpose |
| --- | --- |
| `1ST_READ.BIN` | Output of `make bin RELEASE=1` |
| `NEOCD.BIN` | 512 KB CDZ BIOS; both byte orders are supported |
| `NG_LO.ROM` | 64 KB Y-zoom table; optional, generated if absent |
| `TOC.BIN` | Track table produced by the converter |
| `DATA.ISO` | Data track produced by the converter |
| `TRK02.NGA`, … | All audio tracks produced by the converter |

BIOS and game files are not included in the archive. A self-booting CDI must be
created separately using your preferred Dreamcast disc mastering tool. Use
`ngcd-dc.elf` or the new `1ST_READ.BIN` instead of the old `ssrpg.elf`.

## Controls

| Dreamcast | Neo Geo CD / Action |
| --- | --- |
| D-pad | Directions |
| A / B / X / Y | A / B / C / D |
| Start | Start |
| L | Select |
| R on controller 1 | Toggle PVR ↔ software rendering |
| Start + A + B + X + Y on controller 1 | Exit |

The second controller controls player two. The VMU filename `NGCDBAK`, data
size, and application identifier in the file header are preserved from r13
to maintain compatibility with existing saves. Backup RAM is shared across all games.

## Testing on PC

KallistiOS is not required. You need GNU Make, a C compiler, and Python 3 for tests:

```sh
make -j4 host tools
make test
```

`make test` checks the converter using synthetic disc images and runs 120 frames
of a short synthetic 68000 program. The tests verify palette writes, video
enabling, exact PPM pixel values, and WAV format/silence. Neither a BIOS nor
commercial game data is used for these tests.

Run with your own BIOS and a prepared disc:

```sh
mkdir -p captures
./build/host/ngcd_host NEOCD.BIN - disc 3600 60 captures '120:S,240:a'
```

Arguments: BIOS, zoom ROM or `-`, disc directory, frame count, screenshot
interval (`0` disables screenshots), an existing output directory, and an
optional input script. Frame numbers in the script are zero-based; each
button press lasts 8 frames. Keys: `U D L R a b c d S s`.
Output: `f000060.ppm`, … and `audio.wav`. This harness is not a windowed
emulator and does not test PVR, FAME/RAZE, VMU, or the Dreamcast disc drive.

`HOST_CC`, `HOST_CFLAGS`, `HOST_CPPFLAGS`, and `HOST_LDFLAGS` allow the native
build to be configured independently of KOS variables.

## Project Structure

| Directory | Contents |
| --- | --- |
| `src/core` | Neo Geo CD: memory, CD, DMA, timers, video, YM2610 |
| `src/dreamcast` | KOS main loop, PVR, VMU, CD audio, FAME/RAZE adapters |
| `src/host` | PC command-line test harness and Musashi/superzazu adapters |
| `third_party` | FAME, RAZE, Musashi, superzazu Z80 |
| `tools/ngcdconv` | Converter source, Windows EXE, separate build |
| `mk` | Separate Dreamcast and PC build rules |
| `tests` | Synthetic integration tests |




