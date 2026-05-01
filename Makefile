#
# checkdsk -- two-target Makefile.
#
# Targets:
#   chkdsk.exe    — FAT16 + FAT32 (default), for IDE / CF / SD partitions.
#   chkdsk12.exe  — FAT12 only, for floppies and very small media.
#
# Building both keeps each binary close to the 32 KB code budget by
# excluding the FAT layouts it doesn't need. Each target has its own
# _build/<target>/ subdirectory so the two .rel sets stay independent.
#
# Common knobs (override on the command line):
#   SDK_DIR    SDCC SDK root (../sdcc45-sprinter-sdk/ by default).
#   VERSION    Version string baked into the banner.
#   LOG        1 = enable internal logging (default 0).
#

VPATH      = src
SRCS       = main.c cmdline.c diskio_dss.c diskio_batch.c sectbuf.c prt.c volume.c mount.c summary.c bpb.c fat.c chain.c bitmap.c dirwalk.c dirent.c scan.c fix.c
SDK_DIR   ?= ../sdcc45-sprinter-sdk/
VERSION   ?= 0.1.$(shell date +%Y%m%d)
LOG       ?= 0

# Memory layout (32 KB: WIN1+WIN2, code + data + stack between
# 0x4100..0xBFFF). _DATA is placed just below the stack ceiling so
# BSS-clearing at startup never reaches into the code area; the gap
# between code-end and data-start is unused fill.
CODE_LOC   ?= 0x4100
DATA_LOC   ?= 0xB900
STACK      ?= 0xBFFF

SDCC       ?= sdcc
PYTHON     ?= python3
INC        = -I$(SDK_DIR)include
CRT0       = $(SDK_DIR)build/crt0.rel
SPRLIB     = $(SDK_DIR)build/sprinter.lib

SDCC_TARGET = -mz80
SDCC_FLAGS  = $(SDCC_TARGET) --max-allocs-per-node 5000 --opt-code-speed

# ----- Top-level dispatch (when invoked without APP=...) -----

ifeq ($(APP),)
.PHONY: all clean chkdsk chkdsk12

all: chkdsk chkdsk12

chkdsk:
	@$(MAKE) APP=chkdsk CHKDSK_FAT12=0 CHKDSK_FAT16=1 CHKDSK_FAT32=1

chkdsk12:
	@$(MAKE) APP=chkdsk12 CHKDSK_FAT12=1 CHKDSK_FAT16=0 CHKDSK_FAT32=0

clean:
	rm -rf _build chkdsk.exe chkdsk12.exe *.ihx *.lk *.map *.mem *.noi *.sym

else

# ----- Per-target build (APP is set) -----

BUILD      = _build/$(APP)

CPPFLAGS   = -DCHKDISK_VERSION='"$(VERSION)"' \
             -DCHKDISK_LOG_ENABLE=$(LOG) \
             -DCHKDSK_FAT12=$(CHKDSK_FAT12) \
             -DCHKDSK_FAT16=$(CHKDSK_FAT16) \
             -DCHKDSK_FAT32=$(CHKDSK_FAT32)

APP_RELS   = $(patsubst %.c,$(BUILD)/%.rel,$(SRCS))

.PHONY: target

target: $(APP).exe

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.rel: %.c | $(BUILD)
	$(SDCC) $(SDCC_FLAGS) $(INC) $(CPPFLAGS) -c -o $@ $<

$(BUILD)/$(APP).ihx: $(CRT0) $(SPRLIB) $(APP_RELS)
	$(SDCC) $(SDCC_TARGET) --no-std-crt0 \
		--code-loc $(CODE_LOC) --data-loc $(DATA_LOC) \
		--max-allocs-per-node 5000 --opt-code-speed \
		$(INC) \
		$(CRT0) $(APP_RELS) \
		-l$(SPRLIB) \
		-o $@

$(APP).exe: $(BUILD)/$(APP).ihx
	$(PYTHON) $(SDK_DIR)tools/ihx2exe.py $< $@ --load $(CODE_LOC) --entry $(CODE_LOC) --stack $(STACK)
	@echo "Built: $@"

endif
