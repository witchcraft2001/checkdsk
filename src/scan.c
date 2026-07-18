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
 * -- the per-frame state (dirwalk position, parent SFN, etc.) lives
 * in BSS.
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
 * LFN slots are passed over as opaque entries -- no sequence/checksum
 * cross-check against the following SFN (removed for memory budget,
 * see the comment above scan_frame_t). Name repairs that touch an
 * SFN's raw bytes (see fix_delete_preceding_lfn) still have to respect
 * any LFN run in front of it, even without decoding it.
 *
 * Output policy: only flagged entries (and cycle / depth-cap warnings)
 * are echoed, prefixed by their full path. Clean entries are silent;
 * the trailing Totals line gives the population counts.
 */

#include <sprinter.h>
#include <string.h>
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
#include "diskio_batch.h"
#include "diskio_dss.h"

/* SCAN_MAX_DEPTH bounds the directory recursion in Phase 3.
 * Each frame is ~35 bytes of static state in g_frames. We size the
 * runtime to keep the stack at ~255 bytes so the BIOS DRV_READ helper
 * (which pushes a non-trivial amount of state internally during
 * multi-sector reads) has headroom -- earlier iterations at depth=7
 * with full LFN frame state were observed clobbering g_fix_verbose_*
 * mid-walk. */
#define SCAN_MAX_DEPTH  16u

#define ATTR_VOLID    0x08u
#define ATTR_DIR      0x10u
#define ATTR_LFN      0x0Fu
#define ATTR_DIR_BYTE 0x10u
#define LOSTCHN_SFN   "LOSTCHN    "    /* 11 raw SFN bytes */

/* LFN sequence validation was removed to free per-frame state and
 * step()-stack pressure on the Sprinter target -- the field reports
 * mid-walk corruption (sparse Phase 3 progress dots, depth-limit
 * spam) traced to BIOS-call stack overflow into _DATA. ATTR_LFN
 * entries are now passed over as opaque slots; a future build can
 * reintroduce LFN parsing as a separate utility. */
typedef struct {
    dirwalk_t walker;
    BYTE      name[11];      /* parent SFN, valid for depth >= 1 */
    DWORD     start_cluster; /* first cluster of this directory (0 for FAT12/16 root) */
} scan_frame_t;

/* NOTE: most uninitialized statics in this file are given an explicit
 * `= {0}` / `= 0`. The Sprinter SDK's crt0.s only zeroes _BSS, but
 * SDCC z80 places uninitialized statics in _DATA -- so without an
 * explicit initializer they retain whatever bytes were at their
 * load address at boot time, which corrupts the LOSTCHN allocation
 * cursor and the captured fat date/time.
 *
 * g_frames is the exception: every frame is fully initialised by
 * dirwalk_open_root (frame 0) or dirwalk_open_chain (descend) before
 * any field is read, so leaving it uninit halves its memory cost
 * (no _INITIALIZER copy alongside _INITIALIZED at depth=16 -> save
 * 595 B). frame[0].name is never consumed (only depths >=1 print). */
static scan_frame_t g_frames[SCAN_MAX_DEPTH + 1u];

/* Aggregate counters for the whole walk. Held statically (rather than
 * on scan_run's stack) so step()'s deep call chain doesn't compete
 * with 36 B of locals for the 350-ish-byte stack budget. */
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

/* Uninit at boot is fine: walk_tree zeroes every field before use. */
static scan_totals_t g_totals;

/* Chain-walk findings, flag bits returned by walk_chain. */
#define CW_CYCLE      0x01u   /* first cluster already set in bitmap */
#define CW_CROSS      0x02u   /* mid-chain cluster already set */
#define CW_BROKEN     0x04u   /* invalid link encountered */
#define CW_BAD        0x08u   /* FAT entry == BAD mid-chain */
#define CW_IO_ERR     0x10u   /* disk_read failure */
#define CW_TRUNCATED  0x20u   /* file size implies more clusters than chain */
#define CW_EXCESS     0x40u   /* file size implies fewer clusters than chain */

/* End-of-chain marker for repairs. fix_fat_set packs the value into
 * 12/16/28 bits per FAT type, so one constant works on every layout. */
#define CHAIN_SET_EOC  0x0FFFFFFFul

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

/* Compact diagnostic helpers -- factor out the repeated "  WARN: " /
 * "  ERROR: " prefix and "\r\n" suffix to dedupe ~14 sites across
 * scan.c. Each call site goes from three prt_str()s to one helper
 * call with just the unique tail string. */
static void warn_str(const char *s)
{
    fix_verbose_flush();
    prt_str("  WARN: ");
    prt_str(s);
    prt_nl();
}
static void err_str(const char *s)
{
    fix_verbose_flush();
    prt_str("  ERROR: ");
    prt_str(s);
    prt_nl();
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

/* Renaming a corrupt SFN (see the name-sanitize pack in step()) risks
 * producing two identical live names in the same directory -- specs.md
 * flagged this exact risk and deferred auto-fixing name characters to
 * a future /N flag for it. Instead of adding a flag, check for the
 * collision up front: open a fresh walk of the directory that owns
 * (skip_sect, skip_off) -- the entry about to be renamed -- and look
 * for any other live, non-LFN entry already named `candidate`.
 * `is_root` selects dirwalk_open_root (FAT12/16 fixed root or FAT32
 * root, either way frame 0) vs dirwalk_open_chain(dir_cluster).
 * Returns 1 on collision (including if the re-scan itself hits an I/O
 * error -- abstain rather than risk a silent duplicate name), 0 if
 * `candidate` is free.
 *
 * Side effect: clobbers g_sect_a like any dirwalk. Caller must mark
 * its own walker buffer_dirty afterward. */
static int name_collides(vol_t *fs, DWORD dir_cluster, int is_root,
                          LBA_t skip_sect, WORD skip_off,
                          const BYTE *candidate)
{
    dirwalk_t tw;
    BYTE     *e;
    int       rc;

    if (is_root) dirwalk_open_root(&tw, fs);
    else         dirwalk_open_chain(&tw, fs, dir_cluster);

    for (;;) {
        LBA_t sect; WORD off;
        rc = dirwalk_next(&tw, &e);
        if (rc <= 0) break;
        dirwalk_last_entry_location(&tw, &sect, &off);
        if (sect == skip_sect && off == skip_off) continue;
        if (e[0] == 0xE5u) continue;
        if ((e[11] & 0x3Fu) == ATTR_LFN) continue;
        if (memcmp(e, candidate, 11) == 0) return 1;
    }
    return (rc < 0) ? 1 : 0;
}

/* Walk the FAT chain at `start`, marking visited clusters in the bitmap.
 * Stops at EOC, BAD, invalid link, I/O error, or test_and_set hit.
 * Returns CW_* flag bits; chain length is written to *len_out (0 means
 * "we never marked anything", e.g. cycle on the first cluster), and the
 * last cluster this walk claimed to *last_out -- the EOC-termination
 * repair writes there when the chain ended in garbage.
 * g_sect_a ends up holding a FAT sector -- callers using a dirwalk must
 * mark its buffer dirty after returning. */
static UINT walk_chain(vol_t *fs, DWORD start, DWORD *len_out, DWORD *last_out)
{
    DWORD c = start;
    DWORD next;
    DWORD len = 0ul;
    UINT  flags = 0u;

    *last_out = start;
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
    *len_out  = len;
    *last_out = c;
    return flags;
}

static void print_cw_tags(UINT cflags)
{
    /* Table-driven so the bit-test+print pattern compiles to one
     * loop instead of N separate if-prt_str pairs. CYCLE shadows
     * CROSS and BAD shadows BROKEN -- masks are arranged so that
     * once CYCLE/BAD are emitted we mask their alternate. */
    static const struct { UINT mask; const char *s; } tbl[] = {
        { CW_CYCLE,     " cycle" },
        { CW_CROSS,     " cross-link" },
        { CW_BAD,       " bad-cluster" },
        { CW_BROKEN,    " broken" },
        { CW_IO_ERR,    " io-err" },
        { CW_TRUNCATED, " truncated" },
        { CW_EXCESS,    " excess" }
    };
    UINT i;
    if (cflags & CW_CYCLE) cflags &= (UINT)~CW_CROSS;
    if (cflags & CW_BAD)   cflags &= (UINT)~CW_BROKEN;
    for (i = 0u; i < sizeof(tbl)/sizeof(tbl[0]); i++) {
        if (cflags & tbl[i].mask) prt_str(tbl[i].s);
    }
}

/* Emit "  /A/B/NAME.EXT" -- the full path of a flagged entry. */
static void print_flagged(BYTE depth, const BYTE *e)
{
    fix_verbose_flush();
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
 * We snapshot the 32-byte entry into ent[] before doing any
 * I/O that touches g_sect_a, and operate on the snapshot afterwards. */
static int step(vol_t *fs, BYTE *depth)
{
    scan_totals_t *t = &g_totals;
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
    DWORD         chain_last;
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

    /* LFN sequence validation removed -- ATTR_LFN slots are counted
     * but not parsed. See scan_frame_t comment. */
    if ((ent[11] & 0x3Fu) == ATTR_LFN) {
        t->entries++;
        return 1;
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
                    fix_verbose_flush();
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
                            warn_str("dot-clust");
                        }
                    }
                }
            }
            return 1;
        }
    }

    t->entries++;
    if ((t->entries & 0x7Ful) == 0ul) {
        fix_verbose_tick();
        if (fix_poll_abort()) {
            warn_str("aborted");
            return -1;
        }
    }
    dflags    = dirent_validate(fs, ent);
    cflags    = 0u;
    chain_len = 0ul;

    clust   = entry_first_cluster(ent);
    is_file = ((ent[11] & 0x3Fu) != ATTR_LFN) && !(ent[11] & ATTR_DIR)
            && !(ent[11] & ATTR_VOLID) && (ent[0] != 0xE5u);
    size    = ld_size(ent);

    if (ent[0] != 0xE5u && clust >= 2ul && clust < fs->n_fatent) {
        cflags |= walk_chain(fs, clust, &chain_len, &chain_last);
        dirwalk_buffer_dirty(&g_frames[*depth].walker);

        if (is_file && size != 0ul
            && (cflags & (CW_BROKEN | CW_BAD | CW_IO_ERR | CW_CYCLE | CW_CROSS)) == 0u) {
            /* expected = ceil(size / cluster_bytes), where
             * cluster_bytes = csize << 9 = 1 << (csize_shift+9).
             * Use shift+mask to dodge _divulong/_modulong. */
            BYTE  cl_sh = (BYTE)(fs->csize_shift + 9u);
            DWORD mask  = ((DWORD)1ul << cl_sh) - 1ul;
            DWORD expected = (size + mask) >> cl_sh;
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
        /* Wider than DE_ANY_ERROR on purpose: DE_NAME_LOWERCASE and
         * DE_NAME_LEAD_SPACE are cosmetic (see dirent.h) and must stay
         * out of the directory-descend gate, but they ARE now repaired
         * below, so they must show up as found/flagged like any other
         * repaired issue -- otherwise a dry run would report "clean"
         * on an entry /F is about to touch. */
        UINT de_err = dflags & (DE_ANY_ERROR | DE_NAME_LOWERCASE
                                             | DE_NAME_LEAD_SPACE);
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
                warn_str("dir-corrupt");
            }
        }

        /* MVP dirent repair pack (specs.md:278-282). Each safe-to-apply
         * dflag triggers one fix_dir_patch. Skipped on ATTR_LFN slots
         * (no SFN structure) and on . / .. entries (filtered out at
         * the top of step). Inlined as an if-cascade -- table-driven
         * version added ~20 B vs hand-rolled because the table itself
         * (2 entries * 3 B + per-iter loop overhead) outweighed the
         * common-prefix savings at three masks. */
        if (fix_enabled() && (dflags & (DE_ATTR_RESERVED | DE_NTRES_RSV
                                      | DE_FAT16_HI_CLUST))
            && (ent[11] & 0x3Fu) != ATTR_LFN) {
            LBA_t sect; WORD off;
            int   ok = 1;
            dirwalk_last_entry_location(&frame->walker, &sect, &off);
            if (ok && (dflags & DE_ATTR_RESERVED))
                ok = fix_dir_patch(sect, off, FIX_DPATCH_ATTR_MASK, 0ul);
            if (ok && (dflags & DE_NTRES_RSV))
                ok = fix_dir_patch(sect, off, FIX_DPATCH_NTRES_FIX, 0ul);
            if (ok && (dflags & DE_FAT16_HI_CLUST))
                ok = fix_dir_patch(sect, off, FIX_DPATCH_HI_CLUST_ZERO, 0ul);
            if (!ok) warn_str("dpatch");
            dirwalk_buffer_dirty(&frame->walker);
        }

        /* Name-sanitize pack: DE_NAME_BAD_CHAR / DE_NAME_LOWERCASE /
         * DE_NAME_LEAD_SPACE all rewrite raw SFN bytes, which an LFN
         * checksum may be computed over -- specs.md originally deferred
         * this to a future /N flag over exactly that risk plus the
         * same-directory name-collision risk below. We apply it under
         * plain /F instead, gated on two checks that abstain (leave the
         * entry flagged, untouched) rather than guess:
         *   1. fix_delete_preceding_lfn -- drops any LFN run in front
         *      of this entry so its stale checksum doesn't linger;
         *      abstains if the run isn't fully visible in this sector.
         *   2. name_collides -- the sanitized name must not already
         *      name a live sibling in this directory.
         * Post-MVP: recompute the LFN checksum instead of dropping the
         * run (preserves the long name; see fix_delete_preceding_lfn). */
        if (fix_enabled() && (dflags & (DE_NAME_BAD_CHAR | DE_NAME_LOWERCASE
                                      | DE_NAME_LEAD_SPACE))
            && (ent[11] & 0x3Fu) != ATTR_LFN) {
            LBA_t sect; WORD off;
            LBA_t dir_start;
            BYTE  new_name[11];
            dirwalk_last_entry_location(&frame->walker, &sect, &off);
            dirent_sanitize_name(ent, new_name);
            dir_start = (*depth == 0u)
                        ? ((fs->fs_type == FS_FAT32)
                           ? chain_cluster_to_lba(fs, (DWORD)fs->dirbase)
                           : fs->dirbase)
                        : chain_cluster_to_lba(fs, frame->start_cluster);
            if (name_collides(fs, frame->start_cluster, (*depth == 0u),
                               sect, off, new_name)) {
                fix_verbose_flush();
                prt_str("  WARN: name-fix skipped, sanitized name '");
                print_sfn(new_name);
                prt_str("' collides with an existing entry\r\n");
                fix_count_incomplete();
            } else if (!fix_delete_preceding_lfn(sect, off, dir_start)) {
                warn_str("name-fix skipped, long-name entry spans a sector");
                fix_count_incomplete();
            } else if (!fix_dir_name_set(sect, off, new_name)) {
                warn_str("name-fix");
            }
            dirwalk_buffer_dirty(&frame->walker);
        }
    }

    if (cflags & CW_CYCLE) {
        t->cycles++;
        fix_count_found();
        /* Cycle on the head cluster means another dirent already
         * claims the same first cluster; the duplicate is dead weight
         * (and on a /F /C run from a buggy older build, points back
         * into LOSTCHN itself). Delete the duplicate dirent on /F so
         * a follow-up sweep reports a clean tree. Skip for ATTR_DIR:
         * removing one of two dir entries that share a cluster is
         * fine, but we limit this repair to regular files to avoid
         * surprises on legitimate aliasing patterns. */
        if (is_file && fix_enabled()) {
            LBA_t sect; WORD off;
            dirwalk_last_entry_location(&frame->walker, &sect, &off);
            if (fix_dir_patch(sect, off, FIX_DPATCH_DELETE, 0ul)) {
                dirwalk_buffer_dirty(&frame->walker);
            } else {
                warn_str("cycle-del");
            }
        }
    }
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
        /* chain_capacity = chain_len * cluster_bytes; cluster_bytes is
         * a power of two so this is a shift, no _mullong. */
        DWORD chain_capacity = chain_len << (fs->csize_shift + 9u);
        if (size > chain_capacity) {
            LBA_t sect; WORD off;
            dirwalk_last_entry_location(&frame->walker, &sect, &off);
            if (fix_dir_patch(sect, off, FIX_DPATCH_SIZE, chain_capacity)) {
                if (fix_enabled()) dirwalk_buffer_dirty(&frame->walker);
            } else {
                warn_str("chain-size");
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
        BYTE  cl_sh = (BYTE)(fs->csize_shift + 9u);
        DWORD mask  = ((DWORD)1ul << cl_sh) - 1ul;
        DWORD expected = (size + mask) >> cl_sh;
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
        if (ok) {
            fix_count_applied();
        } else {
            warn_str("chain-trunc");
        }
        chain_invalidate();
        dirwalk_buffer_dirty(&frame->walker);
    }

    /* Terminate the chain at the last cluster this entry actually owns
     * when the walk stopped on a garbage link (BROKEN -- also covers
     * BAD, which always sets BROKEN) or on territory claimed by
     * another chain (CROSS). Writing EOC there clears the invalid FAT
     * value that phase 2 would otherwise keep reporting forever, and
     * detaches the cross-link at the fork (first claimant keeps the
     * shared tail). The size shrink above already matches the kept
     * chain, so one run leaves the entry fully consistent. */
    if (fix_enabled() && chain_len != 0ul
        && (cflags & (CW_BROKEN | CW_CROSS)) != 0u) {
        if (fix_fat_set(fs, chain_last, CHAIN_SET_EOC)) {
            fix_count_applied();
        } else {
            warn_str("chain-eoc");
        }
        chain_invalidate();
        dirwalk_buffer_dirty(&frame->walker);
    }

    if (!is_descendable_dir(ent, dflags)) return 1;
    if (clust < 2ul || clust >= fs->n_fatent) return 1;
    if (cflags & (CW_CYCLE | CW_IO_ERR)) return 1;
    if (has_dot != 1) return 1;

    if (*depth >= SCAN_MAX_DEPTH) {
        fix_verbose_flush();
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

static int walk_tree(vol_t *fs)
{
    BYTE depth;
    int  rc;

    g_totals.entries       = 0ul;
    g_totals.dirs          = 0ul;
    g_totals.flagged       = 0ul;
    g_totals.cycles        = 0ul;
    g_totals.cross_links   = 0ul;
    g_totals.broken_chains = 0ul;
    g_totals.truncated     = 0ul;
    g_totals.excess        = 0ul;
    g_totals.depth_capped  = 0ul;

    depth = 0u;
    /* Root frame: start_cluster only used by dot validation, which is
     * gated on depth >= 1. Set to fs->dirbase on FAT32 (the actual
     * root cluster) and 0 on FAT12/16 for self-consistency, even
     * though no dots will be checked here. */
    g_frames[0].start_cluster =
        (fs->fs_type == FS_FAT32) ? (DWORD)fs->dirbase : 0ul;
    dirwalk_open_root(&g_frames[0].walker, fs);

    for (;;) {
        rc = step(fs, &depth);
        if (rc < 0) return -1;
        if (depth == 0xFFu) break;
    }
    return 0;
}

/* Read a FAT entry value out of `buf` at byte offset `pos`.
 * shift = 1 (FAT16, 2 bytes) or 2 (FAT32, 4 bytes; high nibble masked).
 * `buf` may be g_sect_a or a batch-page sector slice -- caller's choice. */
static DWORD read_fat_entry_in_buf(const u8 *buf, UINT pos, UINT shift)
{
    DWORD v = (DWORD)buf[pos] | ((DWORD)buf[pos + 1u] << 8);
    if (shift == 2u) {
        v |= ((DWORD)buf[pos + 2u] << 16)
           | ((DWORD)(buf[pos + 3u] & 0x0Fu) << 24);
    }
    return v;
}

/* Walk an orphan chain starting at cluster c, claiming each cluster in
 * the bitmap (test_and_set stops cycles and keeps converted chains
 * disjoint). Stops at EOC, BAD-marker link, out-of-range link, an
 * already-claimed cluster, or read error.
 *
 * Under /F the walked chain is then TERMINATED: unless it already
 * ended in a clean EOC, the last claimed cluster gets an EOC written.
 * This is what makes /CONVERT safe -- it breaks self-loops and cycles,
 * cuts garbage tails (phase 2 "invalid" values), and detaches links
 * into territory owned by other chains, so every FILE####.CHK ends up
 * a linear, self-contained file. Without it, a converted chain keeps
 * its rotten FAT links and the next scan reports cycle/cross/excess
 * on the very files a repair run just created.
 *
 * Returns the claimed cluster count (0 = head was already claimed). */
static DWORD phase4_walk_chain(vol_t *fs, DWORD c)
{
    DWORD len  = 0ul;
    DWORD last = 0ul;
    u8    clean_end = 0u;

    while (c >= 2ul && c < fs->n_fatent) {
        DWORD nx;
        if (bitmap_test_and_set((u32)c)) break;
        len++;
        last = c;
        nx = chain_get_entry(fs, c);
        if (nx == CHAIN_READ_ERROR) break;
        if (chain_is_eoc(fs, nx)) { clean_end = 1u; break; }
        if (chain_is_bad(fs, nx)) break;
        c = nx;
    }
    if (len != 0ul && !clean_end) {
        if (!fix_fat_set(fs, last, CHAIN_SET_EOC)) warn_str("chain-eoc");
    }
    return len;
}

/* Format "FILE####" + "CHK" into 11 raw SFN bytes. seq up to 9999.
 * Truncate to 16 bits so the digit extraction stays on _divuint (small)
 * and _divulong is not pulled into _HOME for the whole binary. */
static void format_chk_name(BYTE *out11, DWORD seq)
{
    WORD s = (WORD)seq;
    UINT i;
    out11[0] = 'F'; out11[1] = 'I'; out11[2] = 'L'; out11[3] = 'E';
    for (i = 0u; i < 4u; i++) {
        out11[7u - i] = (BYTE)('0' + (BYTE)(s % 10u));
        s = (WORD)(s / 10u);
    }
    out11[8] = 'C'; out11[9] = 'H'; out11[10] = 'K';
}

/* Match flavour for find_root_slot:
 *   0 -- free slot (byte 0 == 0x00 end-of-dir or 0xE5 deleted)
 *   1 -- existing FILE####.CHK SFN entry (FAT12/16 fallback for
 *        the LOSTCHN open path; overwriting it leaves the old
 *        chain dangling, to be reconverted on the next run).
 *   2 -- existing LOSTCHN dir entry (used by phase4_lostchn_open
 *        to reuse a LOSTCHN created by a prior /F /C run). */
#define ROOT_MATCH_FREE      0
#define ROOT_MATCH_CHK       1
#define ROOT_MATCH_LOSTCHN   2

/* Find the first slot in the root matching `mode`. Returns 1 with
 * sect/off filled, 0 on no match / I/O error. Clobbers g_sect_a.
 * The CHK-mode is FAT12/16 only (FAT32 root auto-extends, so the
 * mode is meaningless there). */
static int find_root_slot(vol_t *fs, LBA_t *out_sect, WORD *out_off, int mode)
{
    /* Reads root-dir sectors into g_sect_a, which is also chain.c's FAT
     * cache. Invalidate so chain.c re-reads on its next call. */
    chain_invalidate();
#if CHKDSK_FAT12 || CHKDSK_FAT16
    if (fs->fs_type != FS_FAT32) {
        DWORD ent_idx;
        LBA_t cur_sec = 0xFFFFFFFFul;
        for (ent_idx = 0ul; ent_idx < (DWORD)fs->n_rootdir; ent_idx++) {
            LBA_t sec = fs->dirbase + (LBA_t)(ent_idx >> 4);
            UINT  off = (UINT)((ent_idx & 0x0Fu) << 5);
            BYTE *e;
            if (sec != cur_sec) {
                if (disk_read(0u, g_sect_a, sec, 1u) != RES_OK) return 0;
                cur_sec = sec;
            }
            e = &g_sect_a[off];
            if (mode == ROOT_MATCH_CHK) {
                if (e[0] == 0x00u) return 0;
                if (e[11] == 0x0Fu) continue;
                if (e[0] != 'F' || e[1] != 'I' || e[2] != 'L' || e[3] != 'E') continue;
                if (e[8] != 'C' || e[9] != 'H' || e[10] != 'K') continue;
            } else if (mode == ROOT_MATCH_LOSTCHN) {
                if (e[0] == 0x00u) return 0;
                if ((e[11] & 0x3Fu) == ATTR_LFN) continue;
                if (!(e[11] & ATTR_DIR_BYTE)) continue;
                if (memcmp(e, LOSTCHN_SFN, 11u) != 0) continue;
            } else {
                if (e[0] != 0x00u && e[0] != 0xE5u) continue;
            }
            *out_sect = sec; *out_off = (WORD)off;
            return 1;
        }
        return 0;
    }
#endif
#if CHKDSK_FAT32
    if (mode == ROOT_MATCH_CHK) return 0;
    {
        DWORD c    = (DWORD)fs->dirbase;
        UINT  scnt = fs->csize;
        for (;;) {
            UINT i;
            for (i = 0u; i < scnt; i++) {
                LBA_t sec = chain_cluster_to_lba(fs, c) + (LBA_t)i;
                UINT off;
                if (disk_read(0u, g_sect_a, sec, 1u) != RES_OK) return 0;
                for (off = 0u; off < 512u; off += 32u) {
                    BYTE *e = &g_sect_a[off];
                    int  match = 0;
                    if (mode == ROOT_MATCH_LOSTCHN) {
                        if (e[0] == 0x00u) return 0;
                        if (e[0] == 0xE5u) continue;
                        if ((e[11] & 0x3Fu) == ATTR_LFN) continue;
                        if (!(e[11] & ATTR_DIR_BYTE)) continue;
                        if (memcmp(e, LOSTCHN_SFN, 11u) == 0) match = 1;
                    } else {
                        match = (e[0] == 0x00u || e[0] == 0xE5u);
                    }
                    if (match) {
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

#define find_free_root_slot(fs, s, o)  find_root_slot((fs), (s), (o), ROOT_MATCH_FREE)

/* Current wall-clock timestamp for newly-created entries, captured
 * once per phase 4 sweep from DSS. Encoded in FAT 16-bit fields:
 *   date = (year-1980)<<9 | month<<5 | day
 *   time = hour<<11 | minute<<5 | second/2 */
static u16 g_fat_date = 0u;
static u16 g_fat_time = 0u;

static void phase4_capture_now(void)
{
    dss_date_t d;
    dss_time_t t;
    BYTE *p;

    dss_getdate(&d);
    dss_gettime(&t);

    /* Pack byte-wise to avoid 16-bit shifts on Z80. */
    p = (BYTE *)&g_fat_date;
    p[0] = (BYTE)((d.month << 5) | d.day);
    p[1] = (BYTE)(((u8)(d.year - 1980u) << 1) | (d.month >> 3));
    p = (BYTE *)&g_fat_time;
    p[0] = (BYTE)((t.minute << 5) | (t.second >> 1));
    p[1] = (BYTE)((t.hour << 3) | (t.minute >> 3));
}

/* Stamp creation/access/last-write date+time bytes into a 32-byte
 * SFN entry. Caller has already zeroed bytes 12..31 (or written
 * them with values that we are about to overwrite). */
static void stamp_entry_dt(BYTE *e)
{
    BYTE *fb;
    fb = (BYTE *)&g_fat_time;
    e[14] = fb[0]; e[15] = fb[1];   /* creation time */
    e[22] = fb[0]; e[23] = fb[1];   /* last write time */
    fb = (BYTE *)&g_fat_date;
    e[16] = fb[0]; e[17] = fb[1];   /* creation date */
    e[18] = fb[0]; e[19] = fb[1];   /* last access date */
    e[24] = fb[0]; e[25] = fb[1];   /* last write date */
}

/* Write a 32-byte SFN entry at (sect, off) describing a FILE####.CHK
 * file: archive attr, current timestamp, first cluster + size set
 * from the orphan chain. Re-reads sector, patches in place, writes
 * back. */
static int write_chk_entry(LBA_t sect, WORD off, const BYTE *name11,
                           DWORD cluster, DWORD size)
{
    BYTE *e;
    BYTE *cb;

    if (!fix_enabled()) return 1;
    /* See find_root_slot/fix_fat_set: g_sect_a is also chain.c's cache. */
    chain_invalidate();
    if (disk_read(0u, g_sect_a, sect, 1u) != RES_OK) return 0;
    e = &g_sect_a[off];
    memcpy(e, name11, 11u);
    e[11] = 0x20u;                              /* ATTR_ARCHIVE */
    memset(e + 12u, 0, 16u);                    /* reserved + time/date */
    stamp_entry_dt(e);
    cb = (BYTE *)&cluster;
    e[20] = cb[2]; e[21] = cb[3];               /* FstClusHI */
    e[26] = cb[0]; e[27] = cb[1];               /* FstClusLO */
    memcpy(e + 28u, &size, 4u);                 /* file size */
    return fix_write(sect, g_sect_a, 1u);
}

/* ---------------------------------------------------------------- */
/* LOSTCHN/ subdirectory (FreeDOS chkdsk convention): destination
 * for FILE####.CHK entries during /CONVERT on FAT12/16/32 volumes.
 * Lazy-created on first conversion.
 *
 * Reuse policy: phase4_lostchn_open first scans the root for an
 * existing LOSTCHN dir entry (left over by an earlier /F /C run).
 * If found, the cursor is positioned at the first free 32-byte
 * slot inside the existing LOSTCHN's cluster chain -- avoiding
 * both the cost of init_dir_cluster on a fresh cluster and the
 * pile-up of duplicate LOSTCHN entries in root that an earlier
 * "always create new" policy was producing across runs.
 *
 * If the root has no free slot when we want to place the LOSTCHN
 * entry (typical when a prior /F /C without LOSTCHN already filled
 * root with FILE####.CHK), find_free_root_slot falls back to
 * overwriting one such old FILE####.CHK entry: the displaced
 * entry's chain becomes orphan and is reconverted on the next
 * chkdsk run, while LOSTCHN gets a place to live. Any other
 * failure (no free cluster, write error, or root genuinely full of
 * non-CHK entries) is graceful -- one WARN, then CHK entries fall
 * back to root for the rest of the sweep.
 *
 * Coherence with the batched FAT scan: fix_fat_set bypasses the
 * WIN3 batch page; the main loop sees stale 0 bytes for any cluster
 * we just allocated mid-batch, but since v==0 is treated as "free"
 * (loop only acts on orphans), stale entries are silently skipped. */

static u8    g_lnf_tried     = 0u;
static DWORD g_lnf_first_clu = 0ul;     /* 0 = fallback to root */
static DWORD g_lnf_cur_clu   = 0ul;
static UINT  g_lnf_cur_sec   = 0u;
static UINT  g_lnf_cur_off   = 0u;

/* No phase4_lostchn_reset(): scan_run() is called once per program
 * invocation, and the explicit `= 0` initialisers above land these
 * in _INITIALIZED so gsinit gives them a clean starting state. */

/* Forward-scan FAT for a free cluster (entry == 0), starting from
 * cluster 2 every call. No hint optimisation: a previous version
 * cached the last allocation in g_free_cluster_hint, but the value
 * was observed corrupted in the field (hint = 0x2A41C26C with
 * subsequent extends bailing out empty-handed) -- robustness > the
 * O(n_fatent) speed-up. Per-extend cost on FAT16 32k-cluster: ~128
 * cached sector loads, sub-second on Sprinter DSS. */
static int find_free_fat_cluster(vol_t *fs, DWORD *out)
{
    DWORD c;
    for (c = 2ul; c < fs->n_fatent; c++) {
        DWORD v = chain_get_entry(fs, c);
        if (v == CHAIN_READ_ERROR) return 0;
        if (v == 0ul) {
            *out = c;
            return 1;
        }
    }
    return 0;
}

/* Zero-fill all sectors of cluster `c`. Used directly for chain
 * extension; init_dir_cluster() calls this then patches sector 0. */
static int zero_cluster(vol_t *fs, DWORD c)
{
    LBA_t base = chain_cluster_to_lba(fs, c);
    UINT  i;
    /* About to overwrite g_sect_a with zeros, but chain.c may still
     * think it holds a FAT sector. Tell chain.c its cache is stale. */
    chain_invalidate();
    memset(g_sect_a, 0, 512u);
    for (i = 0u; i < (UINT)fs->csize; i++) {
        if (!fix_write(base + (LBA_t)i, g_sect_a, 1u)) return 0;
    }
    return 1;
}

/* Initialize a fresh directory cluster: zero-fill, then patch `.`
 * and `..` SFN entries into sector 0. ".." parent FstClus is fixed
 * to 0 (LOSTCHN's parent is always root, per FAT spec ".." in a
 * child of root encodes as FstClus = 0).
 * Clobbers g_sect_a; caller chain_invalidate()s afterwards. */
static int init_dir_cluster(vol_t *fs, DWORD c)
{
    LBA_t base = chain_cluster_to_lba(fs, c);
    UINT  i;
    BYTE *cb;

    if (!zero_cluster(fs, c)) return 0;
    /* zero_cluster left g_sect_a all-zero -- patch dots in place. */
    g_sect_a[0] = '.';
    for (i = 1u; i < 11u; i++) g_sect_a[i] = ' ';
    g_sect_a[11] = ATTR_DIR_BYTE;
    stamp_entry_dt(&g_sect_a[0]);
    cb = (BYTE *)&c;
    g_sect_a[20] = cb[2]; g_sect_a[21] = cb[3];
    g_sect_a[26] = cb[0]; g_sect_a[27] = cb[1];
    g_sect_a[32] = '.';
    g_sect_a[33] = '.';
    for (i = 34u; i < 43u; i++) g_sect_a[i] = ' ';
    g_sect_a[43] = ATTR_DIR_BYTE;
    stamp_entry_dt(&g_sect_a[32]);
    return fix_write(base, g_sect_a, 1u);
}

/* Lazy-open LOSTCHN. First tries to reuse an existing LOSTCHN dir
 * entry left over from a prior /F /C run -- positions the cursor at
 * offset 64 of its first cluster (skipping `.`/`..`), so subsequent
 * alloc_slot calls overwrite any stale FILE####.CHK entries inside.
 * If none exists, creates a fresh one (alloc one FAT cluster, init
 * `.`/`..`, write dir entry into root).
 *
 * Reuse skips the chain-walk-to-tail: stale FILE####.CHK entries
 * inside an existing LOSTCHN are simply overwritten, leaving their
 * old chains as orphans that the same /F /C sweep then converts.
 *
 * On any failure (no free cluster, root full, write error) we
 * print one WARN and return 0; alloc_slot then falls back to
 * writing CHK entries directly into root. */
static int phase4_lostchn_open(vol_t *fs)
{
    DWORD newc;
    LBA_t sect;
    WORD  off;
    UINT  i;
    BYTE *e, *cb;

    g_lnf_tried = 1u;

    /* Reuse an existing LOSTCHN if any. */
    if (find_root_slot(fs, &sect, &off, ROOT_MATCH_LOSTCHN)) {
        DWORD existing;
        if (disk_read(0u, g_sect_a, sect, 1u) != RES_OK) goto fail;
        existing = entry_first_cluster(&g_sect_a[off]);
        if (existing >= 2ul && existing < fs->n_fatent) {
            g_lnf_first_clu = existing;
            g_lnf_cur_clu   = existing;
            g_lnf_cur_sec   = 0u;
            g_lnf_cur_off   = 64u;     /* skip `.`/`..` */
            return 1;
        }
        /* corrupt cluster pointer in the existing entry -- fall
         * through and create a new LOSTCHN; the bad entry stays. */
    }

    if (!find_free_fat_cluster(fs, &newc)) goto fail;
    if (!fix_fat_set(fs, newc, CHAIN_SET_EOC)) goto fail;
    /* Mark in bitmap so the phase 4 orphan-head loop -- which only
     * skips v==0 / bad / already-marked -- does not treat LOSTCHN's
     * own EOC cluster as an orphan and create a self-referential
     * FILE####.CHK pointing back into LOSTCHN. */
    bitmap_set((u32)newc);
    if (!init_dir_cluster(fs, newc)) goto fail;
    if (!find_root_slot(fs, &sect, &off, ROOT_MATCH_FREE)
        && !find_root_slot(fs, &sect, &off, ROOT_MATCH_CHK)) {
        goto fail;
    }
    if (disk_read(0u, g_sect_a, sect, 1u) != RES_OK) goto fail;
    e = &g_sect_a[off];
    memcpy(e, LOSTCHN_SFN, 11u);
    e[11] = ATTR_DIR_BYTE;
    for (i = 12u; i < 32u; i++) e[i] = 0u;
    stamp_entry_dt(e);
    cb = (BYTE *)&newc;
    e[20] = cb[2]; e[21] = cb[3];
    e[26] = cb[0]; e[27] = cb[1];
    if (!fix_write(sect, g_sect_a, 1u)) goto fail;

    g_lnf_first_clu = newc;
    g_lnf_cur_clu   = newc;
    g_lnf_cur_sec   = 0u;
    g_lnf_cur_off   = 64u;     /* skip `.`/`..` */
    return 1;

fail:
    warn_str("LOSTCHN unavail");
    return 0;
}

/* Allocate next slot (sect, off) for a CHK entry. Lazy-opens LOSTCHN
 * on first call; falls back to root if LOSTCHN is unusable. Extends
 * the LOSTCHN chain (alloc cluster + link + zero-fill) when the
 * current cluster is exhausted. */
static int phase4_lostchn_alloc_slot(vol_t *fs, LBA_t *out_sect, WORD *out_off)
{
    char  xr   = '?';
    DWORD newc = 0ul;

    if (!g_lnf_tried) (void)phase4_lostchn_open(fs);

    if (g_lnf_first_clu == 0ul) {
        return find_free_root_slot(fs, out_sect, out_off);
    }

    /* Extend LOSTCHN chain when the current cluster is exhausted.
     * Sub-code identifies which step failed:
     *   f = find_free_fat_cluster (no free FAT entry left)
     *   l = fix_fat_set linking current cluster to the new one
     *   e = fix_fat_set writing EOC into the new cluster's entry
     *   z = zero_cluster (data-area write failure mid-fill)
     * No chain_invalidate() here: the phase 4 main loop already
     * invalidates before every phase4_walk_chain call. */
    if (g_lnf_cur_sec >= (UINT)fs->csize) {
        if (!find_free_fat_cluster(fs, &newc))     { xr = 'f'; goto extend_fail; }
        if (!fix_fat_set(fs, g_lnf_cur_clu, newc)) { xr = 'l'; goto extend_fail; }
        if (!fix_fat_set(fs, newc, CHAIN_SET_EOC))   { xr = 'e'; goto extend_fail; }
        /* See phase4_lostchn_open: mark before the data-area write so
         * an interleaved main-loop iteration cannot mistake newc for
         * an orphan head. */
        bitmap_set((u32)newc);
        if (!zero_cluster(fs, newc))               { xr = 'z'; goto extend_fail; }
        g_lnf_cur_clu = newc;
        g_lnf_cur_sec = 0u;
        g_lnf_cur_off = 0u;
    }

    *out_sect = chain_cluster_to_lba(fs, g_lnf_cur_clu) + (LBA_t)g_lnf_cur_sec;
    *out_off  = (WORD)g_lnf_cur_off;
    g_lnf_cur_off += 32u;
    if (g_lnf_cur_off >= 512u) {
        g_lnf_cur_off = 0u;
        g_lnf_cur_sec++;
    }
    return 1;

extend_fail:
    /* Step (xr) is f/l/e/z; user can re-run with /V if the cause
     * needs investigation. Compact note to keep _CODE under budget. */
    (void)xr; (void)newc;
    warn_str("LOSTCHN extend");
    g_lnf_first_clu = 0ul;
    return find_free_root_slot(fs, out_sect, out_off);
}

/* ---------------------------------------------------------------- */

/* Phase 4: lost cluster scan with two repair modes.
 *
 *   default /F: zero each orphan FAT entry in place (free).
 *   /F /C:      identify each orphan chain head and create a
 *               FILE####.CHK entry under LOSTCHN/ pointing at it.
 *
 * Bitmap state at entry: 1 = reachable from any directory entry
 * (set by walk_tree). The pass scans clusters in ascending order;
 * for /CONVERT each still-unclaimed orphan starts a claim-walk
 * (phase4_walk_chain) that marks its whole chain in the bitmap and
 * EOC-terminates it, so every orphan is converted exactly once and
 * every FILE####.CHK is linear and self-contained. A chain whose
 * middle has a lower cluster index than its head converts as two
 * files -- data preserved, cosmetically split. */
static int phase4_lost(vol_t *fs)
{
    DWORD lost_n   = 0ul;
    DWORD chain_n  = 0ul;
    DWORD seq      = 1ul;
    DWORD sec_idx;
    UINT  shift, n_per;
    DWORD bad_marker;
    LBA_t fat1;
    int   converting;

    prt_str("Phase 4: lost clusters\r\n");

    converting = fix_enabled() && fix_convert_enabled();
    phase4_capture_now();

#if CHKDSK_FAT12
    /* FAT12 has 1.5-byte entries that may straddle a sector boundary,
     * so the per-sector scan loop used for FAT16/32 doesn't apply.
     * Walk cluster-by-cluster using chain_get_entry for reads (already
     * straddle-aware) and fix_fat_set for the per-cluster writes
     * (likewise). Slow on large volumes -- but FAT12 is bounded to
     * ~4085 clusters, so this is fine in practice. */
    if (fs->fs_type == FS_FAT12) {
        DWORD cc;

        for (cc = 2ul; cc < fs->n_fatent; cc++) {
            DWORD v;
            if ((cc & 0x7Ful) == 0ul) {
                fix_verbose_tick();
                if (fix_poll_abort()) { warn_str("aborted"); return -1; }
            }
            if (bitmap_get((u32)cc)) continue;
            v = chain_get_entry(fs, cc);
            if (v == CHAIN_READ_ERROR) { err_str("FAT read"); return -1; }
            if (v == 0ul || chain_is_bad(fs, v)) continue;

            if (converting) {
                DWORD chain_len = phase4_walk_chain(fs, cc);
                LBA_t e_sect;
                WORD  e_off;
                BYTE  name[11];
                if (chain_len == 0ul) {
                    /* defensive */
                } else if (phase4_lostchn_alloc_slot(fs, &e_sect, &e_off)) {
                    /* size = chain_len * cluster_bytes; shift, no _mullong. */
                    DWORD chk_size = chain_len << (fs->csize_shift + 9u);
                    format_chk_name(name, seq);
                    if (write_chk_entry(e_sect, e_off, name, cc, chk_size)) {
                        chain_n++;
                        seq++;
                        lost_n += chain_len;
                    } else {
                        warn_str("FILE write");
                    }
                } else {
                    warn_str("no slot");
                }
                chain_invalidate();
            } else {
                if (fix_enabled()) {
                    /* Soft failure -- see the FAT16/32 free-mode branch
                     * below for why this must not abort the sweep. */
                    if (!fix_fat_set(fs, cc, 0ul)) warn_str("FAT free");
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
    { fix_verbose_flush(); prt_str("  (skipped)\r\n"); return 0; }

    fat1 = fs->fatbase;

    /* Open one DSS page for batched FAT reads. Both the /CONVERT
     * pre-pass and the main scan reuse the same page (one
     * open/close, two passes through the FAT in 32-sector batches).
     *
     * Two notes on coherence in the main pass:
     *  - chain.c's single-sector FAT cache (g_cached_sect / g_sect_a)
     *    is invalidated before every chain walk: the previous orphan
     *    iteration's find_free_root_slot/write_chk_entry may have
     *    overwritten g_sect_a while leaving g_cached_sect stale.
     *  - bios_drv_write may disturb the WIN3 mapping. After flushing
     *    dirty FAT sectors we call diskio_batch_invalidate() so the
     *    next batch_read re-issues dss_setwin_page. */
    if (!diskio_batch_open(1u)) { err_str("no page"); return -1; }

    sec_idx = 0ul;
    while (sec_idx < fs->fsize) {
        DWORD remaining = fs->fsize - sec_idx;
        u8    batch     = (remaining > BATCH_SECTORS_PER_PAGE)
                          ? (u8)BATCH_SECTORS_PER_PAGE
                          : (u8)remaining;
        u8   *page;
        u8    si;

        fix_verbose_tick();
        if (fix_poll_abort()) {
            warn_str("aborted");
            diskio_batch_close();
            return -1;
        }

        if (!diskio_batch_read((unsigned long)(fat1 + (LBA_t)sec_idx),
                               batch, 0u)) {
            err_str("FAT batch");
            diskio_batch_close();
            return -1;
        }
        page = diskio_batch_map(0u);

        for (si = 0u; si < batch; si++) {
            DWORD this_sec = sec_idx + (DWORD)si;
            DWORD start_c  = (shift == 2u) ? (this_sec << 7)
                                           : (this_sec << 8);
            const u8 *sec_buf = page + (UINT)si * 512u;
            UINT      cc_in;

            if (start_c >= fs->n_fatent) break;

            for (cc_in = 0u; cc_in < n_per; cc_in++) {
                DWORD cc  = start_c + (DWORD)cc_in;
                UINT  pos = cc_in << shift;
                DWORD v;

                if (cc < 2ul || cc >= fs->n_fatent) continue;
                /* Re-establish WIN3 = batch page; bitmap or chain
                 * helpers in the previous iteration may have remapped
                 * it. Without this, read_fat_entry_in_buf returns
                 * bitmap or FAT-cache memory instead of batch FAT. */
                (void)diskio_batch_map(0u);
                v = read_fat_entry_in_buf(sec_buf, pos, shift);
                if (v == 0ul || v == bad_marker) continue;
                if (bitmap_get((u32)cc)) continue;

                /* The batch page is only a CANDIDATE source: a bulk
                 * read used to spot orphan heads quickly. Re-read the
                 * entry through the single-sector path before acting,
                 * so the value we free/convert comes from the same
                 * path every write goes through -- never a byte left
                 * over from the bulk scan. */
                chain_invalidate();
                v = chain_get_entry(fs, cc);
                if (v == CHAIN_READ_ERROR) {
                    err_str("FAT read");
                    diskio_batch_close();
                    return -1;
                }
                if (v == 0ul || v == bad_marker) continue;

                if (converting) {
                    /* cc is an orphan chain head. Walk + create
                     * FILE####.CHK in the root. The helpers use
                     * g_sect_a (disjoint from the WIN3 batch page),
                     * so the FAT data we are scanning stays intact
                     * -- no FAT re-read needed. */
                    DWORD chain_len;
                    LBA_t e_sect;
                    WORD  e_off;
                    BYTE  name[11];

                    chain_len = phase4_walk_chain(fs, cc);

                    if (chain_len == 0ul) {
                        /* defensive: cycle protection in walker */
                    } else if (phase4_lostchn_alloc_slot(fs, &e_sect, &e_off)) {
                        DWORD chk_size = chain_len << (fs->csize_shift + 9u);
                        format_chk_name(name, seq);
                        if (write_chk_entry(e_sect, e_off, name, cc, chk_size)) {
                            chain_n++;
                            seq++;
                            lost_n += chain_len;
                        } else {
                            warn_str("FILE write");
                        }
                    } else {
                        /* Press ESC or Ctrl+C to abort the conversion run. */
                        warn_str("no slot");
                        if (fix_poll_abort()) {
                            warn_str("aborted");
                            diskio_batch_close();
                            return -1;
                        }
                    }
                } else {
                    /* Free mode: zero the orphan's entry through
                     * fix_fat_set (single-sector read-modify-write,
                     * mirrored into FAT 2). Never write batch-page
                     * sectors back wholesale -- the batch page is a
                     * read-only bulk-scan buffer; flushing it would
                     * clobber the single-sector repairs. */
                    if (fix_enabled()) {
                        if (!fix_fat_set(fs, cc, 0ul)) {
                            /* Soft failure, same as every other repair
                             * site (e.g. chain-trunc above) -- a
                             * declined WARNING gate or a transient
                             * write error here doesn't warrant aborting
                             * the whole sweep, just this one cluster. */
                            warn_str("FAT free");
                        }
                    }
                    lost_n++;
                }
            }
        }

        sec_idx += (DWORD)batch;
    }
    diskio_batch_close();
    fix_verbose_flush();

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
    int lost_rc;

    prt_str("Phase 3: directory and chain walk\r\n");

    if (!bitmap_init((u32)fs->n_fatent)) {
        prt_str("  error: cannot allocate cluster bitmap (");
        prt_dec((unsigned long)fs->n_fatent);
        prt_str(" entries)\r\n");
        return -1;
    }
    bitmap_set(0u);
    bitmap_set(1u);

#if CHKDSK_FAT32
    /* Claim the FAT32 root directory chain. FAT12/16 roots live in a
     * reserved region outside the cluster heap, but the FAT32 root is
     * an ordinary cluster chain -- without claiming it here, phase 4
     * would count the root's own clusters as lost, and a /F run would
     * free (or /F /C convert into a CHK file) the root directory
     * itself. The claim-walk also EOC-terminates a rotten root chain
     * under /F, same as any other chain repair. */
    if (fs->fs_type == FS_FAT32) {
        (void)phase4_walk_chain(fs, (DWORD)fs->dirbase);
    }
#endif

    if (walk_tree(fs) < 0) {
        prt_str("  error: walk aborted\r\n");
        bitmap_release();
        return -1;
    }
    fix_verbose_flush();

    prt_str("  Totals: entries=");
    prt_dec((unsigned long)g_totals.entries);
    prt_str(" dirs=");
    prt_dec((unsigned long)g_totals.dirs);
    emit_count(" flagged=",     g_totals.flagged);
    emit_count(" cycles=",      g_totals.cycles);
    emit_count(" crosslinks=",  g_totals.cross_links);
    emit_count(" broken=",      g_totals.broken_chains);
    emit_count(" truncated=",   g_totals.truncated);
    emit_count(" excess=",      g_totals.excess);
    emit_count(" depth-cap=",   g_totals.depth_capped);
    prt_nl();

    lost_rc = phase4_lost(fs);

    bitmap_release();
    if (lost_rc < 0) return -1;
    return (int)(g_totals.flagged + g_totals.cycles + g_totals.cross_links
               + g_totals.broken_chains + g_totals.truncated
               + g_totals.excess) + lost_rc;
}
