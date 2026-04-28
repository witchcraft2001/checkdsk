APP        = chkdsk
VPATH      = src
SRCS       = main.c cmdline.c diskio_dss.c bitmap.c volume.c summary.c bpb.c ff.c ffsystem.c
SDK_DIR   ?= ../sdcc-sprinter-sdk/
CRT0_PAGE2 ?= 0
# Stage 0 layout: code occupies WIN1+WIN2 (~32 KB), stack at 0xBFFF.
# Bitmap pages map into WIN3 (#C000) on demand. Switch to CRT0_PAGE2=1
# only after ff.c code shrinks to fit in WIN1 alone (~15.5 KB).
VERSION   ?= 0.1.$(shell date +%Y%m%d)
LOG       ?= 0
CHKDISK_USE_LFN ?= 0

include $(SDK_DIR)examples/common.mk

APP_CPPFLAGS += -DCHKDISK_VERSION='"$(VERSION)"' \
                -DCHKDISK_LOG_ENABLE=$(LOG) \
                -DCHKDISK_USE_LFN=$(CHKDISK_USE_LFN) \
                -DFF_USE_SDCC_TYPES

# FatFs needs SDCC's 32-bit multiply helper (__mullong_rrx_s); sprinter.lib
# only ships the divide/mod variants. Pull z80.lib from the SDCC stdlib
# alongside the toolchain selected via SDCC290_BIN_DIR. Override EXTRA_LIBS
# at make-time if your install layout differs.
ifneq ($(strip $(SDCC290_BIN_DIR)),)
ifeq ($(strip $(EXTRA_LIBS)),)
EXTRA_LIBS = $(abspath $(SDCC290_BIN_DIR))/opt/sdcc-2.9.0/share/sdcc/lib/z80/z80.lib
endif
endif
