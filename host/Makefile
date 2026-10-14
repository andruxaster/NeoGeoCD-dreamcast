# ngcd-dc: PC build of the Neo Geo CD core (Musashi 68000 + superzazu Z80),
# for testing against a reference emulator: dumps frames (PPM) and audio (WAV).
#   make            -> ngcd_host
#   ./ngcd_host neocd.bin ng-lo.rom|- discdir frames every outdir [script]
CC     = gcc
CFLAGS = -O2 -g -Wall -Wno-unused-function -I../src/core -Imusashi
CORE   = ../src/core/ngcd.c ../src/core/mem.c ../src/core/cd.c ../src/core/video.c \
         ../src/core/ym2610.c
SRC    = main.c cpu_musashi.c z80_zazu.c z80.c $(CORE) \
         musashi/m68kcpu.c musashi/m68kops.c musashi/m68kdasm.c musashi/softfloat/softfloat.c

ngcd_host: $(SRC) ../src/core/*.h *.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

clean:
	rm -f ngcd_host

.PHONY: clean
