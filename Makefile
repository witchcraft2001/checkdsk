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
#
# DATA_LOC differs per target because chkdsk (FAT16+FAT32) is fuller
# than chkdsk12 (FAT12-only) -- see the per-target dispatch below.
# Both stay above 0xB000 to preserve enough stack at 0xBFFF.
CODE_LOC   ?= 0x4100
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

# DATA_LOC layout, per target:
#   chkdsk   -- DATA_LOC=0xBB80. Hard constraints (in order):
#               (1) DATA_LOC must be at or above end of _HOME, or
#                   else _DATA storage overlaps math runtime helpers
#                   (_mullong/_modulong/etc. that the linker placed
#                   in _HOME) -- the first disk_read into g_sect_a
#                   then trashes those routines and the next 32-bit
#                   multiply crashes mid-init.
#               (2) DATA_LOC + sizeof(_DATA) must EXCEED end of
#                   _GSFINAL or gsinit's LDIR overwrites the
#                   gsinit_clear_bss instructions following LDIR --
#                   PC runs into corrupted bytes and crashes at
#                   startup.
#               Linker order is _CODE, _HOME (490), _INITIALIZER
#               (= sizeof _INITIALIZED), _GSINIT (37), _GSFINAL (1).
#               At current sizes (_CODE ~30856, _DATA 570 with sums[]
#               and step ent[] on stack, _INITIALIZED 379, depth=7,
#               no LFN) _HOME ends at 0xBB71 and _GSFINAL near 0xBD1B;
#               DATA_LOC=0xBB80 puts _DATA at 0xBB80..0xBDA5 and
#               _INITIALIZED at 0xBDA6..0xBF20, leaving ~222 bytes
#               of stack -- adequate since sums[] (128) and ent[] (32)
#               are now on the stack only during their own phases,
#               and the deepest BIOS chain peaks well under 200 bytes.
#               When _CODE grows, raise DATA_LOC by the same delta.
#   chkdsk12 -- DATA_LOC=0xBA00. Smaller binary (FAT12 only), keeps
#               the lower DATA_LOC and a generous ~570 bytes of stack.
chkdsk:
	@$(MAKE) APP=chkdsk CHKDSK_FAT12=0 CHKDSK_FAT16=1 CHKDSK_FAT32=1 DATA_LOC=0xB900

chkdsk12:
	@$(MAKE) APP=chkdsk12 CHKDSK_FAT12=1 CHKDSK_FAT16=0 CHKDSK_FAT32=0 DATA_LOC=0xBA00

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
