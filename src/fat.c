/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * fat.c -- Phase 2 FAT-table validator. See fat.h.
 *
 * Memory: two 512-byte static buffers (1 KB BSS). g_fat_a holds the
 * current sector of FAT 1; g_fat_b is used for FAT 2 byte-compare on
 * volumes with n_fats == 2 and as the FAT12 sector lookahead buffer
 * when an entry crosses a 512-byte boundary.
 */

#include <sprinter.h>
#include "vol.h"
#include "diskio.h"
#include "diskio_batch.h"
#include "fat.h"
#include "sectbuf.h"
#include "fix.h"
#include "prt.h"

#define g_fat_a g_sect_a

#define SECTOR_SIZE 512u

/* Cheap sum across a 512-byte sector. Lets us detect that two FAT
 * copies differ without holding both in RAM simultaneously. */
static unsigned long sec_sum(const u8 *p)
{
    unsigned long s = 0ul;
    UINT i;
    for (i = 0u; i < SECTOR_SIZE; i++) s += (unsigned long)p[i];
    return s;
}

#define BAD12       0x0FF7u
#define EOC12_MIN   0x0FF8u
#define BAD16       0xFFF7u
#define EOC16_MIN   0xFFF8u
#define BAD32       0x0FFFFFF7ul
#define EOC32_MIN   0x0FFFFFF8ul

#define MAX_DETAIL 3

static unsigned long ld_dword(const BYTE *p)
{
    unsigned long x;
    BYTE *xb = (BYTE *)&x;
    xb[0] = p[0]; xb[1] = p[1]; xb[2] = p[2]; xb[3] = p[3];
    return x;
}

static void fat_err(const char *msg, int *errs)
{
    fix_verbose_flush();
    prt_str("  ERROR: ");
    prt_str(msg);
    prt_nl();
    (*errs)++;
    fix_count_found();
}

static void fat_warn(const char *msg)
{
    fix_verbose_flush();
    prt_str("  WARN: ");
    prt_str(msg);
    prt_nl();
}

/* Categorisation of an entry value. Returns:
 *   0 = free, 1 = in_use, 2 = bad, 3 = eoc, 4 = invalid. */
static u8 classify(unsigned long v,
                   unsigned long bad,
                   unsigned long eoc_min,
                   unsigned long max_cluster)
{
    if (v == 0ul)                            return 0u;
    if (v == bad)                            return 2u;
    if (v >= eoc_min)                        return 3u;
    if (v >= 2ul && v <= max_cluster)        return 1u;
    return 4u;
}

/* Read media descriptor byte from the BPB (offset 0x15 of VBR).
 * Caller saves the byte; the buffer may be overwritten by subsequent
 * disk reads. */
static u8 read_media_byte(vol_t *fs)
{
    (void)fs;
    if (disk_read(0, g_fat_a, (LBA_t)0u, 1) != RES_OK) return 0u;
    return g_fat_a[0x15];
}


/* Per-entry counters used by both FAT12 and FAT16/32 paths. */
typedef struct {
    unsigned long free_n;
    unsigned long used_n;
    unsigned long bad_n;
    unsigned long invalid_n;
    unsigned long eoc_n;     /* end-of-chain markers in user clusters (rare) */
    int           detail_left;
} cnt_t;

static void cnt_init(cnt_t *c)
{
    c->free_n      = 0ul;
    c->used_n      = 0ul;
    c->bad_n       = 0ul;
    c->invalid_n   = 0ul;
    c->eoc_n       = 0ul;
    c->detail_left = MAX_DETAIL;
}

static void note_invalid(cnt_t *c, unsigned long entry_n, unsigned long val)
{
    c->invalid_n++;
    if (c->detail_left > 0) {
        prt_str("    cluster ");
        prt_dec(entry_n);
        prt_str(" -> 0x");
        prt_hex(val, 8u);
        prt_str(" (invalid)\r\n");
        c->detail_left--;
    }
}

/* Common per-entry validator: updates counters, prints up to MAX_DETAIL
 * invalid-reference lines. entry_n 0 and 1 are skipped (handled by the
 * caller for special-entry checks). */
static void validate_one(cnt_t *c,
                         unsigned long entry_n,
                         unsigned long val,
                         unsigned long bad,
                         unsigned long eoc_min,
                         unsigned long max_cluster)
{
    u8 k;

    if (entry_n < 2ul) return;
    k = classify(val, bad, eoc_min, max_cluster);
    switch (k) {
    case 0u: c->free_n++; break;
    case 1u: c->used_n++; break;
    case 2u: c->bad_n++;  break;
    case 3u: c->eoc_n++;  break;
    default: note_invalid(c, entry_n, val); break;
    }
}

/* Copy FAT 1 over FAT 2 sector by sector. Called only when /F is set
 * and a FAT 1/2 mismatch was detected during the validation pass. Per
 * FAT spec FAT 1 is the primary copy, so blanket-overwriting FAT 2 is
 * the safe default direction; we don't try to choose per-sector
 * winners. Counts as one logical fix regardless of how many sectors
 * were copied. */
static int sync_fats(vol_t *fs, int *errs)
{
    DWORD sec;
    LBA_t fat1 = fs->fatbase;
    LBA_t fat2 = fat1 + (LBA_t)fs->fsize;

    for (sec = 0ul; sec < fs->fsize; sec++) {
        if (disk_read(0, g_fat_a, fat1 + (LBA_t)sec, 1) != RES_OK) {
            fat_err("FAT 1 read failed during sync", errs);
            return -1;
        }
        if (!fix_write(fat2 + (LBA_t)sec, g_fat_a, 1u)) {
            fat_err("FAT 2 write failed during sync", errs);
            return -1;
        }
    }
    fix_count_applied();
    prt_str("  Synced FAT 2 from FAT 1 (");
    prt_dec((unsigned long)fs->fsize);
    prt_str(" sectors)\r\n");
    return 0;
}

#if CHKDSK_FAT16 || CHKDSK_FAT32
/* FAT16/FAT32 unified path. Reads in 32-sector batches via
 * diskio_batch (one BIOS DRV_READ call per batch). Walks all entries
 * within the batch from the mapped DSS page (WIN3); for n_fats == 2,
 * snapshots a per-sector sum, then re-reads FAT 2 over the same page
 * and compares sums to detect mismatching sectors. On a 1 GB FAT16
 * volume this drops phase 2 from ~25 s to a few seconds. */
static int check_fat_wide(vol_t *fs, int *errs, cnt_t *c, int is_fat32)
{
    LBA_t         fat1 = fs->fatbase;
    LBA_t         fat2 = fat1 + (LBA_t)fs->fsize;
    int           cmp_two = (fs->n_fats == 2u);
    unsigned long max_cluster = (unsigned long)fs->n_fatent - 1ul;
    unsigned long entry_n = 0ul;
    unsigned long entries_total = (unsigned long)fs->n_fatent;
    unsigned long bad     = is_fat32 ? BAD32     : (unsigned long)BAD16;
    unsigned long eoc_min = is_fat32 ? EOC32_MIN : (unsigned long)EOC16_MIN;
    UINT          step    = is_fat32 ? 4u : 2u;
    unsigned long diff_sectors = 0ul;
    unsigned long sums[BATCH_SECTORS_PER_PAGE];
    DWORD         sec_off;
    u8           *page;

    if (!diskio_batch_open(1u)) {
        fat_err("page memory unavailable for batch FAT read", errs);
        return -1;
    }

    sec_off = 0;
    while (sec_off < fs->fsize && entry_n < entries_total) {
        fix_verbose_tick();
        DWORD remaining = fs->fsize - sec_off;
        u8    batch     = (remaining > BATCH_SECTORS_PER_PAGE)
                          ? (u8)BATCH_SECTORS_PER_PAGE
                          : (u8)remaining;
        UINT  batch_bytes = (UINT)batch * SECTOR_SIZE;
        UINT  i;
        u8    si;

        if (!diskio_batch_read((unsigned long)(fat1 + (LBA_t)sec_off),
                               batch, 0u)) {
            fat_err("FAT 1 batch read failed", errs);
            diskio_batch_close();
            return -1;
        }
        page = diskio_batch_map(0u);

        if (cmp_two) {
            for (si = 0u; si < batch; si++) {
                sums[si] = sec_sum(page + (UINT)si * SECTOR_SIZE);
            }
        }

        for (i = 0u; i + step - 1u < batch_bytes && entry_n < entries_total;
             i += step) {
            unsigned long v;
            if (is_fat32) {
                v = ld_dword(&page[i]) & 0x0FFFFFFFul;
            } else {
                v = (unsigned long)page[i]
                  | ((unsigned long)page[i + 1u] << 8);
            }
            validate_one(c, entry_n, v, bad, eoc_min, max_cluster);
            entry_n++;
        }

        if (cmp_two) {
            if (!diskio_batch_read((unsigned long)(fat2 + (LBA_t)sec_off),
                                   batch, 0u)) {
                fat_err("FAT 2 batch read failed", errs);
                cmp_two = 0;
            } else {
                page = diskio_batch_map(0u);
                for (si = 0u; si < batch; si++) {
                    if (sec_sum(page + (UINT)si * SECTOR_SIZE) != sums[si]) {
                        diff_sectors++;
                    }
                }
            }
        }

        sec_off += batch;
    }
    fix_verbose_flush();

    diskio_batch_close();

    if (diff_sectors != 0ul) {
        prt_str("  ERROR: FAT 1/2 differ in ");
        prt_dec(diff_sectors);
        prt_str(" sector(s)\r\n");
        (*errs)++;
        fix_count_found();
        if (fix_enabled()) sync_fats(fs, errs);
    }
    return 0;
}
#endif /* CHKDSK_FAT16 || CHKDSK_FAT32 */

#if CHKDSK_FAT12
/* FAT12 path. 12-bit entries, 2 per 3 bytes, may straddle the 512-byte
 * sector boundary -- we use a one-sector cache. Two passes:
 *   1. validate FAT 1 entries via the cached walker.
 *   2. (if n_fats == 2) re-read FAT 1 + FAT 2 sector by sector and
 *      compare via sector sums. Single buffer is reused. */
static LBA_t  g_fat12_cached_sec = 0xFFFFFFFFul;
static LBA_t  g_fat12_fat_lba    = 0ul;
static int   *g_fat12_errs       = (int *)0;

static u8 fat12_read_byte(unsigned long pos)
{
    LBA_t sec = g_fat12_fat_lba + (LBA_t)(pos / SECTOR_SIZE);
    UINT  off = (UINT)(pos % SECTOR_SIZE);
    if (sec != g_fat12_cached_sec) {
        if (disk_read(0, g_fat_a, sec, 1) != RES_OK) {
            fat_err("FAT 1 read failed (FAT12)", g_fat12_errs);
            g_fat12_cached_sec = 0xFFFFFFFFul;
            return 0u;
        }
        g_fat12_cached_sec = sec;
    }
    return g_fat_a[off];
}

static int check_fat12(vol_t *fs, int *errs, cnt_t *c)
{
    int           cmp_two = (fs->n_fats == 2u);
    unsigned long max_cluster = (unsigned long)fs->n_fatent - 1ul;
    unsigned long entry_n = 0ul;
    unsigned long entries_total = (unsigned long)fs->n_fatent;
    unsigned long byte_pos = 0ul;

    g_fat12_fat_lba    = fs->fatbase;
    g_fat12_cached_sec = 0xFFFFFFFFul;
    g_fat12_errs       = errs;

    /* Pass 1: walk FAT 1 entries. */
    while (entry_n < entries_total) {
        u8 b0 = fat12_read_byte(byte_pos);
        u8 b1 = fat12_read_byte(byte_pos + 1u);
        u8 b2 = fat12_read_byte(byte_pos + 2u);
        unsigned long e0 = (unsigned long)b0
                         | (((unsigned long)b1 & 0x0Ful) << 8);
        unsigned long e1 = ((unsigned long)b1 >> 4)
                         | ((unsigned long)b2 << 4);

        validate_one(c, entry_n, e0, BAD12, EOC12_MIN, max_cluster);
        entry_n++;
        if (entry_n >= entries_total) break;
        validate_one(c, entry_n, e1, BAD12, EOC12_MIN, max_cluster);
        entry_n++;
        byte_pos += 3ul;
    }

    /* Pass 2: per-sector sum compare with FAT 2. */
    if (cmp_two) {
        unsigned long diff_sectors = 0ul;
        DWORD         sec;
        for (sec = 0; sec < fs->fsize; sec++) {
            unsigned long s1;
            if (disk_read(0, g_fat_a, fs->fatbase + (LBA_t)sec, 1) != RES_OK) {
                fat_err("FAT 1 re-read failed (compare)", errs);
                break;
            }
            s1 = sec_sum(g_fat_a);
            if (disk_read(0, g_fat_a,
                          fs->fatbase + (LBA_t)fs->fsize + (LBA_t)sec, 1)
                != RES_OK) {
                fat_err("FAT 2 read failed (compare)", errs);
                break;
            }
            if (sec_sum(g_fat_a) != s1) diff_sectors++;
        }
        if (diff_sectors != 0ul) {
            prt_str("  ERROR: FAT 1/2 differ in ");
            prt_dec(diff_sectors);
            prt_str(" sector(s)\r\n");
            (*errs)++;
            fix_count_found();
            if (fix_enabled()) sync_fats(fs, errs);
        }
    }
    return 0;
}
#endif /* CHKDSK_FAT12 */

/* Read FAT[0] and FAT[1] specifically and report. Called after the
 * main walk because the cached sector layout differs by FAT type. */
static void check_special_entries(vol_t *fs, int *errs)
{
    DRESULT       drc;
    u8            media_bpb;
    unsigned long e0;
    unsigned long e1;

    /* Pull media descriptor from BPB. */
    media_bpb = read_media_byte(fs);

    /* Re-read FAT 1 sector 0 to get FAT[0]/FAT[1]. */
    drc = disk_read(0, g_fat_a, fs->fatbase, 1);
    if (drc != RES_OK) {
        fat_err("cannot reread FAT 1 sector 0 for FAT[0]/FAT[1]", errs);
        return;
    }

#if CHKDSK_FAT12
    if (fs->fs_type == FS_FAT12) {
        e0 = (unsigned long)g_fat_a[0]
           | (((unsigned long)g_fat_a[1] & 0x0Ful) << 8);
        e1 = ((unsigned long)g_fat_a[1] >> 4)
           | ((unsigned long)g_fat_a[2] << 4);
        if ((e0 & 0xFFul) != (unsigned long)media_bpb) {
            fat_err("FAT[0] media byte does not match BPB", errs);
        }
        if (e1 < EOC12_MIN) {
            fat_err("FAT[1] not an EOC value", errs);
        }
    } else
#endif
#if CHKDSK_FAT16
    if (fs->fs_type == FS_FAT16) {
        e0 = (unsigned long)g_fat_a[0]
           | ((unsigned long)g_fat_a[1] << 8);
        e1 = (unsigned long)g_fat_a[2]
           | ((unsigned long)g_fat_a[3] << 8);
        if ((e0 & 0xFFul) != (unsigned long)media_bpb) {
            fat_err("FAT[0] media byte does not match BPB", errs);
        }
        if ((e1 & 0xFFFFul) < EOC16_MIN) {
            fat_err("FAT[1] not an EOC value", errs);
        }
        /* FAT16 FAT[1]: bit 15 = clean shutdown, bit 14 = hard-error
         * (1 = OK, 0 = problem). */
        if ((e1 & 0x8000ul) == 0ul) fat_warn("FAT[1] clean-shutdown bit clear (volume dirty)");
        if ((e1 & 0x4000ul) == 0ul) fat_warn("FAT[1] hard-error bit clear");
    } else
#endif
#if CHKDSK_FAT32
    if (fs->fs_type == FS_FAT32) {
        e0 = ld_dword(&g_fat_a[0]) & 0x0FFFFFFFul;
        e1 = ld_dword(&g_fat_a[4]) & 0x0FFFFFFFul;
        if ((e0 & 0xFFul) != (unsigned long)media_bpb) {
            fat_err("FAT[0] media byte does not match BPB", errs);
        }
        if (e1 < EOC32_MIN) {
            fat_err("FAT[1] not an EOC value", errs);
        }
        /* FAT32 FAT[1]: bit 27 = clean shutdown, bit 26 = hard-error. */
        if ((e1 & 0x08000000ul) == 0ul) fat_warn("FAT[1] clean-shutdown bit clear (volume dirty)");
        if ((e1 & 0x04000000ul) == 0ul) fat_warn("FAT[1] hard-error bit clear");
    } else
#endif
    {
        /* unreachable */
        (void)e0; (void)e1;
    }

    prt_str("  FAT[0] media: 0x");
    prt_hex((unsigned long)media_bpb, 2u);
    prt_str(", FAT[1] OK\r\n");
}

int fat_check(vol_t *fs)
{
    int   errs = 0;
    cnt_t c;

    prt_str("Phase 2: FAT tables\r\n");

    cnt_init(&c);

#if CHKDSK_FAT12
    if (fs->fs_type == FS_FAT12) {
        check_fat12(fs, &errs, &c);
    } else
#endif
#if CHKDSK_FAT16 || CHKDSK_FAT32
    if (fs->fs_type == FS_FAT16 || fs->fs_type == FS_FAT32) {
        check_fat_wide(fs, &errs, &c, fs->fs_type == FS_FAT32);
    } else
#endif
    {
        fat_err("unknown FAT type", &errs);
        return errs;
    }

    check_special_entries(fs, &errs);

    prt_str("  free=");
    prt_dec(c.free_n);
    prt_str(" used=");
    prt_dec(c.used_n);
    prt_str(" invalid=");
    prt_dec(c.invalid_n);
    prt_nl();
    if (c.bad_n != 0ul) {
        prt_str("  bad clusters: ");
        prt_dec(c.bad_n);
        prt_nl();
    }
    if (errs == 0) {
        prt_str("  No issues\r\n");
    }
    return errs;
}
