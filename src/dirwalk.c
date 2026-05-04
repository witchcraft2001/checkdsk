/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * dirwalk.c -- 32-byte directory-entry iterator. See dirwalk.h.
 */

#include <sprinter.h>
#include "vol.h"
#include "diskio.h"
#include "chain.h"
#include "sectbuf.h"
#include "dirwalk.h"

#define DIRENT_SIZE      32u
#define SECTOR_SIZE     512u
#define ENTRIES_PER_SEC  16u   /* 512 / 32 */

int dirwalk_open_root(dirwalk_t *w, vol_t *fs)
{
    w->fs           = fs;
    w->end_seen     = 0u;
    w->buffer_dirty = 0u;
    w->sect_off     = SECTOR_SIZE;     /* trigger first sector load on next() */

    if (fs->fs_type == FS_FAT32) {
        w->cluster            = (DWORD)fs->dirbase;
        w->cur_sect           = chain_cluster_to_lba(fs, w->cluster);
        w->cluster_sects_left = fs->csize;
        w->entries_left       = 0xFFFFFFFFul;
    } else {
        w->cluster            = 0ul;
        w->cur_sect           = (LBA_t)fs->dirbase;
        w->cluster_sects_left = 0u;
        w->entries_left       = (DWORD)fs->n_rootdir;
    }
    return 0;
}

int dirwalk_open_chain(dirwalk_t *w, vol_t *fs, DWORD start_cluster)
{
    w->fs                 = fs;
    w->end_seen           = 0u;
    w->buffer_dirty       = 0u;
    w->sect_off           = SECTOR_SIZE;
    w->cluster            = start_cluster;
    w->cur_sect           = chain_cluster_to_lba(fs, start_cluster);
    w->cluster_sects_left = fs->csize;
    w->entries_left       = 0xFFFFFFFFul;
    return 0;
}

void dirwalk_buffer_dirty(dirwalk_t *w)
{
    w->buffer_dirty = 1u;
}

void dirwalk_last_entry_location(const dirwalk_t *w,
                                 LBA_t *out_sect, WORD *out_off)
{
    /* cur_sect was incremented after load_dir_sector, so the live sector
     * is cur_sect - 1. sect_off was advanced past the just-returned
     * 32-byte entry, so its on-sector offset is sect_off - DIRENT_SIZE. */
    *out_sect = w->cur_sect - 1u;
    *out_off  = (WORD)(w->sect_off - DIRENT_SIZE);
}

/* Load the sector at w->cur_sect into g_sect_a, advance bookkeeping.
 * Returns 1 ok, -1 on I/O error.
 *
 * Chain-based walks (cluster != 0 -- FAT32 root, every subdir on every
 * FAT type) decrement cluster_sects_left so the caller can switch to
 * the next cluster on the FAT chain at the boundary. The fixed-area
 * FAT12/16 root has cluster == 0 and pages by entries_left instead. */
static int load_dir_sector(dirwalk_t *w)
{
    if (disk_read(0u, g_sect_a, w->cur_sect, 1u) != RES_OK) return -1;
    chain_invalidate();   /* g_sect_a no longer holds a FAT sector */
    w->cur_sect++;
    if (w->cluster != 0ul && w->cluster_sects_left > 0u) {
        w->cluster_sects_left--;
    }
    w->sect_off = 0u;
    return 1;
}

/* Cluster boundary -- follow the FAT chain to the next cluster.
 * Used for any chain-based walk (FAT32 root, every subdir on every
 * FAT type). Returns 1 if a fresh cluster is now staged in
 * cur_sect/cluster_sects_left, 0 on EOC (end of directory),
 * -1 on error. */
static int advance_cluster(dirwalk_t *w)
{
    DWORD next;

    next = chain_get_entry(w->fs, w->cluster);
    if (next == CHAIN_READ_ERROR)        return -1;
    if (chain_is_eoc(w->fs, next))       return 0;
    if (next < 2ul || next >= w->fs->n_fatent) return -1;

    w->cluster            = next;
    w->cur_sect           = chain_cluster_to_lba(w->fs, next);
    w->cluster_sects_left = w->fs->csize;
    return 1;
}

int dirwalk_next(dirwalk_t *w, BYTE **out_entry)
{
    int rc;

    if (w->end_seen) return 0;

    /* External code (e.g. a sub-directory walker) clobbered g_sect_a;
     * re-load the sector we were partway through so iteration resumes
     * at the exact byte offset. No-op if we're at a sector boundary. */
    if (w->buffer_dirty && w->sect_off > 0u && w->sect_off < SECTOR_SIZE) {
        if (disk_read(0u, g_sect_a, w->cur_sect - 1u, 1u) != RES_OK) {
            w->end_seen = 1u;
            return -1;
        }
        chain_invalidate();
    }
    w->buffer_dirty = 0u;

    while (w->entries_left > 0ul) {
        if (w->sect_off >= SECTOR_SIZE) {
            /* Cluster boundary on any chain-based walk (FAT32 root or
             * any subdir on any FAT type). Without this, FAT16/12
             * multi-cluster subdirs walked linearly past their first
             * cluster, parsing adjacent on-disk data as 32-byte
             * entries -- producing spurious child-cluster references
             * and inflating phase-3 reach (or, on /F, corrupting
             * unrelated content). */
            if (w->cluster != 0ul && w->cluster_sects_left == 0u) {
                rc = advance_cluster(w);
                if (rc <= 0) { w->end_seen = 1u; return rc; }
            }
            rc = load_dir_sector(w);
            if (rc < 0) { w->end_seen = 1u; return -1; }
        }

        *out_entry = &g_sect_a[w->sect_off];
        w->sect_off += DIRENT_SIZE;

        if (w->cluster == 0ul) {
            /* Fixed-root mode: count entries, not sectors. */
            w->entries_left--;
        }

        if ((*out_entry)[0] == 0x00u) {
            w->end_seen = 1u;
            return 0;
        }
        return 1;
    }
    w->end_seen = 1u;
    return 0;
}
