/*
 * summary.c -- volume summary printer. See summary.h.
 *
 * Note on 32-bit values: the SDK printf is 16-bit only -- %ld silently
 * prints the lower 16 bits (specs.md / printf rule). For totals and
 * byte counts we format manually with _utoa32 from printf_long.c.
 */

#include <stdio.h>
#include <string.h>
#include <sprinter.h>
#include "ff.h"
#include "diskio.h"
#include "summary.h"

/* Provided by sdcc-sprinter-sdk/lib/src/stdio/printf_long.c. */
extern char *_utoa32(unsigned long val, char *end, int base, int upper);

/* Format a u32 as decimal into a stack buffer and return a pointer to
 * the start of the string (within the buffer). Buffer must be >= 11 chars. */
static char *fmt_u32(unsigned long v, char *buf12)
{
    return _utoa32(v, buf12 + 11, 10, 0);
}

static const char *fat_type_name(BYTE fs_type)
{
    switch (fs_type) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    case FS_EXFAT: return "exFAT";
    default:       return "?";
    }
}

/* Read 4 bytes little-endian from a buffer at the given offset. */
static unsigned long ld_dword(const BYTE *p, UINT off)
{
    return ((unsigned long)p[off])
         | ((unsigned long)p[off + 1] << 8)
         | ((unsigned long)p[off + 2] << 16)
         | ((unsigned long)p[off + 3] << 24);
}

int summary_print(FATFS *fs, char drive_letter)
{
    DRESULT       drc;
    unsigned long serial;
    unsigned long cluster_bytes;
    unsigned long total_clusters;
    unsigned long total_bytes_lo;
    char          buf[12];

    /* Read VBR (Volume Boot Record) at fs->volbase to extract the
     * volume serial number directly. We avoid f_getlabel for stage 0:
     * its dir_read -> move_window path triggers an SDCC ABI/codegen
     * issue (32-bit struct field arg corrupted to 0x???? duplicated
     * across both halves) that needs deeper investigation in stage 1.
     * Volume label scan is deferred to that work too. */
    serial = 0ul;
    drc = disk_read(0, fs->win, fs->volbase, 1);
    if (drc == RES_OK) {
        if (fs->fs_type == FS_FAT32) {
            serial = ld_dword(fs->win, 0x43);   /* BS_VolID32 */
        } else {
            /* FAT12/16: BS_VolID at offset 0x27 if BS_BootSig (0x26) == 0x29 */
            if (fs->win[0x26] == 0x29u) {
                serial = ld_dword(fs->win, 0x27);
            }
        }
    }

    cluster_bytes  = (unsigned long)fs->csize * 512ul;
    total_clusters = (unsigned long)(fs->n_fatent - 2u);
    total_bytes_lo = total_clusters * cluster_bytes;

    printf("Volume %c: (%s)\r\n", drive_letter, fat_type_name(fs->fs_type));
    printf("Serial: %04X-%04X\r\n",
           (unsigned int)((serial >> 16) & 0xFFFFul),
           (unsigned int)(serial & 0xFFFFul));
    printf("Cluster size:   %s bytes\r\n", fmt_u32(cluster_bytes,  buf));
    printf("Total clusters: %s\r\n",        fmt_u32(total_clusters, buf));
    printf("Total bytes:    %s\r\n",        fmt_u32(total_bytes_lo, buf));
    /* Volume label and free-cluster count: see stage 1 follow-up. */
    return 0;
}
