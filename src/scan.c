/*
 * scan.c -- Phase 3 driver. See scan.h.
 *
 * Stage 3.4: iterative depth-first walk over the entire directory
 * tree starting at the root.  An explicit stack of dirwalk frames
 * caps the recursion depth at SCAN_MAX_DEPTH and keeps the C call
 * stack flat -- the per-frame state lives in BSS.
 *
 * Cycle / cross-link detection: bitmap_test_and_set on the first
 * cluster of every sub-directory we enter. A bit already set means
 * either a real loop in the FAT (".." pointing forward) or two
 * directory entries claiming the same starting cluster. We refuse
 * to descend in either case.
 *
 * "." and ".." are skipped during the walk -- their cluster pointers
 * are validated by Stage 3.5 (chain checker), not here.
 *
 * Output policy: only flagged entries (and cycle / depth-cap warnings)
 * are echoed, prefixed by their full path. Clean entries are silent;
 * the trailing Totals line gives the population counts. Trees with
 * thousands of entries thus produce a handful of lines unless there
 * are real findings.
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

/* Stage 3.6 (LFN sequence + SFN checksum) deferred -- the rotate-add
 * checksum + per-frame state, even in its minimal form, ate the last
 * static-data bytes left under the 0xBFFF stack ceiling. Future plan:
 * shrink fat.c (6.6 KB) and chain.c by sharing FAT12/16/32 dispatch,
 * which would free the room. dirent_validate keeps the basic per-slot
 * LFN sanity (DE_LFN_BAD: type byte / first cluster). */

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
 * entries and emit a cascade of garbage findings. (Stage 4.2's
 * dir-size repair is irreversible, so even after a previous /F run
 * an entry that originally was a corrupted file may now look like
 * a clean dir; the dflags guard catches that case via the other
 * tell-tale flags -- bad-name, attr-rsv, clust-oor, fat16-hi,
 * vol-nz, lfn-bad.) */
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

    /* Stage 4.3: validate "." / ".." cluster pointers, then continue
     * iteration without counting them as regular entries.
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

        /* Stage 4.2: ATTR_DIR with a non-zero size DWORD. Two flavours:
         *   has_dot == 1 -- a real subdir whose size got corrupted; just
         *                   zero the size DWORD, structure stays intact.
         *   otherwise    -- a file whose attribute byte got corrupted to
         *                   include ATTR_DIR; mark the entry deleted so
         *                   the walker can't be tricked into descending
         *                   into user data. The cluster chain becomes
         *                   orphaned (Stage 4.6 will recover it). */
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

    /* Stage 4.4: shrink the file's size DWORD to match the actual chain
     * coverage when the chain was cut short or hit garbage. Covers
     * TRUNCATED (clean EOC earlier than expected), BROKEN/BAD (hard
     * error mid-chain), and CROSS (chain ran into territory already
     * claimed by another file). */
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
    dirwalk_open_root(&g_frames[0].walker, fs);

    for (;;) {
        rc = step(fs, &depth, t);
        if (rc < 0) return -1;
        if (depth == 0xFFu) break;
    }
    return 0;
}

/* Stage 4.6 (compact): walk every FAT entry and free those that are
 * in-use but unreachable from any directory (bitmap unset after the
 * Phase 3 walk). Single sector-by-sector pass: read FAT 1, patch
 * orphan entries to zero in place, write back to FAT 1 (and FAT 2
 * if n_fats == 2). The whole sweep counts as one logical fix.
 * FAT16 and FAT32 share the loop body via `shift` (1 vs 2 = bytes
 * per entry log2). */
static int phase4_lost(vol_t *fs)
{
    DWORD lost_n = 0ul;
    DWORD sec_idx;
    UINT  shift, n_per;
    DWORD bad_marker;
    LBA_t fat1, fat2;

    prt_str("Phase 4: lost clusters\r\n");

#if CHKDSK_FAT32
    if (fs->fs_type == FS_FAT32)      { shift = 2u; n_per = 128u; bad_marker = 0x0FFFFFF7ul; }
    else
#endif
#if CHKDSK_FAT16
    if (fs->fs_type == FS_FAT16)      { shift = 1u; n_per = 256u; bad_marker = 0xFFF7ul;     }
    else
#endif
    { prt_str("  (skipped)\r\n"); return 0; }

    fat1 = fs->fatbase;
    fat2 = fat1 + (LBA_t)fs->fsize;

    for (sec_idx = 0ul; sec_idx < fs->fsize; sec_idx++) {
        DWORD start_c = (shift == 2u) ? (sec_idx << 7) : (sec_idx << 8);
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

            v = (DWORD)g_sect_a[pos] | ((DWORD)g_sect_a[pos + 1u] << 8);
            if (shift == 2u) {
                v |= ((DWORD)g_sect_a[pos + 2u] << 16)
                   | ((DWORD)(g_sect_a[pos + 3u] & 0x0Fu) << 24);
            }
            if (v == 0ul || v == bad_marker) continue;

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

        if (dirty) {
            if (!fix_write(fat1 + (LBA_t)sec_idx, g_sect_a, 1u)) return -1;
            if (fs->n_fats == 2u
                && !fix_write(fat2 + (LBA_t)sec_idx, g_sect_a, 1u)) return -1;
        }
    }

    if (lost_n == 0ul) {
        prt_str("  No issues\r\n");
    } else if (fix_enabled()) {
        prt_str("  Freed ");
        prt_dec((unsigned long)lost_n);
        prt_str(" lost cluster(s)\r\n");
        fix_count_found();
        fix_count_applied();
    } else {
        prt_str("  Found ");
        prt_dec((unsigned long)lost_n);
        prt_str(" lost cluster(s); /F to free\r\n");
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

    if (walk_tree(fs, &t) < 0) {
        prt_str("  error: walk aborted\r\n");
        bitmap_release();
        return -1;
    }

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
