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

int summary_print(FATFS *fs, char drive_letter)
{
    FRESULT  rc;
    char     label[12];
    DWORD    serial;
    unsigned long cluster_bytes;
    unsigned long total_clusters;
    unsigned long total_bytes_lo;
    char     buf[12];
    char     path[3];

    path[0] = drive_letter;
    path[1] = ':';
    path[2] = '\0';

    label[0] = '\0';
    serial = 0ul;
    rc = f_getlabel(path, label, &serial);
    if (rc != FR_OK) {
        printf("checkdsk: f_getlabel failed (rc=%u)\r\n", (unsigned int)rc);
        return (int)rc;
    }

    /* csize is cluster size in sectors; sector size is fixed at 512
     * in our build (FF_MIN_SS == FF_MAX_SS). */
    cluster_bytes  = (unsigned long)fs->csize * 512ul;
    total_clusters = (unsigned long)(fs->n_fatent - 2u);
    total_bytes_lo = total_clusters * cluster_bytes;

    printf("Volume %c: ", drive_letter);
    if (label[0] != '\0') printf("%s ", label);
    printf("(%s)\r\n", fat_type_name(fs->fs_type));
    printf("Serial: %04X-%04X\r\n",
           (unsigned int)((serial >> 16) & 0xFFFFu),
           (unsigned int)(serial & 0xFFFFu));

    printf("Cluster size:   %s bytes\r\n", fmt_u32(cluster_bytes,  buf));
    printf("Total clusters: %s\r\n",        fmt_u32(total_clusters, buf));
    printf("Total bytes:    %s\r\n",        fmt_u32(total_bytes_lo, buf));
    /* Free-cluster count requires f_getfree, which is removed by
     * FF_FS_READONLY = 1. Restored at stage 5 when fix mode lands. */
    return 0;
}
