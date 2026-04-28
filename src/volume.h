/*
 * volume.h -- drive-letter to physical-device + partition resolver.
 *
 * Stage 0 mapping (mirrors the default DSS scheme; see specs.md):
 *   A:  -> FDD0           (disk=0x00, desc=0xC1E0, part=0/whole)
 *   B:  -> FDD1           (disk=0x01, desc=0xC1E8, part=0/whole)
 *   C:  -> IDE0 master    (disk=0x80, desc=0xC1C0, part=1)
 *   D:  -> IDE0 master    part=2
 *   E:  -> IDE0 master    part=3
 *   F:  -> IDE0 master    part=4
 *   G:  -> IDE0 slave     (disk=0x81, desc=0xC1C8, part=1)
 *   H:  -> IDE0 slave     part=2
 *   I:  -> IDE0 slave     part=3
 *   J:  -> IDE0 slave     part=4
 *
 * Letters K..Z and CMOS #1E TR-DOS remapping are not supported; the
 * resolver returns VOL_ERR_UNSUPPORTED. See specs.md "Drive letter
 * resolution" for the architect feature-request behind this gap.
 */

#ifndef CHKDSK_VOLUME_H
#define CHKDSK_VOLUME_H

#include <sprinter/types.h>

#define VOL_OK              0
#define VOL_ERR_BAD_LETTER  1   /* not in A..Z */
#define VOL_ERR_UNSUPPORTED 2   /* K..Z or any out-of-scope mapping */

typedef struct {
    u8  disk;       /* BIOS drive byte: 0x80/0x81/0x00/0x01/... */
    u16 desc;       /* Address of 8-byte descriptor in system page */
    u8  part;       /* 0 = whole disk (no MBR), 1..4 = MBR partition */
} volume_t;

/* Resolve a single ASCII letter (case-insensitive) into a volume_t.
 * Letter must be one of A..J on stage 0; anything else returns
 * VOL_ERR_BAD_LETTER (non-letter) or VOL_ERR_UNSUPPORTED (K..Z). */
int volume_resolve(char letter, volume_t *out);

/* Configure the FatFs glue layer and VolToPart[0] so a subsequent
 * f_mount(fs, "0:", 1) targets the resolved volume. */
void volume_apply(const volume_t *vol);

#endif /* CHKDSK_VOLUME_H */
