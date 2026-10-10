.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
HOST_CC ?= cc
HOST_CFLAGS ?= -O2 -Wall -Wextra
MINGW_CC ?= x86_64-w64-mingw32-gcc

.PHONY: all windows clean
all: ngcdconv

ngcdconv: ngcdconv.c Makefile
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $<

# Optional rebuild of the Windows x86-64 converter with MinGW-w64.
windows:
	$(MINGW_CC) -O2 -Wall -Wextra -o ngcdconv.exe ngcdconv.c

clean:
	rm -f ngcdconv
