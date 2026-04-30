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
#include "bitmap.h"
#include "chain.h"
#include "dirwalk.h"
#include "dirent.h"
#include "prt.h"
#include "scan.h"

#define SCAN_MAX_DEPTH  10u

#define ATTR_VOLID 0x08u
#define ATTR_DIR   0x10u
#define ATTR_LFN   0x0Fu

typedef struct {
    dirwalk_t walker;
    BYTE      name[11];      /* parent SFN, valid for depth >= 1 */
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
    DWORD hi = ((DWORD)e[21] << 8) | (DWORD)e[20];
    DWORD lo = ((DWORD)e[27] << 8) | (DWORD)e[26];
    return (hi << 16) | lo;
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
 * "." / ".."), 0 otherwise. A directory with non-zero size is treated
 * as corrupt and not entered -- otherwise we read random data as 32-byte
 * directory entries and emit a cascade of garbage findings. */
static int is_descendable_dir(const BYTE *e)
{
    DWORD size;
    if (e[0] == 0xE5u)                  return 0;
    if ((e[11] & 0x3Fu) == ATTR_LFN)    return 0;
    if (!(e[11] & ATTR_DIR))            return 0;
    if (e[11] & ATTR_VOLID)             return 0;
    if (is_dot_entry(e))                return 0;
    size = ((DWORD)e[31] << 24) | ((DWORD)e[30] << 16)
         | ((DWORD)e[29] << 8)  | (DWORD)e[28];
    if (size != 0ul)                    return 0;
    return 1;
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

    /* "." and ".." -- skipped silently. */
    if ((ent[11] & 0x3Fu) != ATTR_LFN && ent[0] != 0xE5u && is_dot_entry(ent)) {
        return 1;
    }

    t->entries++;
    dflags    = dirent_validate(fs, ent);
    cflags    = 0u;
    chain_len = 0ul;

    clust   = entry_first_cluster(ent);
    is_file = ((ent[11] & 0x3Fu) != ATTR_LFN) && !(ent[11] & ATTR_DIR)
            && !(ent[11] & ATTR_VOLID) && (ent[0] != 0xE5u);
    size    = ((DWORD)ent[31] << 24) | ((DWORD)ent[30] << 16)
            | ((DWORD)ent[29] << 8)  | (DWORD)ent[28];

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

    {
        UINT de_err = dflags & DE_ANY_ERROR;
        if (de_err || cflags) {
            print_flagged(*depth, ent);
            prt_str(" *");
            if (de_err) dirent_flags_print(dflags);
            if (cflags) print_cw_tags(cflags);
            prt_nl();
            if (de_err) t->flagged++;
        }
    }

    if (cflags & CW_CYCLE)     t->cycles++;
    if (cflags & CW_CROSS)     t->cross_links++;
    if (cflags & CW_BROKEN)    t->broken_chains++;
    if (cflags & CW_TRUNCATED) t->truncated++;
    if (cflags & CW_EXCESS)    t->excess++;

    if (!is_descendable_dir(ent)) return 1;
    if (clust < 2ul || clust >= fs->n_fatent) return 1;
    if (cflags & (CW_CYCLE | CW_IO_ERR)) return 1;

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
    dirwalk_open_root(&g_frames[0].walker, fs);

    for (;;) {
        rc = step(fs, &depth, t);
        if (rc < 0) return -1;
        if (depth == 0xFFu) break;
    }
    return 0;
}

int scan_run(vol_t *fs)
{
    scan_totals_t t;

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

    bitmap_release();
    return (int)(t.flagged + t.cycles + t.cross_links + t.broken_chains
               + t.truncated + t.excess);
}
