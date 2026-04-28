/*
 * volume.h -- drive-letter to physical-device + partition resolver.
 *
 * Mapping algorithm (mirrors the actual DSS scheme observed on real
 * hardware; the manual's "C: master part 1, D: master part 2..."
 * example only applies when a single device has multiple partitions):
 *
 *   A: / B: -> FDD0 / FDD1 (descriptors #C1E0 / #C1E8, part = 0)
 *   C:..    -> scan IDE slots in order
 *                 #80 primary master   (#C1C0)
 *                 #81 primary slave    (#C1C8)
 *                 #82 secondary master (#C1D0)
 *                 #83 secondary slave  (#C1D8)
 *              for each device that is present and has a valid MBR,
 *              walk partition entries 1..4 in order and count only
 *              the ones with a non-zero starting LBA. The N-th such
 *              partition (across all devices) gets the letter C+N.
 *
 * partition_lba is filled in at resolve time so callers can run
 * Phase 1 (and beyond) without re-parsing the MBR.
 */

#ifndef CHKDSK_VOLUME_H
#define CHKDSK_VOLUME_H

#include <sprinter/types.h>

#define VOL_OK              0
#define VOL_ERR_BAD_LETTER  1   /* not in A..Z */
#define VOL_ERR_UNSUPPORTED 2   /* K..Z or any out-of-scope mapping */

typedef struct {
    u8  disk;            /* BIOS drive byte: 0x80/0x81/0x00/0x01/... */
    u16 desc;            /* Address of 8-byte descriptor in system page */
    u8  part;            /* 0 = whole disk (no MBR), 1..4 = MBR partition */
    u32 partition_lba;   /* LBA of the VBR (0 for whole-disk FDD) */
} volume_t;

/* Resolve a single ASCII letter (case-insensitive) into a volume_t,
 * including the partition's starting LBA. As a side effect this may
 * call diskio_dss_set_device while scanning IDE devices; callers must
 * still call volume_apply() afterwards to lock in the final selection. */
int volume_resolve(char letter, volume_t *out);

/* Configure the FatFs glue layer and VolToPart[0] so a subsequent
 * f_mount(fs, "0:", 1) targets the resolved volume. */
void volume_apply(const volume_t *vol);

#endif /* CHKDSK_VOLUME_H */
