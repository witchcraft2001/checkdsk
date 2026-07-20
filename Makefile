#
# checkdsk -- single-binary Makefile (code-biased win0 extended layout).
#
# One binary, chkdsk.exe, covering FAT12 + FAT16 + FAT32. The FAT-type
# branches are runtime-selected from the BPB; the CHKDSK_FAT* macros used
# to compile out unused layouts only to fit the old 32 KB budget. The
# extended WIN0..WIN2 layout removes that pressure, so all three ship in
# one binary and the former chkdsk12 split is gone.
#
# ----- Memory layout (code-biased extended WIN0..WIN2) -----
#
# The program runs from its own pages in WIN0+WIN1+WIN2. WIN0 is swapped to
# the DSS core page only for the duration of each firmware call, via RST
# trampolines that live in WIN2 (the "never-repaged" window). Because WIN2
# is never repaged, code executes there safely too -- so code+rodata span
# all three windows and only the WIN2 *top* is reserved for the trampolines,
# data and stack:
#
#   0x0180  _CODE   code + rodata, grows up through WIN0 -> WIN1 -> WIN2
#     ...           (~44 KB budget up to _WINRT)
#   0xB000  _WINRT  RST trampolines (win0 runtime), high in WIN2
#   0xB100  _DATA   data / bss / heap, up toward the stack
#   0xBF00  stack   set by crt0_win0, grows down (~2.4 KB)
#
# This is the opposite bias to the stock examples/win0.mk (which reserves
# all of WIN2 for data); checkdsk is code-bound, so code gets WIN2 and the
# small data/stack are lifted to the top. No SDK sources are modified --
# only the link origins differ.
#
# Build is two-pass, packed by tools/win0_exe.py into one DSS .EXE:
#   1. payload  -- app code + win0 runtime, linked at the origins above.
#   2. loader   -- a stage-1 PRELOAD stub at 0x8100 that GETMEMs P0/P1/P2
#                  and streams the payload blobs into them, then jumps in.
#
# Common knobs (override on the command line):
#   SDK_DIR    SDCC SDK root (../sdcc45-sprinter-sdk/ by default).
#   VERSION    X.Y.Z version string baked into the banner. Each part
#              is an 8-bit field (0-255) per the project's versioning
#              scheme -- do not put a date in here (that broke Z, which
#              used to hold YYYYMMDD). Defaults to 0.1.$(BUILD_NUM).
#   BUILD_NUM  The Z part of VERSION: a build counter, 0-255. Defaults
#              to the number of commits since the most recent v<X.Y> git
#              tag (so it resets at each major/minor bump instead of
#              growing across the whole project history -- see the
#              comment above its definition below); override for a
#              manual build number.
#   BUILD_DATE Human-readable build date shown in the banner next to
#              VERSION, e.g. "checkdsk 0.1.3 (19072026)". Defaults to
#              today (DDMMYYYY). Informational only, not part of VERSION.
#   LOG        1 = enable internal logging (default 0).
#   WIN0_WINRT / WIN0_DATA  raise/lower the WIN2 reserve to trade code
#                           headroom against stack size.
#

APP        = chkdsk
SRCS       = main.c cmdline.c diskio_dss.c diskio_batch.c sectbuf.c prt.c volume.c mount.c summary.c bpb.c fat.c chain.c bitmap.c dirwalk.c dirent.c scan.c fix.c

SDK_DIR    ?= ../sdcc45-sprinter-sdk/

# Build number (VERSION's Z part): commits since the most recent v<X.Y>
# git tag, so it resets at each major/minor bump instead of growing
# without bound across the whole project history -- still wrapped into
# the 8-bit field the versioning scheme requires, as a defensive
# fallback. To activate the reset, tag the commit where VERSION's X.Y
# changes, e.g.:
#   git tag v0.2
# Until such a tag exists, this falls back to the full commit count
# (today's behavior). Falls back to 1 outside a git checkout.
LAST_TAG    := $(shell git describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*' 2>/dev/null)
GIT_COMMITS := $(shell if [ -n "$(LAST_TAG)" ]; then git rev-list --count $(LAST_TAG)..HEAD 2>/dev/null; else git rev-list --count HEAD 2>/dev/null; fi)
BUILD_NUM   ?= $(shell if [ -n "$(GIT_COMMITS)" ]; then echo $$(( $(GIT_COMMITS) % 256 )); else echo 1; fi)
BUILD_DATE  ?= $(shell date +%d%m%Y)
VERSION     ?= 0.1.$(BUILD_NUM)
LOG         ?= 0

SDCC       ?= sdcc
SDASZ80    ?= sdasz80
SDLDZ80    ?= sdldz80
PYTHON     ?= python3

BUILD      = _build
WIN0DIR    = $(SDK_DIR)lib/win0
INC        = -I$(SDK_DIR)include -I$(WIN0DIR) -Isrc
SPRLIB     = $(SDK_DIR)build/sprinter.lib
CRT0       = $(SDK_DIR)build/crt0.rel        # standard crt0 for the stage-1 loader

SDCC_TARGET = -mz80
SDCC_FLAGS  = $(SDCC_TARGET) --max-allocs-per-node 5000 --opt-code-size

# Payload link origins (see the layout block above).
WIN0_CODE   ?= 0x0180
WIN0_WINRT  ?= 0xB000
WIN0_DATA   ?= 0xB100

CPPFLAGS   = -DCHKDISK_VERSION='"$(VERSION)"' \
             -DCHKDISK_BUILD_DATE='"$(BUILD_DATE)"' \
             -DCHKDISK_LOG_ENABLE=$(LOG) \
             -DCHKDSK_FAT12=1 -DCHKDSK_FAT16=1 -DCHKDSK_FAT32=1

APP_RELS     = $(patsubst %.c,$(BUILD)/%.rel,$(SRCS))
PAYLOAD_RELS = $(BUILD)/crt0_win0.rel $(BUILD)/win0_rt.rel \
               $(BUILD)/win0_dss.rel $(BUILD)/dss_raw.rel $(APP_RELS)

# SDCC's own z80 runtime library, linked last so it only supplies helpers
# (e.g. __divuint / __moduint pulled in by printf) that sprinter.lib lacks.
SDCC_Z80LIB := $(shell for d in $$($(SDCC) -mz80 --print-search-dirs 2>/dev/null | sed -n '/libdir:/,/libpath:/p' | grep '^/'); do if [ -f "$$d/z80.lib" ]; then echo "$$d/z80.lib"; break; fi; done)

.PHONY: all clean
all: $(APP).exe

$(BUILD):
	mkdir -p $(BUILD)

# ----- win0 runtime objects (from the SDK, unmodified) -----
$(BUILD)/crt0_win0.rel: $(WIN0DIR)/crt0_win0.s | $(BUILD)
	$(SDASZ80) -plosgff $@ $<

$(BUILD)/win0_rt.rel: $(WIN0DIR)/win0_rt.s | $(BUILD)
	$(SDASZ80) -plosgff $@ $<

$(BUILD)/win0_dss.rel: $(WIN0DIR)/win0_dss.c | $(BUILD)
	$(SDCC) $(SDCC_FLAGS) $(INC) -c -o $@ $<

$(BUILD)/dss_raw.rel: $(WIN0DIR)/dss_raw.c | $(BUILD)
	$(SDCC) $(SDCC_FLAGS) $(INC) -c -o $@ $<

$(BUILD)/loader.rel: $(WIN0DIR)/loader.c | $(BUILD)
	$(SDCC) $(SDCC_FLAGS) $(INC) -c -o $@ $<

# ----- checkdsk application objects -----
$(BUILD)/%.rel: src/%.c | $(BUILD)
	$(SDCC) $(SDCC_FLAGS) $(INC) $(CPPFLAGS) -c -o $@ $<

# ----- payload: app code + win0 runtime, code-biased origins -----
# crt0_win0.rel must be first so _win0_entry lands at $(WIN0_CODE).
$(BUILD)/payload.ihx: $(PAYLOAD_RELS) $(SPRLIB)
	printf '%s\n' '-mjx' > $(BUILD)/payload.lk
	printf '%s\n' '-i $(BUILD)/payload.ihx' >> $(BUILD)/payload.lk
	printf '%s\n' '-b _CODE = $(WIN0_CODE)' >> $(BUILD)/payload.lk
	printf '%s\n' '-b _WINRT = $(WIN0_WINRT)' >> $(BUILD)/payload.lk
	printf '%s\n' '-b _DATA = $(WIN0_DATA)' >> $(BUILD)/payload.lk
	printf '%s\n' '-l $(abspath $(SPRLIB))' >> $(BUILD)/payload.lk
	printf '%s\n' '-l $(SDCC_Z80LIB)' >> $(BUILD)/payload.lk
	printf '%s\n' '$(abspath $(BUILD)/crt0_win0.rel)' >> $(BUILD)/payload.lk
	printf '%s\n' '$(abspath $(BUILD)/win0_rt.rel)' >> $(BUILD)/payload.lk
	printf '%s\n' '$(abspath $(BUILD)/win0_dss.rel)' >> $(BUILD)/payload.lk
	printf '%s\n' '$(abspath $(BUILD)/dss_raw.rel)' >> $(BUILD)/payload.lk
	for rel in $(APP_RELS); do printf '%s\n' "$(abspath $(BUILD))/$$(basename $$rel)" >> $(BUILD)/payload.lk; done
	printf '%s\n' '-e' >> $(BUILD)/payload.lk
	$(SDLDZ80) -n -f $(BUILD)/payload.lk

# ----- stage-1 PRELOAD loader (@0x8100), independent of the payload -----
$(BUILD)/loader.ihx: $(CRT0) $(SPRLIB) $(BUILD)/loader.rel
	printf '%s\n' '-mjx' > $(BUILD)/loader.lk
	printf '%s\n' '-i $(BUILD)/loader.ihx' >> $(BUILD)/loader.lk
	printf '%s\n' '-b _CODE = 0x8100' >> $(BUILD)/loader.lk
	printf '%s\n' '-l $(abspath $(SPRLIB))' >> $(BUILD)/loader.lk
	printf '%s\n' '-l $(SDCC_Z80LIB)' >> $(BUILD)/loader.lk
	printf '%s\n' '$(abspath $(CRT0))' >> $(BUILD)/loader.lk
	printf '%s\n' '$(abspath $(BUILD)/loader.rel)' >> $(BUILD)/loader.lk
	printf '%s\n' '-e' >> $(BUILD)/loader.lk
	$(SDLDZ80) -n -f $(BUILD)/loader.lk

$(APP).exe: $(BUILD)/loader.ihx $(BUILD)/payload.ihx $(SDK_DIR)tools/win0_exe.py
	$(PYTHON) $(SDK_DIR)tools/win0_exe.py $(BUILD)/loader.ihx $(BUILD)/payload.ihx $@
	@echo "Built (code-biased win0): $@"

clean:
	rm -rf $(BUILD) $(APP).exe *.ihx *.lk *.map *.mem *.noi *.sym
