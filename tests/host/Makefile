# Host-side test build: compiles the chkdsk core (src/) against the
# host environment (host_env.c) into a native binary that operates on
# disk-image files. Used by ../run_tests.sh; not part of the Sprinter
# build.

CC      ?= cc
# NOTE: -fsanitize=address hangs at process start on macOS 26 (ASan
# malloc-zone init spin); UBSan alone works and still catches the
# classes we care about (shifts, overflow, misaligned access).
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=undefined

SRC_DIR  = ../../src
CORE     = mount.c bpb.c fat.c chain.c dirwalk.c dirent.c scan.c fix.c summary.c prt.c
CORE_SRC = $(addprefix $(SRC_DIR)/,$(CORE))

DEFS = -DCHKDSK_FAT12=1 -DCHKDSK_FAT16=1 -DCHKDSK_FAT32=1 \
       -DCHKDISK_VERSION='"host"' -DCHKDISK_LOG_ENABLE=0
INC  = -Ishim -I$(SRC_DIR)

chkdsk_host: chkdsk_host.c host_env.c $(CORE_SRC)
	$(CC) $(CFLAGS) $(DEFS) $(INC) -o $@ chkdsk_host.c host_env.c $(CORE_SRC)

clean:
	rm -f chkdsk_host
	rm -rf chkdsk_host.dSYM

.PHONY: clean
