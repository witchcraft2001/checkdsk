/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * fix.c -- write-mode infrastructure. See fix.h.
 */

#include <sprinter.h>
#include "vol.h"
#include "diskio.h"
#include "sectbuf.h"
#include "chain.h"
#include "prt.h"
#include "fix.h"

static u8    g_fix_enabled = 0u;
static u8    g_fix_convert = 0u;
static u8    g_fix_verbose = 0u;
static u8    g_fix_verbose_dirty = 0u;   /* a progress dot is on the current line */
static DWORD g_fix_found      = 0ul;
static DWORD g_fix_applied    = 0ul;
static u8    g_fix_incomplete = 0u;

void fix_enable(void)         { g_fix_enabled = 1u; }
int  fix_enabled(void)        { return g_fix_enabled ? 1 : 0; }
void fix_enable_convert(void) { g_fix_convert = 1u; }
int  fix_convert_enabled(void){ return g_fix_convert ? 1 : 0; }
void fix_enable_verbose(void) { g_fix_verbose = 1u; }
int  fix_verbose_enabled(void){ return g_fix_verbose ? 1 : 0; }

void fix_verbose_tick(void)
{
    if (!g_fix_verbose) return;
    if (!g_fix_verbose_dirty) prt_str("  ");   /* indent for a fresh streak */
    prt_chr('.');
    g_fix_verbose_dirty = 1u;
}

void fix_verbose_flush(void)
{
    if (g_fix_verbose_dirty) {
        prt_nl();
        g_fix_verbose_dirty = 0u;
    }
}
void fix_count_found(void)      { g_fix_found++; }
void fix_count_applied(void)    { g_fix_applied++; }
void fix_count_incomplete(void) { g_fix_incomplete = 1u; }
int  fix_any_found(void)        { return (g_fix_found != 0ul) ? 1 : 0; }
int  fix_any_applied(void)      { return (g_fix_applied != 0ul) ? 1 : 0; }
int  fix_any_incomplete(void)   { return g_fix_incomplete ? 1 : 0; }

int fix_write(LBA_t lba, const BYTE *buf, UINT count)
{
    if (!g_fix_enabled) return 1;
    if (disk_write(0u, buf, lba, count) == RES_OK) return 1;
    fix_count_incomplete();
    return 0;
}

int fix_fat_set(vol_t *fs, DWORD clust, DWORD value)
{
    DWORD       byte_off;
    LBA_t       sect_a;
    UINT        pos;
    const BYTE *vb = (const BYTE *)&value;

    if (!g_fix_enabled) return 1;

    /* CRITICAL: this function does read-modify-write on g_sect_a, which
     * chain.c also uses as its FAT-sector cache via g_cached_sect.
     * Without invalidating chain.c's cache, a subsequent chain_get_entry()
     * can hit a stale `g_cached_sect == X` while g_sect_a actually holds
     * a different sector's bytes -- returning garbage as the FAT entry.
     * That bug cascaded into find_free_fat_cluster() returning clusters
     * already in use, so /F /C clobbered existing directories with
     * LOSTCHN/FILE####.CHK content. Invalidate up front so chain.c
     * re-reads from disk on its next call regardless of which path we
     * take through this function. */
    chain_invalidate();

#if CHKDSK_FAT12
    if (fs->fs_type == FS_FAT12) {
        /* 1.5 bytes per entry. Two raw bytes (b0, b1) hold the cluster's
         * low+high nibbles, packed differently for odd vs even cluster
         * indexes; the pair may straddle a sector boundary so we read
         * up to two FAT sectors, patch each in turn, and mirror to FAT 2. */
        BYTE  b0_old, b1_old, b0_new, b1_new;
        LBA_t sect_b;
        int   straddle;
        byte_off = clust + (clust >> 1);
        sect_a   = fs->fatbase + (LBA_t)(byte_off >> 9);
        pos      = (UINT)(byte_off & 0x1FFu);
        straddle = (pos == 511u);
        sect_b   = straddle ? (sect_a + 1u) : sect_a;

        if (disk_read(0u, g_sect_a, sect_a, 1u) != RES_OK) return 0;
        b0_old = g_sect_a[pos];
        if (straddle) {
            if (disk_read(0u, g_sect_a, sect_b, 1u) != RES_OK) return 0;
            b1_old = g_sect_a[0];
        } else {
            b1_old = g_sect_a[pos + 1u];
        }

        if (clust & 1ul) {
            b0_new = (BYTE)((b0_old & 0x0Fu) | ((value & 0x0Fu) << 4));
            b1_new = (BYTE)((value >> 4) & 0xFFu);
        } else {
            b0_new = (BYTE)(value & 0xFFu);
            b1_new = (BYTE)((b1_old & 0xF0u) | ((value >> 8) & 0x0Fu));
        }

        if (disk_read(0u, g_sect_a, sect_a, 1u) != RES_OK) return 0;
        g_sect_a[pos] = b0_new;
        if (!straddle) g_sect_a[pos + 1u] = b1_new;
        if (!fix_write(sect_a, g_sect_a, 1u)) return 0;
        if (fs->n_fats == 2u
            && !fix_write(sect_a + (LBA_t)fs->fsize, g_sect_a, 1u)) return 0;

        if (straddle) {
            if (disk_read(0u, g_sect_a, sect_b, 1u) != RES_OK) return 0;
            g_sect_a[0] = b1_new;
            if (!fix_write(sect_b, g_sect_a, 1u)) return 0;
            if (fs->n_fats == 2u
                && !fix_write(sect_b + (LBA_t)fs->fsize, g_sect_a, 1u)) return 0;
        }
        return 1;
    }
#endif

    {
        UINT shift;
        if (fs->fs_type == FS_FAT32)      shift = 2u;
        else if (fs->fs_type == FS_FAT16) shift = 1u;
        else                              return 0;

        byte_off = (shift == 2u) ? (clust << 2) : (clust << 1);
        sect_a   = fs->fatbase + (LBA_t)(byte_off >> 9);
        pos      = (UINT)(byte_off & 0x1FFu);

        if (disk_read(0u, g_sect_a, sect_a, 1u) != RES_OK) return 0;

        g_sect_a[pos]      = vb[0];
        g_sect_a[pos + 1u] = vb[1];
        if (shift == 2u) {
            g_sect_a[pos + 2u] = vb[2];
            /* FAT32 entries are 28-bit; preserve the reserved upper nibble. */
            g_sect_a[pos + 3u] = (g_sect_a[pos + 3u] & 0xF0u) | (vb[3] & 0x0Fu);
        }

        if (!fix_write(sect_a, g_sect_a, 1u)) return 0;
        if (fs->n_fats == 2u
            && !fix_write(sect_a + (LBA_t)fs->fsize, g_sect_a, 1u)) return 0;
    }
    return 1;
}

int fix_dir_patch(LBA_t sect, WORD off, u8 kind, DWORD value)
{
    const BYTE *vb = (const BYTE *)&value;  /* SDCC z80 stores LSB first */

    if (!g_fix_enabled) return 1;
    /* See fix_fat_set for why this is required. */
    chain_invalidate();
    if (disk_read(0u, g_sect_a, sect, 1u) != RES_OK) return 0;
    switch (kind) {
    case FIX_DPATCH_DELETE:
        g_sect_a[off] = 0xE5u;
        break;
    case FIX_DPATCH_DOT_CLUST:
        g_sect_a[off + 20u] = vb[2];
        g_sect_a[off + 21u] = vb[3];
        g_sect_a[off + 26u] = vb[0];
        g_sect_a[off + 27u] = vb[1];
        break;
    case FIX_DPATCH_ATTR_MASK:
        g_sect_a[off + 11u] &= 0x3Fu;
        break;
    case FIX_DPATCH_NTRES_FIX:
        g_sect_a[off + 12u] &= 0x18u;
        g_sect_a[off + 13u]  = 0u;
        break;
    case FIX_DPATCH_HI_CLUST_ZERO:
        g_sect_a[off + 20u] = 0u;
        g_sect_a[off + 21u] = 0u;
        break;
    default: /* FIX_DPATCH_SIZE */
        g_sect_a[off + 28u] = vb[0];
        g_sect_a[off + 29u] = vb[1];
        g_sect_a[off + 30u] = vb[2];
        g_sect_a[off + 31u] = vb[3];
        break;
    }
    if (!fix_write(sect, g_sect_a, 1u)) return 0;
    fix_count_applied();
    return 1;
}

int fix_dir_name_set(LBA_t sect, WORD off, const BYTE *new_name)
{
    UINT i;

    if (!g_fix_enabled) return 1;
    chain_invalidate();
    if (disk_read(0u, g_sect_a, sect, 1u) != RES_OK) return 0;
    for (i = 0u; i < 11u; i++) g_sect_a[off + i] = new_name[i];
    if (!fix_write(sect, g_sect_a, 1u)) return 0;
    fix_count_applied();
    return 1;
}

#define FIX_ATTR_LFN 0x0Fu

int fix_delete_preceding_lfn(LBA_t sect, WORD off, LBA_t dir_start_sect)
{
    WORD scan_off;
    WORD del_count;

    if (!g_fix_enabled) return 1;

    /* The directory's very first slot has nothing before it at all --
     * not ambiguous, just empty. Every other off==0 means "this is a
     * later sector of the directory and we can't see the one before
     * it", which IS ambiguous (handled below). */
    if (off == 0u && sect == dir_start_sect) return 1;

    /* Pass 1: read-only. Walk backward one slot at a time while the
     * preceding slot is LFN, staying inside this sector. */
    if (disk_read(0u, g_sect_a, sect, 1u) != RES_OK) return 0;
    scan_off  = off;
    del_count = 0u;
    while (scan_off >= 32u
           && (g_sect_a[scan_off - 32u + 11u] & 0x3Fu) == FIX_ATTR_LFN) {
        scan_off -= 32u;
        del_count++;
    }
    if (scan_off == 0u) {
        /* Either off itself was this sector's first slot (no
         * visibility into whatever precedes this sector), or the walk
         * consumed every slot down to offset 0 and slot 0 was itself
         * LFN (the run may continue into the previous sector). Either
         * way we cannot rule out more of the run sitting where we
         * cannot see it -- abstain, no writes at all. */
        return 0;
    }
    if (del_count == 0u) return 1;   /* no LFN precedes; nothing to do */

    chain_invalidate();
    scan_off = off;
    while (del_count-- > 0u) {
        scan_off -= 32u;
        if (disk_read(0u, g_sect_a, sect, 1u) != RES_OK) return 0;
        g_sect_a[scan_off] = 0xE5u;
        if (!fix_write(sect, g_sect_a, 1u)) return 0;
        fix_count_applied();
    }
    return 1;
}

void fix_print_summary(void)
{
    if (g_fix_found == 0ul) return;

    if (g_fix_enabled) {
        prt_str("Fixes: found=");
        prt_dec((unsigned long)g_fix_found);
        prt_str(" applied=");
        prt_dec((unsigned long)g_fix_applied);
        prt_nl();
        /* A repair can introduce new entries the walker never saw on
         * this pass (e.g. /CONVERT creates FILE####.CHK in the root,
         * EXCESS truncate orphans the trailing chain). Encourage the
         * user to rerun so those follow-on findings get reported. */
        if (g_fix_applied != 0ul) {
            prt_str("Re-run chkdsk to verify the volume is now clean.\r\n");
        }
    } else {
        prt_str("Found ");
        prt_dec((unsigned long)g_fix_found);
        prt_str(" issue(s); run with /F to apply repairs\r\n");
    }
}
