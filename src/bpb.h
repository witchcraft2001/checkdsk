/*
 * bpb.h -- Phase 1 boot-sector / BPB validator.
 *
 * Stage 1 reads the VBR (volume boot record) via disk_read and runs
 * the integrity checks described in specs.md "Stage 1":
 *   - 0xAA55 signature at offset 510
 *   - BPB field sanity (sector size, cluster size, FAT count, root
 *     entries, total sectors, media type)
 *   - FAT type vs cluster count consistency
 *     (<=4084 FAT12, 4085..65524 FAT16, >=65525 FAT32)
 *   - FAT32: FSInfo signatures (0x41615252 / 0x61417272 / 0xAA55) and
 *     plausibility of free_count / next_free
 *   - FAT32: backup boot sector at volbase + 6 byte-equal to main
 *
 * Stage 1 is strictly read-only. Errors are reported only.
 */

#ifndef CHKDSK_BPB_H
#define CHKDSK_BPB_H

#include "vol.h"    /* for LBA_t */

/* Run the Phase 1 validation against the volume whose VBR sits at
 * `volbase`. Prints one line per finding to stdout. Returns the number
 * of issues found (0 = clean). */
int bpb_check(LBA_t volbase);

#endif /* CHKDSK_BPB_H */
