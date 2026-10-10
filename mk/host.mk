# Native diagnostic harness, independent of Dreamcast/KallistiOS.
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

HOST_CC ?= cc
HOST_CFLAGS ?= -O2 -g -Wall -Wno-unused-function
HOST_CPPFLAGS ?=
HOST_LDFLAGS ?=
BUILD_DIR := build/host
PROGRAM := $(BUILD_DIR)/ngcd_host
SOURCES := src/host/main.c src/host/cpu_musashi.c src/host/z80_zazu.c \
           src/core/ngcd.c src/core/mem.c src/core/cd.c src/core/video.c src/core/ym2610.c \
           third_party/z80/z80.c third_party/musashi/m68kcpu.c \
           third_party/musashi/m68kops.c third_party/musashi/m68kdasm.c \
           third_party/musashi/softfloat/softfloat.c
OBJECTS := $(addprefix $(BUILD_DIR)/,$(SOURCES:.c=.o))

.PHONY: all
all: $(PROGRAM)

$(PROGRAM): $(OBJECTS) mk/host.mk
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_LDFLAGS) -o $@ $(OBJECTS) -lm

$(BUILD_DIR)/%.o: %.c mk/host.mk
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CPPFLAGS) $(HOST_CFLAGS) -Isrc/core -Ithird_party/musashi -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d)
