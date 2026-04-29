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

/* Load the sector at w->cur_sect into g_sect_a, advance bookkeeping.
 * Returns 1 ok, -1 on I/O error. */
static int load_dir_sector(dirwalk_t *w)
{
    if (disk_read(0u, g_sect_a, w->cur_sect, 1u) != RES_OK) return -1;
    chain_invalidate();   /* g_sect_a no longer holds a FAT sector */
    w->cur_sect++;
    if (w->fs->fs_type == FS_FAT32 && w->cluster_sects_left > 0u) {
        w->cluster_sects_left--;
    }
    w->sect_off = 0u;
    return 1;
}

/* Cluster boundary on FAT32 -- follow the chain to the next cluster.
 * Returns 1 if a fresh cluster is now staged in cur_sect/cluster_sects_left,
 * 0 on EOC (end of directory), -1 on error. */
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
            if (w->fs->fs_type == FS_FAT32 && w->cluster_sects_left == 0u) {
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
