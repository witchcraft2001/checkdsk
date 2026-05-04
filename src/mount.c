/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * mount.c -- vol_mount: parse the BPB and populate vol_t.
 *
 * Only does what we use: read sector 0 of the active volume, validate
 * the boot signature + BPB sanity, classify FAT12/16/32 by cluster
 * count using the canonical 4085 / 65525 thresholds, and compute the
 * per-volume LBA layout. No FSInfo cross-checks or FAT-content
 * validation -- those belong to bpb_check / fat_check, which run
 * separately around mount.
 *
 * Multiplications by n_fats (always 1 or 2) are written as a branch
 * to avoid pulling __mullong from z80.lib.
 */

#include <string.h>
#include <sprinter.h>
#include "vol.h"
#include "diskio.h"
#include "sectbuf.h"

/* Shared LE byte loaders -- declared in vol.h, used by mount/bpb/fat/
 * dirent. One global copy instead of four static copies. */
WORD vol_ld_w(const BYTE *p)
{
    WORD x;
    BYTE *xb = (BYTE *)&x;
    xb[0] = p[0]; xb[1] = p[1];
    return x;
}

DWORD vol_ld_d(const BYTE *p)
{
    DWORD x;
    BYTE *xb = (BYTE *)&x;
    xb[0] = p[0]; xb[1] = p[1]; xb[2] = p[2]; xb[3] = p[3];
    return x;
}

#define LD_W(p, o)  vol_ld_w((p) + (o))
#define LD_D(p, o)  vol_ld_d((p) + (o))

static int is_pow2(WORD v)
{
    if (v == 0u) return 0;
    return ((v & (WORD)(v - 1u)) == 0u) ? 1 : 0;
}

void vol_unmount(vol_t *v)
{
    memset(v, 0, sizeof(*v));
}

int vol_mount(vol_t *v, BYTE pdrv)
{
    BYTE  *vbr;
    WORD   bps, rsvd, n_root, totsec16, fatsz16;
    BYTE   spc, n_fats, spc_shift;
    DWORD  totsec32, fatsz32, totsec, fatsz;
    DWORD  root_sectors, data_sectors, count_clust, fat_total;

    vol_unmount(v);

    vbr = g_sect_a;
    if (disk_read(pdrv, vbr, (LBA_t)0u, 1u) != RES_OK) return VOL_E_DISK_READ;
    if (vbr[510] != 0x55u || vbr[511] != 0xAAu)        return VOL_E_BAD_VBR;

    bps      = LD_W(vbr, 11);
    spc      = vbr[13];
    rsvd     = LD_W(vbr, 14);
    n_fats   = vbr[16];
    n_root   = LD_W(vbr, 17);
    totsec16 = LD_W(vbr, 19);
    fatsz16  = LD_W(vbr, 22);
    totsec32 = LD_D(vbr, 32);
    fatsz32  = LD_D(vbr, 36);

    if (bps != 512u)                     return VOL_E_BAD_BPB;
    if (!is_pow2((WORD)spc) || spc > 128u) return VOL_E_BAD_BPB;
    if (n_fats == 0u || n_fats > 2u)     return VOL_E_BAD_BPB;
    if (rsvd == 0u)                      return VOL_E_BAD_BPB;

    /* spc is power of two in [1..128] -- compute log2 once, then use
     * shifts everywhere instead of 32-bit *spc / /spc. Lets the linker
     * drop _mullong/_divulong/_modulong from _HOME entirely. */
    {
        BYTE s = spc;
        spc_shift = 0u;
        while ((s >>= 1) != 0u) spc_shift++;
    }

    fatsz  = (fatsz16 != 0u)  ? (DWORD)fatsz16  : fatsz32;
    totsec = (totsec16 != 0u) ? (DWORD)totsec16 : totsec32;
    if (fatsz == 0u || totsec == 0u)     return VOL_E_BAD_BPB;

    /* Root dir occupies ceil(n_root * 32 / bps) sectors. bps == 512
     * (validated above), so this reduces to ceil(n_root / 16) =
     * (n_root + 15) >> 4 -- 16-bit math, no math runtime pulled. */
    root_sectors = (DWORD)(WORD)((n_root + 15u) >> 4);

    /* fat_total = n_fats * fatsz, but n_fats is 1 or 2: avoid _mullong. */
    fat_total = (n_fats == 2u) ? (fatsz << 1) : fatsz;

    if (totsec <= ((DWORD)rsvd + fat_total + root_sectors)) return VOL_E_BAD_BPB;
    data_sectors = totsec - ((DWORD)rsvd + fat_total + root_sectors);
    count_clust  = data_sectors >> spc_shift;

    if (count_clust < 4085ul) {
#if CHKDSK_FAT12
        v->fs_type = FS_FAT12;
        if (n_root == 0u) return VOL_E_BAD_BPB;
#else
        return VOL_E_BAD_FAT_TYPE;
#endif
    } else if (count_clust < 65525ul) {
#if CHKDSK_FAT16
        v->fs_type = FS_FAT16;
        if (n_root == 0u) return VOL_E_BAD_BPB;
#else
        return VOL_E_BAD_FAT_TYPE;
#endif
    } else {
#if CHKDSK_FAT32
        v->fs_type = FS_FAT32;
        if (n_root != 0u || fatsz16 != 0u) return VOL_E_BAD_BPB;
#else
        return VOL_E_BAD_FAT_TYPE;
#endif
    }

    v->n_fats      = n_fats;
    v->csize       = (WORD)spc;
    v->csize_shift = spc_shift;
    v->n_rootdir   = n_root;
    v->n_fatent    = count_clust + 2ul;
    v->fsize       = fatsz;
    v->fatbase     = (LBA_t)rsvd;
    v->n_total_sec = totsec;

    if (v->fs_type == FS_FAT32) {
        v->dirbase    = (LBA_t)LD_D(vbr, 44);            /* root cluster */
        v->database   = v->fatbase + (LBA_t)fat_total;
        v->fsi_sector = (LBA_t)LD_W(vbr, 48);
        v->volid      = LD_D(vbr, 0x43);
    } else {
        v->dirbase    = v->fatbase + (LBA_t)fat_total;   /* first root sector */
        v->database   = v->dirbase + (LBA_t)root_sectors;
        v->fsi_sector = 0ul;
        v->volid      = (vbr[0x26] == 0x29u) ? LD_D(vbr, 0x27) : 0ul;
    }

    return VOL_OK;
}
