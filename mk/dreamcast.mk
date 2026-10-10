# Invoked from the project root by the public Makefile.
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

ifndef KOS_BASE
$(error KOS_BASE is not set. Source your KallistiOS environ.sh before building Dreamcast targets)
endif
ifeq ($(wildcard $(KOS_BASE)/Makefile.rules),)
$(error KallistiOS Makefile.rules was not found under KOS_BASE)
endif

include $(KOS_BASE)/Makefile.rules

KOS_CFLAGS += -O3 -Wall $(EXTRA_CFLAGS)
ifeq ($(RELEASE),1)
CONFIG := release
KOS_CFLAGS += -DNGCD_RELEASE -DNDEBUG
else
CONFIG := debug
KOS_CFLAGS += -DNGCD_PERF
endif

BUILD_DIR := build/dc-$(CONFIG)
PROGRAM := $(BUILD_DIR)/ngcd-dc.elf
SOURCES := src/dreamcast/main.c src/dreamcast/machine_ngcd.c \
           src/dreamcast/cpu_fame.c src/dreamcast/z80_raze.c \
           src/dreamcast/video_pvr.c \
           src/core/ngcd.c src/core/mem.c src/core/cd.c \
           src/core/video.c src/core/ym2610.c
ASM_SOURCES := third_party/fame/fame.s third_party/raze/raze.s
OBJECTS := $(addprefix $(BUILD_DIR)/,$(SOURCES:.c=.o) $(ASM_SOURCES:.s=.o))
DEPS := $(addprefix $(BUILD_DIR)/,$(SOURCES:.c=.d))
SCRAMBLE ?= $(KOS_BASE)/utils/scramble/scramble

.PHONY: all bin
all: $(PROGRAM)
	cp $(PROGRAM) ngcd-dc.elf

$(PROGRAM): $(OBJECTS) mk/dreamcast.mk
	kos-cc -o $@ $(OBJECTS) -lm

$(BUILD_DIR)/%.o: %.c mk/dreamcast.mk
	@mkdir -p $(dir $@)
	$(KOS_CC) $(KOS_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.s mk/dreamcast.mk
	@mkdir -p $(dir $@)
	$(KOS_AS) --little -o $@ $<

$(BUILD_DIR)/src/core/ngcd.o: KOS_CFLAGS += -DNGCD_AUDIO_RATE=22050
ifneq ($(RELEASE),1)
$(BUILD_DIR)/src/core/ngcd.o: KOS_CFLAGS += -DNGCD_PROF
endif
# Retain r13's SH4 software-renderer optimization choice.
$(BUILD_DIR)/src/core/video.o: KOS_CFLAGS += -O2

bin: all
	@test -x "$(SCRAMBLE)" || { echo 'scramble tool missing: build KOS utils/scramble or set SCRAMBLE=/path/to/scramble'; exit 1; }
	$(KOS_OBJCOPY) -R .stack -O binary $(PROGRAM) $(BUILD_DIR)/ngcd-dc.bin
	"$(SCRAMBLE)" $(BUILD_DIR)/ngcd-dc.bin 1ST_READ.BIN

-include $(DEPS)
