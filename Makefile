APP        = chkdsk
VPATH      = src
SRCS       = main.c cmdline.c diskio_dss.c diskio_batch.c sectbuf.c prt.c volume.c mount.c summary.c bpb.c fat.c chain.c bitmap.c dirwalk.c dirent.c scan.c
SDK_DIR   ?= ../sdcc-sprinter-sdk/
CRT0_PAGE2 ?= 0
# Code+data occupy WIN1+WIN2 (~32 KB), stack at 0xBFFF. Bitmap pages
# map into WIN3 (#C000) on demand.
VERSION   ?= 0.1.$(shell date +%Y%m%d)
LOG       ?= 0

# FAT type selection (compile-time). Default: chkdsk.exe handles
# FAT16+FAT32. Build chkdsk12.exe for floppies as a separate target
# with CHKDSK_FAT12=1 / FAT16=0 / FAT32=0. At least one must be 1.
CHKDSK_FAT12 ?= 0
CHKDSK_FAT16 ?= 1
CHKDSK_FAT32 ?= 1

include $(SDK_DIR)examples/common.mk

APP_CPPFLAGS += -DCHKDISK_VERSION='"$(VERSION)"' \
                -DCHKDISK_LOG_ENABLE=$(LOG) \
                -DCHKDSK_FAT12=$(CHKDSK_FAT12) \
                -DCHKDSK_FAT16=$(CHKDSK_FAT16) \
                -DCHKDSK_FAT32=$(CHKDSK_FAT32)

# Pull z80.lib for SDCC runtime helpers (32-bit divide/mod, etc.) that
# sprinter.lib does not ship. Set SDCC290_BIN_DIR to the SDCC-2.9.0
# install root, or override EXTRA_LIBS at make-time.
ifneq ($(strip $(SDCC290_BIN_DIR)),)
ifeq ($(strip $(EXTRA_LIBS)),)
EXTRA_LIBS = $(abspath $(SDCC290_BIN_DIR))/opt/sdcc-2.9.0/share/sdcc/lib/z80/z80.lib
endif
endif
