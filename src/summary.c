/*
 * summary.c -- volume summary printer. See summary.h.
 */

#include <sprinter.h>
#include "ff.h"
#include "diskio.h"
#include "summary.h"
#include "prt.h"

static const char *fat_type_name(BYTE fs_type)
{
    switch (fs_type) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    default:       return "?";
    }
}

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

    /* Read VBR at fs->volbase to extract the volume serial number.
     * f_getlabel is avoided -- its dir_read -> move_window path hits
     * an SDCC ABI/codegen issue documented in specs.md. */
    serial = 0ul;
    drc = disk_read(0, fs->win, fs->volbase, 1);
    if (drc == RES_OK) {
        if (fs->fs_type == FS_FAT32) {
            serial = ld_dword(fs->win, 0x43);
        } else {
            if (fs->win[0x26] == 0x29u) {
                serial = ld_dword(fs->win, 0x27);
            }
        }
    }

    cluster_bytes  = (unsigned long)fs->csize * 512ul;
    total_clusters = (unsigned long)(fs->n_fatent - 2u);
    total_bytes_lo = total_clusters * cluster_bytes;

    prt_str("Volume ");
    prt_chr(drive_letter);
    prt_str(": (");
    prt_str(fat_type_name(fs->fs_type));
    prt_str(")\r\n");

    prt_str("Serial: ");
    prt_hex((serial >> 16) & 0xFFFFul, 4u);
    prt_chr('-');
    prt_hex(serial & 0xFFFFul, 4u);
    prt_nl();

    prt_str("Cluster size:   ");
    prt_dec(cluster_bytes);
    prt_str(" bytes\r\n");

    prt_str("Total clusters: ");
    prt_dec(total_clusters);
    prt_nl();

    prt_str("Total bytes:    ");
    prt_dec(total_bytes_lo);
    prt_nl();

    return 0;
}
