# NGCD-DC: standalone Neo Geo CD emulator for Dreamcast.
# Host tools, tests and clean do not require a KallistiOS environment.
.DEFAULT_GOAL := all

.PHONY: all dc release bin host tools test clean help ngcd-dc.elf 1ST_READ.BIN
all: dc

ngcd-dc.elf: dc

1ST_READ.BIN: bin

dc:
	$(MAKE) --no-print-directory -f mk/dreamcast.mk all

release:
	$(MAKE) --no-print-directory -f mk/dreamcast.mk RELEASE=1 all

bin:
	$(MAKE) --no-print-directory -f mk/dreamcast.mk bin

host:
	$(MAKE) --no-print-directory -f mk/host.mk all

tools:
	$(MAKE) --no-print-directory -C tools/ngcdconv all

test: host tools
	python3 tests/smoke.py

clean:
	rm -rf build
	rm -f ngcd-dc.elf 1ST_READ.BIN
	$(MAKE) --no-print-directory -C tools/ngcdconv clean

help:
	@echo 'make                  Build ngcd-dc.elf (Dreamcast, debug/performance log)'
	@echo 'make RELEASE=1        Build ngcd-dc.elf (Dreamcast, quiet release)'
	@echo 'make bin RELEASE=1    Also create scrambled 1ST_READ.BIN'
	@echo 'make host             Build build/host/ngcd_host for the local computer'
	@echo 'make tools            Build tools/ngcdconv/ngcdconv'
	@echo 'make test             Run synthetic host/converter checks (no game ROMs)'
	@echo 'make clean            Remove generated builds; keep bundled Windows tool'
