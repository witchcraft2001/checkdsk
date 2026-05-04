/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * summary.c -- volume summary printer. See summary.h.
 *
 * vol_mount populates v->volid directly so we no longer need to
 * re-read the VBR here (which used to work around an SDCC ABI bug
 * in FatFs's f_getlabel path).
 */

#include <sprinter.h>
#include "vol.h"
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

int summary_print(vol_t *fs, char drive_letter)
{
    unsigned long cluster_bytes;
    unsigned long total_clusters;
    unsigned long total_bytes_lo;

    /* cluster_bytes = csize * 512 = 1 << (csize_shift + 9); use shift
     * everywhere to keep _mullong out of _HOME. */
    cluster_bytes  = (unsigned long)fs->csize << 9;
    total_clusters = (unsigned long)(fs->n_fatent - 2u);
    total_bytes_lo = total_clusters << (fs->csize_shift + 9u);

    prt_str("Volume ");
    prt_chr(drive_letter);
    prt_str(": (");
    prt_str(fat_type_name(fs->fs_type));
    prt_str(")\r\n");

    prt_str("Serial: ");
    prt_hex((fs->volid >> 16) & 0xFFFFul, 4u);
    prt_chr('-');
    prt_hex(fs->volid & 0xFFFFul, 4u);
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
