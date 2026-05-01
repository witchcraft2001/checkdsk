/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * vol.h -- mounted-volume state + BPB-derived layout.
 *
 * Replaces FatFs's FATFS struct + f_mount. We use FatFs only to
 * populate a small set of BPB-derived fields and never call any of
 * its file/dir APIs, so reimplementing mount in ~200 lines saves
 * ~10 KB of code (most of ff.c) and ~500 bytes of stack (FatFs's
 * win[512] sector buffer).
 *
 * Volume-relative LBAs:
 *   The diskio layer (diskio_dss.c) adds the partition offset set by
 *   volume_apply(), so this module passes LBAs as if the volume began
 *   at sector 0. Every field in vol_t is also volume-relative.
 *
 * BYTE/WORD/DWORD/LBA_t/UINT typedefs live here because diskio.h
 * (vendored from FatFs) expects them at file scope. Anything that
 * includes diskio.h must include vol.h first.
 */

#ifndef CHKDSK_VOL_H
#define CHKDSK_VOL_H

#include <sprinter/types.h>

typedef u8           BYTE;
typedef u16          WORD;
typedef u32          DWORD;
typedef DWORD        LBA_t;
typedef DWORD        FSIZE_t;
typedef unsigned int UINT;

#define FS_FAT12 1
#define FS_FAT16 2
#define FS_FAT32 3

typedef struct {
    BYTE  fs_type;       /* FS_FAT12 / 16 / 32, 0 if not mounted */
    BYTE  n_fats;        /* 1 or 2 */
    WORD  csize;         /* sectors per cluster (power of two) */
    WORD  n_rootdir;     /* root dir entry count (FAT12/16); 0 for FAT32 */
    DWORD n_fatent;      /* FAT entries = max_clust + 2 */
    DWORD fsize;         /* sectors per FAT */
    LBA_t fatbase;       /* first FAT sector, volume-relative */
    LBA_t dirbase;       /* root sector (FAT12/16) or root cluster (FAT32) */
    LBA_t database;      /* first data sector */
    DWORD volid;         /* volume serial number (BS_VolID) */
    DWORD n_total_sec;   /* BPB total sectors */
    LBA_t fsi_sector;    /* FAT32 FSInfo sector LBA, 0 on FAT12/16 */
} vol_t;

/* Backwards-compatible alias for code paths still using FATFS naming.
 * New code should prefer vol_t directly. */
typedef vol_t FATFS;

/* vol_mount return codes. */
#define VOL_OK              0
#define VOL_E_DISK_READ     1
#define VOL_E_BAD_VBR       2  /* missing 0xAA55 signature */
#define VOL_E_BAD_BPB       3  /* BytsPerSec / SecPerClus / fat-sz / etc. */
#define VOL_E_BAD_FAT_TYPE  4  /* unsupported cluster count */

/* Read sector 0 of the active volume via disk_read(pdrv, ..., 0, ...),
 * parse the BPB, populate `v`. The diskio partition offset must have
 * already been set via volume_apply(). Returns VOL_OK or VOL_E_*. */
int  vol_mount(vol_t *v, BYTE pdrv);

/* Zero the struct. No I/O. */
void vol_unmount(vol_t *v);

#endif /* CHKDSK_VOL_H */
