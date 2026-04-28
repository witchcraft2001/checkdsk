/*
 * volume.c -- drive-letter resolver. See volume.h for the mapping
 * algorithm and side-effect contract.
 */

#include "ff.h"
#include "diskio.h"
#include "volume.h"
#include "diskio_dss.h"
#include "sectbuf.h"

#define g_mbr_sec g_sect_a


/* IDE slot table. Order matches the BIOS device numbers (#80..#83) and
 * the physical descriptor table at #C1C0/#C1C8/#C1D0/#C1D8. */
static const u8  IDE_DISK[4] = { 0x80u, 0x81u, 0x82u, 0x83u };
static const u16 IDE_DESC[4] = { 0xC1C0u, 0xC1C8u, 0xC1D0u, 0xC1D8u };

static u32 read_le32(const BYTE *p)
{
    return  (u32)p[0]
         | ((u32)p[1] << 8)
         | ((u32)p[2] << 16)
         | ((u32)p[3] << 24);
}

/* Try reading the MBR of the device currently configured in diskio. Returns
 * 1 if MBR is valid (signature OK), 0 otherwise (read failure or no MBR). */
static int read_mbr_into(BYTE *buf)
{
    if (disk_read(0, buf, (LBA_t)0u, 1) != RES_OK) return 0;
    if (buf[510] != 0x55u || buf[511] != 0xAAu)    return 0;
    return 1;
}

int volume_resolve(char letter, volume_t *out)
{
    int target;
    int idx;
    u8  i;
    u8  p;

    if (letter >= 'a' && letter <= 'z') {
        letter = (char)(letter - ('a' - 'A'));
    }
    if (letter < 'A' || letter > 'Z') {
        return VOL_ERR_BAD_LETTER;
    }

    /* A: / B: -> floppy. Whole-disk volume, no MBR. */
    if (letter == 'A') {
        out->disk          = 0x00u;
        out->desc          = 0xC1E0u;
        out->part          = 0u;
        out->partition_lba = 0ul;
        return VOL_OK;
    }
    if (letter == 'B') {
        out->disk          = 0x01u;
        out->desc          = 0xC1E8u;
        out->part          = 0u;
        out->partition_lba = 0ul;
        return VOL_OK;
    }

    /* C..Z: scan IDE slots, count non-empty partitions in order. */
    target = (int)(letter - 'C');
    idx    = 0;

    for (i = 0u; i < 4u; i++) {
        diskio_dss_set_device(IDE_DISK[i], IDE_DESC[i]);
        if (!read_mbr_into(g_mbr_sec)) {
            continue;   /* device absent or no MBR */
        }
        for (p = 1u; p <= 4u; p++) {
            UINT off  = 0x1BEu + ((UINT)(p - 1u) * 16u);
            u32  plba = read_le32(&g_mbr_sec[off + 8]);
            if (plba == 0ul) continue;
            if (idx == target) {
                out->disk          = IDE_DISK[i];
                out->desc          = IDE_DESC[i];
                out->part          = p;
                out->partition_lba = plba;
                return VOL_OK;
            }
            idx++;
        }
    }

    /* Letter not found. Leave diskio device in an unselected state so
     * later operations cannot accidentally hit whichever IDE slot the
     * scan happened to leave configured. */
    diskio_dss_set_device(0xFFu, 0u);
    return VOL_ERR_UNSUPPORTED;
}

void volume_apply(const volume_t *vol)
{
    diskio_dss_set_device(vol->disk, vol->desc);
    diskio_dss_set_partition_offset((unsigned long)vol->partition_lba);
}
