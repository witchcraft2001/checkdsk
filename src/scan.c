/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * scan.c -- Phase 3 directory walk + Phase 4 lost-cluster sweep.
 * See scan.h.
 *
 * Phase 3 is an iterative depth-first walk over the directory tree
 * starting at the root. An explicit stack of dirwalk frames caps the
 * recursion depth at SCAN_MAX_DEPTH and keeps the C call stack flat
 * -- the per-frame state (dirwalk position, LFN-group accumulator,
 * etc.) lives in BSS.
 *
 * Cycle / cross-link detection uses bitmap_test_and_set on the first
 * cluster of every sub-directory the walker enters. A bit already set
 * means either a real loop in the FAT (".." pointing forward) or two
 * directory entries claiming the same starting cluster; the walker
 * refuses to descend in either case.
 *
 * "." / ".." are special-cased near the top of step(): their cluster
 * pointers must equal the current dir's start_cluster (".") or the
 * parent's start_cluster (".." -- 0 if the parent is the root). They
 * are not counted toward the per-walk entry totals.
 *
 * LFN slots run a small per-walker state machine that verifies the
 * descending order byte, that all slots agree on the checksum byte,
 * and that the SFN that follows produces the same checksum. A 0xE5
 * slot (deleted SFN) or any sequence break cancels the running group.
 *
 * Output policy: only flagged entries (and cycle / depth-cap warnings)
 * are echoed, prefixed by their full path. Clean entries are silent;
 * the trailing Totals line gives the population counts.
 */

#include <sprinter.h>
#include "vol.h"
#include "diskio.h"
#include "sectbuf.h"
#include "bitmap.h"
#include "chain.h"
#include "dirwalk.h"
#include "dirent.h"
#include "fix.h"
#include "prt.h"
#include "scan.h"

#define SCAN_MAX_DEPTH  10u

#define ATTR_VOLID 0x08u
#define ATTR_DIR   0x10u
#define ATTR_LFN   0x0Fu

typedef struct {
    dirwalk_t walker;
    BYTE      name[11];      /* parent SFN, valid for depth >= 1 */
    DWORD     start_cluster; /* first cluster of this directory (0 for FAT12/16 root) */
    BYTE      lfn_count;     /* LFN slots accumulated since last SFN reset (0 = none) */
    BYTE      lfn_checksum;  /* checksum byte (offset 13) from the first LFN slot */
    BYTE      lfn_next_ord;  /* expected order byte for the next LFN slot (descending) */
} scan_frame_t;

static scan_frame_t g_frames[SCAN_MAX_DEPTH + 1u];

/* Aggregate counters for the whole walk. */
typedef struct {
    DWORD entries;
    DWORD dirs;
    DWORD flagged;
    DWORD cycles;        /* first-cluster cross-link (cycle / dup ref) */
    DWORD cross_links;   /* mid-chain cross-links (clusters seen twice) */
    DWORD broken_chains; /* invalid link or BAD encountered mid-chain */
    DWORD truncated;     /* file size > chain length */
    DWORD excess;        /* file size < chain length */
    DWORD depth_capped;
} scan_totals_t;

/* Chain-walk findings, flag bits returned by walk_chain. */
#define CW_CYCLE      0x01u   /* first cluster already set in bitmap */
#define CW_CROSS      0x02u   /* mid-chain cluster already set */
#define CW_BROKEN     0x04u   /* invalid link encountered */
#define CW_BAD        0x08u   /* FAT entry == BAD mid-chain */
#define CW_IO_ERR     0x10u   /* disk_read failure */
#define CW_TRUNCATED  0x20u   /* file size implies more clusters than chain */
#define CW_EXCESS     0x40u   /* file size implies fewer clusters than chain */

static DWORD entry_first_cluster(const BYTE *e)
{
    DWORD x;
    BYTE *xb = (BYTE *)&x;
    xb[0] = e[26]; xb[1] = e[27]; xb[2] = e[20]; xb[3] = e[21];
    return x;
}

static DWORD ld_size(const BYTE *e)
{
    DWORD x;
    BYTE *xb = (BYTE *)&x;
    xb[0] = e[28]; xb[1] = e[29]; xb[2] = e[30]; xb[3] = e[31];
    return x;
}

/* 1 = ".", 2 = "..", 0 = neither. */
static int is_dot_entry(const BYTE *e)
{
    UINT i;
    UINT skip;

    if (e[0] != '.') return 0;
    if (e[1] != '.' && e[1] != ' ') return 0;
    skip = (e[1] == '.') ? 2u : 1u;
    for (i = skip; i < 11u; i++) {
        if (e[i] != ' ') return 0;
    }
    return (skip == 2u) ? 2 : 1;
}

static void print_sfn(const BYTE *raw)
{
    UINT i;
    BYTE c;
    int  emitted_dot = 0;

    for (i = 0u; i < 8u; i++) {
        c = raw[i];
        if (c == ' ') break;
        if (c < 0x20u || c >= 0x7Fu) c = '?';
        prt_chr((char)c);
    }
    for (i = 8u; i < 11u; i++) {
        c = raw[i];
        if (c == ' ') continue;
        if (!emitted_dot) { prt_chr('.'); emitted_dot = 1; }
        if (c < 0x20u || c >= 0x7Fu) c = '?';
        prt_chr((char)c);
    }
}

static void print_path(BYTE depth)
{
    BYTE i;
    prt_chr('/');
    for (i = 1u; i <= depth; i++) {
        print_sfn(g_frames[i].name);
        prt_chr('/');
    }
}

static void copy_name(BYTE *dst, const BYTE *src)
{
    BYTE i;
    for (i = 0u; i < 11u; i++) dst[i] = src[i];
}

/* Returns 1 if the entry can be descended into (a real subdir, not
 * "." / ".."), 0 otherwise. A directory with non-zero size, or any
 * DE_ANY_ERROR flag set on the entry, is treated as corrupt and not
 * entered -- otherwise we read random data as 32-byte directory
 * entries and emit a cascade of garbage findings.
 *
 * The dir-corrupt repair (size-zero or 0xE5) is irreversible -- once
 * an originally-corrupted file has had its size DWORD zeroed, dflags
 * comes back clean and only the actual cluster contents tell the
 * truth. dir_has_dot_entry handles that second-line check. */
static int is_descendable_dir(const BYTE *e, UINT dflags)
{
    DWORD size;
    if (e[0] == 0xE5u)                  return 0;
    if ((e[11] & 0x3Fu) == ATTR_LFN)    return 0;
    if (!(e[11] & ATTR_DIR))            return 0;
    if (e[11] & ATTR_VOLID)             return 0;
    if (is_dot_entry(e))                return 0;
    size = ld_size(e);
    if (size != 0ul)                    return 0;
    if (dflags & DE_ANY_ERROR)          return 0;
    return 1;
}

/* SFN checksum used by LFN slots (offset 13). FAT spec algorithm:
 * rotate the running sum right by one bit, then add the next of the
 * 11 SFN name bytes. Each LFN slot carries this byte verbatim, and
 * the sequence is invalid if any LFN slot disagrees with its peers
 * or with the SFN that follows. */
static u8 sfn_checksum(const BYTE *name11)
{
    u8   sum = 0u;
    UINT i;
    for (i = 0u; i < 11u; i++) {
        sum = (u8)(((sum & 1u) << 7) | (sum >> 1));
        sum = (u8)(sum + name11[i]);
    }
    return sum;
}

/* Peek at the first 32-byte slot of `clust` -- a real subdirectory's
 * first slot is the "." entry (a '.' followed by 10 spaces). If the
 * first byte isn't '.', `clust` points at user data, not a directory,
 * and we must not descend (otherwise random file content gets parsed
 * as 32-byte dir entries and produces a cascade of garbage findings).
 * Catches Stage-4.2-corrupted files masquerading as empty dirs after
 * their size DWORD has been zeroed on a prior /F run.
 *
 * Side effect: clobbers g_sect_a -- caller must mark the parent
 * walker buffer_dirty afterwards. */
static int dir_has_dot_entry(vol_t *fs, DWORD clust)
{
    LBA_t lba = chain_cluster_to_lba(fs, clust);
    if (disk_read(0u, g_sect_a, lba, 1u) != RES_OK) return 0;
    chain_invalidate();
    return (g_sect_a[0] == '.') ? 1 : 0;
}

/* Walk the FAT chain at `start`, marking visited clusters in the bitmap.
 * Stops at EOC, BAD, invalid link, I/O error, or test_and_set hit.
 * Returns CW_* flag bits; chain length is written to *len_out (0 means
 * "we never marked anything", e.g. cycle on the first cluster).
 * g_sect_a ends up holding a FAT sector -- callers using a dirwalk must
 * mark its buffer dirty after returning. */
static UINT walk_chain(vol_t *fs, DWORD start, DWORD *len_out)
{
    DWORD c = start;
    DWORD next;
    DWORD len = 0ul;
    UINT  flags = 0u;

    if (bitmap_test_and_set(c)) { *len_out = 0ul; return CW_CYCLE; }
    len = 1ul;

    for (;;) {
        next = chain_get_entry(fs, c);
        if (next == CHAIN_READ_ERROR)              { flags |= CW_IO_ERR; break; }
        if (chain_is_bad(fs, next))                { flags |= CW_BAD | CW_BROKEN; break; }
        if (chain_is_eoc(fs, next))                                                 break;
        if (next < 2ul || next >= fs->n_fatent)    { flags |= CW_BROKEN; break; }
        if (bitmap_test_and_set(next))             { flags |= CW_CROSS; break; }
        len++;
        c = next;
    }
    *len_out = len;
    return flags;
}

static void print_cw_tags(UINT cflags)
{
    if (cflags & CW_CYCLE)        prt_str(" cycle");
    else if (cflags & CW_CROSS)   prt_str(" cross-link");
    if (cflags & CW_BAD)          prt_str(" bad-cluster");
    else if (cflags & CW_BROKEN)  prt_str(" broken");
    if (cflags & CW_IO_ERR)       prt_str(" io-err");
    if (cflags & CW_TRUNCATED)    prt_str(" truncated");
    if (cflags & CW_EXCESS)       prt_str(" excess");
}

/* Emit "  /A/B/NAME.EXT" -- the full path of a flagged entry. */
static void print_flagged(BYTE depth, const BYTE *e)
{
    prt_str("  ");
    print_path(depth);
    if ((e[11] & 0x3Fu) == ATTR_LFN) {
        prt_str("[lfn ord=0x");
        prt_hex((unsigned long)(e[0] & 0xFFu), 2u);
        prt_chr(']');
    } else {
        print_sfn(e);
    }
}

/* Process one entry of the dir at top-of-stack. Returns:
 *   1 -- continued, stay on same frame
 *   0 -- popped (frame finished)
 *  -1 -- I/O error (bubble up)
 *
 * IMPORTANT: dirwalk_next returns a pointer INTO g_sect_a. walk_chain
 * uses g_sect_a as its FAT-sector cache, so it clobbers that pointer.
 * We snapshot the 32-byte entry into a local buffer before doing any
 * I/O that touches g_sect_a, and operate on the snapshot afterwards. */
static int step(vol_t *fs, BYTE *depth, scan_totals_t *t)
{
    scan_frame_t *frame;
    BYTE         *src;
    BYTE          ent[32];
    int           rc;
    UINT          i;
    UINT          dflags;
    UINT          cflags;
    DWORD         clust;
    DWORD         size;
    DWORD         chain_len;
    int           is_file;
    int           has_dot;   /* -1=not peeked, 0=garbage, 1=real subdir */

    frame = &g_frames[*depth];
    rc = dirwalk_next(&frame->walker, &src);
    if (rc <  0) return -1;
    if (rc == 0) {
        if (*depth > 0u) {
            (*depth)--;
            dirwalk_buffer_dirty(&g_frames[*depth].walker);
        } else {
            (*depth) = 0xFFu;
        }
        return 0;
    }

    for (i = 0u; i < 32u; i++) ent[i] = src[i];

    /* LFN sequence + SFN-checksum check. Walk-order assumes LFN slots
     * immediately precede their SFN. The first slot of a group has
     * the 0x40 flag set in the order byte; subsequent slots carry
     * decreasing orders. All slots in a group must agree on the
     * checksum byte, and the SFN that follows must produce the same
     * checksum. A deleted entry (0xE5) cancels any pending LFN group. */
    if (ent[0] == 0xE5u) {
        frame->lfn_count = 0u;
    } else if ((ent[11] & 0x3Fu) == ATTR_LFN) {
        BYTE ord = ent[0];
        BYTE chk = ent[13];
        int  bad = 0;
        if (ord & 0x40u) {
            frame->lfn_count    = 1u;
            frame->lfn_checksum = chk;
            frame->lfn_next_ord = (BYTE)((ord & 0x3Fu) - 1u);
        } else if (frame->lfn_count > 0u) {
            if (ord != frame->lfn_next_ord || chk != frame->lfn_checksum) {
                bad = 1;
            } else {
                frame->lfn_count++;
                if (frame->lfn_next_ord > 0u) frame->lfn_next_ord--;
            }
        } else {
            bad = 1;       /* orphan continuation slot, no 0x40 start */
        }
        if (bad) {
            prt_str("  ");
            print_path(*depth);
            prt_str("[lfn ord=0x");
            prt_hex((unsigned long)ord, 2u);
            prt_str("] * lfn-seq\r\n");
            t->flagged++;
            fix_count_found();
            frame->lfn_count = 0u;
        }
        t->entries++;
        return 1;
    } else if (frame->lfn_count > 0u) {
        if (sfn_checksum(ent) != frame->lfn_checksum) {
            prt_str("  ");
            print_path(*depth);
            print_sfn(ent);
            prt_str(" * lfn-checksum\r\n");
            t->flagged++;
            fix_count_found();
        }
        frame->lfn_count = 0u;
    }

    /* Validate "." / ".." cluster pointers, then continue iteration
     * without counting them as regular entries.
     *   "."  must point at the directory's own first cluster
     *   ".." must point at the parent's first cluster, or 0 when the
     *        parent is the root (per FAT spec, regardless of FAT type).
     * Only applies inside a subdirectory (depth >= 1) -- the root is
     * walked via dirwalk_open_root and contains no dot entries. */
    if ((ent[11] & 0x3Fu) != ATTR_LFN && ent[0] != 0xE5u) {
        int dot_kind = is_dot_entry(ent);
        if (dot_kind != 0) {
            if (*depth >= 1u) {
                DWORD got_clust = entry_first_cluster(ent);
                DWORD expected;
                if (dot_kind == 1) {
                    expected = frame->start_cluster;
                } else {
                    expected = (*depth == 1u)
                               ? 0ul
                               : g_frames[*depth - 1u].start_cluster;
                }
                if (got_clust != expected) {
                    prt_str("  ");
                    print_path(*depth);
                    prt_str((dot_kind == 1) ? "." : "..");
                    prt_str(" * dot-clust got=");
                    prt_dec((unsigned long)got_clust);
                    prt_str(" expect=");
                    prt_dec((unsigned long)expected);
                    prt_nl();
                    t->flagged++;
                    fix_count_found();
                    if (fix_enabled()) {
                        LBA_t sect; WORD off;
                        dirwalk_last_entry_location(&frame->walker, &sect, &off);
                        if (fix_dir_patch(sect, off, FIX_DPATCH_DOT_CLUST, expected)) {
                            dirwalk_buffer_dirty(&frame->walker);
                        } else {
                            prt_str("  WARN: dot-clust repair failed\r\n");
                        }
                    }
                }
            }
            return 1;
        }
    }

    t->entries++;
    if (fix_verbose_enabled() && (t->entries & 0x7Ful) == 0ul) prt_chr('.');
    dflags    = dirent_validate(fs, ent);
    cflags    = 0u;
    chain_len = 0ul;

    clust   = entry_first_cluster(ent);
    is_file = ((ent[11] & 0x3Fu) != ATTR_LFN) && !(ent[11] & ATTR_DIR)
            && !(ent[11] & ATTR_VOLID) && (ent[0] != 0xE5u);
    size    = ld_size(ent);

    if (ent[0] != 0xE5u && clust >= 2ul && clust < fs->n_fatent) {
        cflags |= walk_chain(fs, clust, &chain_len);
        dirwalk_buffer_dirty(&g_frames[*depth].walker);

        if (is_file && size != 0ul
            && (cflags & (CW_BROKEN | CW_BAD | CW_IO_ERR | CW_CYCLE | CW_CROSS)) == 0u) {
            DWORD cluster_bytes = (DWORD)fs->csize << 9;
            DWORD expected      = (size + cluster_bytes - 1ul) / cluster_bytes;
            if (chain_len < expected)      cflags |= CW_TRUNCATED;
            else if (chain_len > expected) cflags |= CW_EXCESS;
        }
    }

    /* Peek at the first cluster of any ATTR_DIR entry that has a usable
     * cluster pointer: the result drives both the dir-corrupt repair
     * (real subdir vs file-masquerading-as-dir) and the descent guard.
     * Skipped for "." / ".." (filtered out at the top), 0xE5 slots,
     * volume labels, LFN slots, and out-of-range cluster pointers --
     * those can't be descended into anyway. */
    has_dot = -1;
    if (ent[0] != 0xE5u && (ent[11] & 0x3Fu) != ATTR_LFN
        && (ent[11] & ATTR_DIR) && !(ent[11] & ATTR_VOLID)
        && clust >= 2ul && clust < fs->n_fatent
        && (cflags & (CW_CYCLE | CW_IO_ERR)) == 0u) {
        has_dot = dir_has_dot_entry(fs, clust);
        dirwalk_buffer_dirty(&frame->walker);
    }

    {
        UINT de_err = dflags & DE_ANY_ERROR;
        if (de_err || cflags) {
            print_flagged(*depth, ent);
            prt_str(" *");
            if (de_err) dirent_flags_print(dflags);
            if (cflags) print_cw_tags(cflags);
            prt_nl();
        }
        if (de_err) { t->flagged++; fix_count_found(); }

        /* ATTR_DIR with a non-zero size DWORD. Two flavours:
         *   has_dot == 1 -- a real subdir whose size got corrupted; just
         *                   zero the size DWORD, structure stays intact.
         *   otherwise    -- a file whose attribute byte got corrupted to
         *                   include ATTR_DIR; mark the entry deleted so
         *                   the walker can't be tricked into descending
         *                   into user data. The cluster chain becomes
         *                   orphaned and is later recovered by the
         *                   Phase 4 lost-cluster sweep. */
        if (dflags & DE_DIR_NONZERO_SIZE) {
            LBA_t sect; WORD off;
            int   ok;
            dirwalk_last_entry_location(&frame->walker, &sect, &off);
            ok = (has_dot == 1)
                 ? fix_dir_patch(sect, off, FIX_DPATCH_SIZE, 0ul)
                 : fix_dir_patch(sect, off, FIX_DPATCH_DELETE, 0ul);
            if (ok) {
                if (fix_enabled()) {
                    /* Sector reload clobbered g_sect_a -- walker stale. */
                    dirwalk_buffer_dirty(&frame->walker);
                }
            } else {
                prt_str("  WARN: dir-corrupt repair failed\r\n");
            }
        }
    }

    if (cflags & CW_CYCLE)     { t->cycles++;         fix_count_found(); }
    if (cflags & CW_CROSS)     { t->cross_links++;    fix_count_found(); }
    if (cflags & CW_BROKEN)    { t->broken_chains++;  fix_count_found(); }
    if (cflags & CW_TRUNCATED) { t->truncated++;      fix_count_found(); }
    if (cflags & CW_EXCESS)    { t->excess++;         fix_count_found(); }

    /* Shrink the file's size DWORD to match the actual chain coverage
     * when the chain was cut short or hit garbage. Covers TRUNCATED
     * (clean EOC earlier than expected), BROKEN/BAD (hard error mid-
     * chain), and CROSS (chain ran into territory already claimed by
     * another file). */
    if (is_file && size != 0ul && chain_len != 0ul
        && (cflags & (CW_BROKEN | CW_BAD | CW_TRUNCATED | CW_CROSS)) != 0u) {
        DWORD cluster_bytes  = (DWORD)fs->csize << 9;
        DWORD chain_capacity = chain_len * cluster_bytes;
        if (size > chain_capacity) {
            LBA_t sect; WORD off;
            dirwalk_last_entry_location(&frame->walker, &sect, &off);
            if (fix_dir_patch(sect, off, FIX_DPATCH_SIZE, chain_capacity)) {
                if (fix_enabled()) dirwalk_buffer_dirty(&frame->walker);
            } else {
                prt_str("  WARN: chain-size repair failed\r\n");
            }
        }
    }

    /* Chain runs longer than the file's size requires (CW_EXCESS).
     * Walk forward `expected - 1` steps to find the last cluster to
     * keep, write EOC there to terminate the chain, then walk and
     * free the trailing portion (FAT entries set to 0). All FAT
     * writes go through fix_fat_set, which mirrors into FAT 2 if
     * n_fats == 2. The whole truncate+free counts as one fix. */
    if (is_file && (cflags & CW_EXCESS) != 0u && fix_enabled()
        && size != 0ul && clust >= 2ul && clust < fs->n_fatent) {
        DWORD cluster_bytes = (DWORD)fs->csize << 9;
        DWORD expected = (size + cluster_bytes - 1ul) / cluster_bytes;
        DWORD eoc = (fs->fs_type == FS_FAT32) ? 0x0FFFFFFFul : 0xFFFFul;
        DWORD c   = clust;
        DWORD i;
        int   ok  = 1;

        for (i = 1ul; i < expected && ok; i++) {
            DWORD nx = chain_get_entry(fs, c);
            if (nx < 2ul || nx >= fs->n_fatent) { ok = 0; break; }
            c = nx;
        }
        if (ok) {
            DWORD chop = chain_get_entry(fs, c);
            if (chop == CHAIN_READ_ERROR)      ok = 0;
            else if (!fix_fat_set(fs, c, eoc)) ok = 0;
            else {
                while (chop >= 2ul && chop < fs->n_fatent
                       && !chain_is_eoc(fs, chop)
                       && !chain_is_bad(fs, chop)) {
                    DWORD nn = chain_get_entry(fs, chop);
                    if (nn == CHAIN_READ_ERROR)      { ok = 0; break; }
                    if (!fix_fat_set(fs, chop, 0ul)) { ok = 0; break; }
                    chop = nn;
                }
            }
        }
        if (ok) fix_count_applied();
        else    prt_str("  WARN: chain-truncate repair failed\r\n");
        chain_invalidate();
        dirwalk_buffer_dirty(&frame->walker);
    }



    if (!is_descendable_dir(ent, dflags)) return 1;
    if (clust < 2ul || clust >= fs->n_fatent) return 1;
    if (cflags & (CW_CYCLE | CW_IO_ERR)) return 1;
    if (has_dot != 1) return 1;

    if (*depth >= SCAN_MAX_DEPTH) {
        prt_str("  depth-limit ");
        print_path(*depth);
        print_sfn(ent);
        prt_nl();
        t->depth_capped++;
        return 1;
    }

    (*depth)++;
    t->dirs++;
    copy_name(g_frames[*depth].name, ent);
    g_frames[*depth].start_cluster = clust;
    g_frames[*depth].lfn_count = 0u;
    dirwalk_open_chain(&g_frames[*depth].walker, fs, clust);
    dirwalk_buffer_dirty(&g_frames[*depth - 1u].walker);
    return 1;
}

/* Print " label=N" only if N != 0. Compresses the totals line output. */
static void emit_count(const char *label, DWORD val)
{
    if (val == 0ul) return;
    prt_str(label);
    prt_dec((unsigned long)val);
}

static int walk_tree(vol_t *fs, scan_totals_t *t)
{
    BYTE depth;
    int  rc;

    t->entries       = 0ul;
    t->dirs          = 0ul;
    t->flagged       = 0ul;
    t->cycles        = 0ul;
    t->cross_links   = 0ul;
    t->broken_chains = 0ul;
    t->truncated     = 0ul;
    t->excess        = 0ul;
    t->depth_capped  = 0ul;

    depth = 0u;
    /* Root frame: start_cluster only used by dot validation, which is
     * gated on depth >= 1. Set to fs->dirbase on FAT32 (the actual
     * root cluster) and 0 on FAT12/16 for self-consistency, even
     * though no dots will be checked here. */
    g_frames[0].start_cluster =
        (fs->fs_type == FS_FAT32) ? (DWORD)fs->dirbase : 0ul;
    g_frames[0].lfn_count = 0u;
    dirwalk_open_root(&g_frames[0].walker, fs);

    for (;;) {
        rc = step(fs, &depth, t);
        if (rc < 0) return -1;
        if (depth == 0xFFu) break;
    }
    return 0;
}

/* Read a FAT entry value out of g_sect_a at byte offset `pos`.
 * shift = 1 (FAT16, 2 bytes) or 2 (FAT32, 4 bytes; high nibble masked). */
static DWORD read_fat_entry_in_buf(UINT pos, UINT shift)
{
    DWORD v = (DWORD)g_sect_a[pos] | ((DWORD)g_sect_a[pos + 1u] << 8);
    if (shift == 2u) {
        v |= ((DWORD)g_sect_a[pos + 2u] << 16)
           | ((DWORD)(g_sect_a[pos + 3u] & 0x0Fu) << 24);
    }
    return v;
}

/* Pre-pass for /CONVERT. For each cluster c that is orphan (bit unset
 * in the post-walk_tree bitmap) and whose FAT entry is a forward link
 * to a valid cluster v, mark v in the bitmap. After this pass, any
 * orphan cluster c with bitmap[c]==0 is a chain HEAD: nothing in the
 * orphan subgraph references it. Entirely-cyclic orphan groups have
 * no head and are not converted. */
static int phase4_premark(vol_t *fs, UINT shift, UINT n_per, DWORD bad)
{
    DWORD sec_idx;
    LBA_t fat1 = fs->fatbase;

    for (sec_idx = 0ul; sec_idx < fs->fsize; sec_idx++) {
        DWORD start_c = (shift == 2u) ? (sec_idx << 7) : (sec_idx << 8);
        UINT cc_in;
        if (start_c >= fs->n_fatent) break;
        if (disk_read(0u, g_sect_a, fat1 + (LBA_t)sec_idx, 1u) != RES_OK) return -1;
        chain_invalidate();
        for (cc_in = 0u; cc_in < n_per; cc_in++) {
            DWORD cc = start_c + (DWORD)cc_in;
            DWORD v;
            if (cc < 2ul || cc >= fs->n_fatent) continue;
            if (bitmap_get((u32)cc)) continue;
            v = read_fat_entry_in_buf(cc_in << shift, shift);
            if (v < 2ul || v >= fs->n_fatent || v == bad) continue;
            bitmap_set((u32)v);
        }
    }
    return 0;
}

/* Walk an orphan chain starting at cluster c. Mark each cluster in the
 * bitmap (test_and_set protects against cycles). Returns chain length. */
static DWORD phase4_walk_chain(vol_t *fs, DWORD c)
{
    DWORD len = 0ul;
    while (c >= 2ul && c < fs->n_fatent) {
        DWORD nx;
        if (bitmap_test_and_set((u32)c)) break;
        len++;
        nx = chain_get_entry(fs, c);
        if (nx == CHAIN_READ_ERROR) break;
        if (chain_is_eoc(fs, nx) || chain_is_bad(fs, nx)) break;
        c = nx;
    }
    return len;
}

/* Format "FILE####" + "CHK" into 11 raw SFN bytes. seq up to 9999. */
static void format_chk_name(BYTE *out11, DWORD seq)
{
    UINT i;
    out11[0] = 'F'; out11[1] = 'I'; out11[2] = 'L'; out11[3] = 'E';
    for (i = 0u; i < 4u; i++) {
        out11[7u - i] = (BYTE)('0' + (BYTE)(seq % 10ul));
        seq /= 10ul;
    }
    out11[8] = 'C'; out11[9] = 'H'; out11[10] = 'K';
}

/* Find the first free 32-byte slot in the root directory (byte 0 ==
 * 0xE5 deleted, or 0x00 end-of-dir). Returns 1 with sect/off filled
 * on success, 0 on root full / I/O error. Clobbers g_sect_a. */
static int find_free_root_slot(vol_t *fs, LBA_t *out_sect, WORD *out_off)
{
#if CHKDSK_FAT12 || CHKDSK_FAT16
    if (fs->fs_type != FS_FAT32) {
        DWORD ent_idx;
        LBA_t cur_sec = 0xFFFFFFFFul;
        for (ent_idx = 0ul; ent_idx < (DWORD)fs->n_rootdir; ent_idx++) {
            LBA_t sec = fs->dirbase + (LBA_t)(ent_idx >> 4);
            UINT  off = (UINT)((ent_idx & 0x0Fu) << 5);
            if (sec != cur_sec) {
                if (disk_read(0u, g_sect_a, sec, 1u) != RES_OK) return 0;
                cur_sec = sec;
            }
            if (g_sect_a[off] == 0x00u || g_sect_a[off] == 0xE5u) {
                *out_sect = sec;
                *out_off  = (WORD)off;
                return 1;
            }
        }
        return 0;
    }
#endif
#if CHKDSK_FAT32
    {
        DWORD c = (DWORD)fs->dirbase;
        UINT  scnt = fs->csize;
        for (;;) {
            UINT i;
            for (i = 0u; i < scnt; i++) {
                LBA_t sec = chain_cluster_to_lba(fs, c) + (LBA_t)i;
                UINT off;
                if (disk_read(0u, g_sect_a, sec, 1u) != RES_OK) return 0;
                for (off = 0u; off < 512u; off += 32u) {
                    if (g_sect_a[off] == 0x00u || g_sect_a[off] == 0xE5u) {
                        *out_sect = sec;
                        *out_off  = (WORD)off;
                        return 1;
                    }
                }
            }
            {
                DWORD nx = chain_get_entry(fs, c);
                chain_invalidate();
                if (nx < 2ul || nx >= fs->n_fatent || chain_is_eoc(fs, nx)) return 0;
                c = nx;
            }
        }
    }
#endif
    return 0;
}

/* Write a 32-byte SFN entry at (sect, off) describing a FILE####.CHK
 * file: archive attr, no time/date, first cluster + size set from
 * the orphan chain. Re-reads sector, patches in place, writes back. */
static int write_chk_entry(LBA_t sect, WORD off, const BYTE *name11,
                           DWORD cluster, DWORD size)
{
    UINT  i;
    BYTE *e;
    BYTE *cb;

    if (!fix_enabled()) return 1;
    if (disk_read(0u, g_sect_a, sect, 1u) != RES_OK) return 0;
    e = &g_sect_a[off];
    for (i = 0u;  i < 11u; i++) e[i] = name11[i];
    e[11] = 0x20u;                              /* ATTR_ARCHIVE */
    for (i = 12u; i < 28u; i++) e[i] = 0u;      /* reserved + time/date */
    cb = (BYTE *)&cluster;
    e[20] = cb[2]; e[21] = cb[3];               /* FstClusHI */
    e[26] = cb[0]; e[27] = cb[1];               /* FstClusLO */
    cb = (BYTE *)&size;
    e[28] = cb[0]; e[29] = cb[1]; e[30] = cb[2]; e[31] = cb[3];
    return fix_write(sect, g_sect_a, 1u);
}

/* Phase 4: lost cluster scan with two repair modes.
 *
 *   default /F: zero each orphan FAT entry in place (free).
 *   /F /C:      identify each orphan chain head and create a
 *               FILE####.CHK entry in the root pointing at it.
 *
 * Bitmap state at entry: 1 = reachable from any directory entry
 * (set by walk_tree). For /CONVERT, a pre-pass marks orphan chain
 * successors so the main pass can identify chain heads. The main
 * pass walks each FAT sector, locates orphan heads (or just any
 * orphan in free-mode), and applies the chosen repair. */
static int phase4_lost(vol_t *fs)
{
    DWORD lost_n   = 0ul;
    DWORD chain_n  = 0ul;
    DWORD seq      = 1ul;
    DWORD sec_idx;
    UINT  shift, n_per;
    DWORD bad_marker;
    LBA_t fat1, fat2;
    int   converting;

    prt_str("Phase 4: lost clusters\r\n");

    converting = fix_enabled() && fix_convert_enabled();

#if CHKDSK_FAT12
    /* FAT12 has 1.5-byte entries that may straddle a sector boundary,
     * so the per-sector scan loop used for FAT16/32 doesn't apply.
     * Walk cluster-by-cluster using chain_get_entry for reads (already
     * straddle-aware) and fix_fat_set for the per-cluster writes
     * (likewise). Slow on large volumes -- but FAT12 is bounded to
     * ~4085 clusters, so this is fine in practice. */
    if (fs->fs_type == FS_FAT12) {
        DWORD cc;

        /* Pre-pass for /CONVERT: mark successors so we can identify
         * chain heads in the main pass. */
        if (converting) {
            for (cc = 2ul; cc < fs->n_fatent; cc++) {
                DWORD v;
                if (bitmap_get((u32)cc)) continue;
                v = chain_get_entry(fs, cc);
                if (v == CHAIN_READ_ERROR) {
                    prt_str("  ERROR: FAT read failed\r\n");
                    return -1;
                }
                if (v < 2ul || v >= fs->n_fatent) continue;
                if (chain_is_bad(fs, v)) continue;
                bitmap_set((u32)v);
            }
        }

        for (cc = 2ul; cc < fs->n_fatent; cc++) {
            DWORD v;
            if (bitmap_get((u32)cc)) continue;
            v = chain_get_entry(fs, cc);
            if (v == CHAIN_READ_ERROR) {
                prt_str("  ERROR: FAT read failed\r\n");
                return -1;
            }
            if (v == 0ul || chain_is_bad(fs, v)) continue;

            if (converting) {
                DWORD chain_len = phase4_walk_chain(fs, cc);
                LBA_t e_sect;
                WORD  e_off;
                BYTE  name[11];
                if (chain_len == 0ul) {
                    /* defensive */
                } else if (find_free_root_slot(fs, &e_sect, &e_off)) {
                    DWORD cluster_bytes = (DWORD)fs->csize << 9;
                    format_chk_name(name, seq);
                    if (write_chk_entry(e_sect, e_off, name, cc,
                                        chain_len * cluster_bytes)) {
                        chain_n++;
                        seq++;
                        lost_n += chain_len;
                    } else {
                        prt_str("  WARN: write FILE entry failed\r\n");
                    }
                } else {
                    prt_str("  WARN: root dir full; chain at cluster ");
                    prt_dec((unsigned long)cc);
                    prt_str(" not converted\r\n");
                }
                chain_invalidate();
            } else {
                if (fix_enabled()) {
                    if (!fix_fat_set(fs, cc, 0ul)) {
                        prt_str("  ERROR: FAT free failed\r\n");
                        return -1;
                    }
                }
                lost_n++;
            }
        }
        goto phase4_report;
    }
#endif

#if CHKDSK_FAT32
    if (fs->fs_type == FS_FAT32) { shift = 2u; n_per = 128u; bad_marker = 0x0FFFFFF7ul; }
    else
#endif
#if CHKDSK_FAT16
    if (fs->fs_type == FS_FAT16) { shift = 1u; n_per = 256u; bad_marker = 0xFFF7ul; }
    else
#endif
    { prt_str("  (skipped)\r\n"); return 0; }

    fat1 = fs->fatbase;
    fat2 = fat1 + (LBA_t)fs->fsize;

    if (converting) {
        if (phase4_premark(fs, shift, n_per, bad_marker) < 0) {
            prt_str("  ERROR: pre-pass FAT read failed\r\n");
            return -1;
        }
    }

    if (fix_verbose_enabled()) prt_str("  ");
    for (sec_idx = 0ul; sec_idx < fs->fsize; sec_idx++) {
        DWORD start_c = (shift == 2u) ? (sec_idx << 7) : (sec_idx << 8);
        if (fix_verbose_enabled() && (sec_idx & 0x0Ful) == 0ul) prt_chr('.');
        UINT  cc_in;
        int   dirty = 0;

        if (start_c >= fs->n_fatent) break;

        if (disk_read(0u, g_sect_a, fat1 + (LBA_t)sec_idx, 1u) != RES_OK) {
            prt_str("  ERROR: FAT read failed\r\n");
            return -1;
        }
        chain_invalidate();

        for (cc_in = 0u; cc_in < n_per; cc_in++) {
            DWORD cc  = start_c + (DWORD)cc_in;
            UINT  pos = cc_in << shift;
            DWORD v;

            if (cc < 2ul || cc >= fs->n_fatent) continue;
            if (bitmap_get((u32)cc)) continue;
            v = read_fat_entry_in_buf(pos, shift);
            if (v == 0ul || v == bad_marker) continue;

            if (converting) {
                /* cc is a chain head. Walk it (clobbers g_sect_a),
                 * write FILE####.CHK entry, restore FAT sector. */
                DWORD chain_len = phase4_walk_chain(fs, cc);
                LBA_t e_sect;
                WORD  e_off;
                BYTE  name[11];
                if (chain_len == 0ul) {
                    /* re-read the FAT sector and continue */
                } else if (find_free_root_slot(fs, &e_sect, &e_off)) {
                    DWORD cluster_bytes = (DWORD)fs->csize << 9;
                    format_chk_name(name, seq);
                    if (write_chk_entry(e_sect, e_off, name, cc,
                                        chain_len * cluster_bytes)) {
                        chain_n++;
                        seq++;
                        lost_n += chain_len;
                    } else {
                        prt_str("  WARN: write FILE entry failed\r\n");
                    }
                } else {
                    prt_str("  WARN: root dir full; chain at cluster ");
                    prt_dec((unsigned long)cc);
                    prt_str(" not converted\r\n");
                }
                /* Restore FAT 1 sector for the outer loop. */
                if (disk_read(0u, g_sect_a, fat1 + (LBA_t)sec_idx, 1u) != RES_OK) {
                    prt_str("  ERROR: FAT re-read failed\r\n");
                    return -1;
                }
                chain_invalidate();
            } else {
                /* Free mode: zero entry in the loaded buffer. */
                if (fix_enabled()) {
                    g_sect_a[pos]      = 0u;
                    g_sect_a[pos + 1u] = 0u;
                    if (shift == 2u) {
                        g_sect_a[pos + 2u]  = 0u;
                        g_sect_a[pos + 3u] &= 0xF0u;
                    }
                    dirty = 1;
                }
                lost_n++;
            }
        }

        if (dirty) {
            if (!fix_write(fat1 + (LBA_t)sec_idx, g_sect_a, 1u)) return -1;
            if (fs->n_fats == 2u
                && !fix_write(fat2 + (LBA_t)sec_idx, g_sect_a, 1u)) return -1;
        }
    }
    if (fix_verbose_enabled()) prt_nl();

phase4_report:
    if (lost_n == 0ul) {
        prt_str("  No issues\r\n");
    } else if (converting) {
        prt_str("  Converted ");
        prt_dec((unsigned long)chain_n);
        prt_str(" chain(s) (");
        prt_dec((unsigned long)lost_n);
        prt_str(" cluster(s)) to FILE####.CHK\r\n");
        fix_count_found();
        fix_count_applied();
    } else if (fix_enabled()) {
        prt_str("  Freed ");
        prt_dec((unsigned long)lost_n);
        prt_str(" lost cluster(s)\r\n");
        fix_count_found();
        fix_count_applied();
    } else {
        prt_str("  Found ");
        prt_dec((unsigned long)lost_n);
        prt_str(" lost cluster(s); /F to free, /F /C to convert\r\n");
        fix_count_found();
    }
    return (lost_n == 0ul) ? 0 : 1;
}

int scan_run(vol_t *fs)
{
    scan_totals_t t;
    int           lost_rc;

    prt_str("Phase 3: directory and chain walk\r\n");

    if (!bitmap_init((u32)fs->n_fatent)) {
        prt_str("  error: cannot allocate cluster bitmap (");
        prt_dec((unsigned long)fs->n_fatent);
        prt_str(" entries)\r\n");
        return -1;
    }
    bitmap_set(0u);
    bitmap_set(1u);

    if (fix_verbose_enabled()) prt_str("  ");
    if (walk_tree(fs, &t) < 0) {
        prt_str("  error: walk aborted\r\n");
        bitmap_release();
        return -1;
    }
    if (fix_verbose_enabled()) prt_nl();

    prt_str("  Totals: entries=");
    prt_dec((unsigned long)t.entries);
    prt_str(" dirs=");
    prt_dec((unsigned long)t.dirs);
    emit_count(" flagged=",     t.flagged);
    emit_count(" cycles=",      t.cycles);
    emit_count(" crosslinks=",  t.cross_links);
    emit_count(" broken=",      t.broken_chains);
    emit_count(" truncated=",   t.truncated);
    emit_count(" excess=",      t.excess);
    emit_count(" depth-cap=",   t.depth_capped);
    prt_nl();

    lost_rc = phase4_lost(fs);

    bitmap_release();
    if (lost_rc < 0) return -1;
    return (int)(t.flagged + t.cycles + t.cross_links + t.broken_chains
               + t.truncated + t.excess) + lost_rc;
}
