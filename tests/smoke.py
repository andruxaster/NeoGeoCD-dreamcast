#!/usr/bin/env python3
"""Integration checks without copyrighted BIOS/game data or KallistiOS."""
from pathlib import Path
import struct
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
CONVERTER = ROOT / "tools/ngcdconv/ngcdconv"
HOST = ROOT / "build/host/ngcd_host"


def run(*args):
    return subprocess.run([str(a) for a in args], check=True,
                          capture_output=True, text=True, timeout=60)


def main():
    with tempfile.TemporaryDirectory(prefix="ngcd-dc smoke ") as directory:
        work = Path(directory)
        raw = b"".join(bytes(16) + bytes([n]) * 2048 + bytes(288) for n in range(10))
        (work / "single.bin").write_bytes(raw + bytes(12 * 2352))
        (work / "data.bin").write_bytes(raw)
        (work / "audio.bin").write_bytes(bytes(12 * 2352))
        (work / "single.cue").write_text(
            'FILE "single.bin" BINARY\n TRACK 01 MODE1/2352\n'
            ' INDEX 01 00:00:00\n INDEX 02 00:00:05\n TRACK 02 AUDIO\n'
            ' INDEX 00 00:00:10\n INDEX 01 00:00:12\n', encoding="ascii")
        (work / "multi.cue").write_text(
            'FILE "data.bin" BINARY\n TRACK 01 MODE1/2352\n'
            ' INDEX 01 00:00:00\n INDEX 02 00:00:05\n'
            'FILE "audio.bin" BINARY\n TRACK 02 AUDIO\n'
            ' INDEX 00 00:00:00\n INDEX 01 00:00:02\n', encoding="ascii")
        for name in ("single", "multi"):
            run(CONVERTER, work / (name + ".cue"), work / name)
        for name in ("TOC.BIN", "DATA.ISO", "TRK02.NGA"):
            assert (work / "single" / name).read_bytes() == (work / "multi" / name).read_bytes(), name
        assert (work / "single/DATA.ISO").read_bytes() == b"".join(bytes([n]) * 2048 for n in range(10))
        toc = (work / "single/TOC.BIN").read_bytes()
        assert toc[:8] == b"NGTOC1\0\0"
        assert struct.unpack_from("<II", toc, 8) == (4, 22)
        assert [struct.unpack_from("<BBBBII", toc, 20 + i * 12) for i in range(4)] == [
            (1, 1, 1, 0, 0, 5), (1, 2, 1, 0, 5, 5),
            (2, 0, 0, 0, 10, 2), (2, 1, 0, 0, 12, 10)]
        audio = (work / "single/TRK02.NGA").read_bytes()
        assert audio[:16] == struct.pack("<4sIHHI", b"NGA1", 44100, 2, 0, 10)
        assert len(audio) == 16 + 10 * 596
        assert not any(audio[16:]), "silence must stay silent in NGA"
        print("PASS converter: single/multiple BIN, extra INDEX, INDEX 00, sector contents, NGA layout")

        # Synthetic 68000 program: set background palette, enable video, loop.
        # No original BIOS bytes are used. This checks execution, mapped writes,
        # frame scheduling, software video and WAV output through the host app.
        bios = bytearray(0x80000)
        struct.pack_into(">II", bios, 0, 0x10FF00, 0xC00100)
        code = (struct.pack(">HHI", 0x33FC, 0x0F00, 0x401FFE)
                + struct.pack(">HHI", 0x13FC, 0x0001, 0xFF0119)
                + b"\x60\xfe")
        bios[0x100:0x100 + len(code)] = code
        (work / "synthetic-bios.bin").write_bytes(bios)
        out = work / "frames"
        out.mkdir()
        run(HOST, work / "synthetic-bios.bin", "-", work / "single", 120, 60, out)
        # Neo Geo 0x0F00 maps to RGB565 0xF020 (red's low bit is unset).
        expected = b"P6\n320 224\n255\n" + bytes((240, 4, 0)) * (320 * 224)
        for name in ("f000060.ppm", "f000120.ppm"):
            assert (out / name).read_bytes() == expected, name
        with wave.open(str(out / "audio.wav"), "rb") as wav:
            assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (2, 2, 44100)
            assert abs(wav.getnframes() - 88792) <= 2
            assert not any(wav.readframes(wav.getnframes()))
        print("PASS host: 120 frames, 68000 program, palette/video writes, exact PPM output, stereo WAV")
    print("These checks do not establish game compatibility or Dreamcast performance.")


if __name__ == "__main__":
    main()
