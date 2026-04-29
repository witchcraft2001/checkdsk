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
#include "dirwalk.h"
#include "dirent.h"
#include "prt.h"
#include "scan.h"

#define SCAN_MAX_DEPTH   8u

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
    DWORD cycles;
    DWORD depth_capped;
} scan_totals_t;

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
 *  -1 -- I/O error (bubble up) */
static int step(vol_t *fs, BYTE *depth, scan_totals_t *t)
{
    scan_frame_t *frame;
    BYTE         *e;
    int           rc;
    UINT          flags;
    DWORD         clust;

    frame = &g_frames[*depth];
    rc = dirwalk_next(&frame->walker, &e);
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

    /* Skip "." and ".." silently. Their cluster pointers are validated
     * by Stage 3.5, not here. */
    if ((e[11] & 0x3Fu) != ATTR_LFN && e[0] != 0xE5u && is_dot_entry(e)) {
        return 1;
    }

    t->entries++;
    flags = dirent_validate(fs, e);
    if (flags & DE_ANY_ERROR) {
        t->flagged++;
        print_flagged(*depth, e);
        prt_str(" *");
        dirent_flags_print(flags);
        prt_nl();
    }

    if (!is_descendable_dir(e)) return 1;

    clust = entry_first_cluster(e);
    if (clust < 2ul || clust >= fs->n_fatent) return 1; /* DE_CLUST_OOR */

    if (bitmap_test_and_set(clust)) {
        prt_str("  cycle/cross-link at cluster 0x");
        prt_hex((unsigned long)clust, 8u);
        prt_str("  ");
        print_path(*depth);
        print_sfn(e);
        prt_nl();
        t->cycles++;
        return 1;
    }

    if (*depth >= SCAN_MAX_DEPTH) {
        prt_str("  depth-limit ");
        print_path(*depth);
        print_sfn(e);
        prt_nl();
        t->depth_capped++;
        return 1;
    }

    /* Descend. */
    (*depth)++;
    t->dirs++;
    copy_name(g_frames[*depth].name, e);
    dirwalk_open_chain(&g_frames[*depth].walker, fs, clust);
    /* parent frame's g_sect_a is gone -- mark it. */
    dirwalk_buffer_dirty(&g_frames[*depth - 1u].walker);
    return 1;
}

static int walk_tree(vol_t *fs, scan_totals_t *t)
{
    BYTE depth;
    int  rc;

    t->entries      = 0ul;
    t->dirs         = 0ul;
    t->flagged      = 0ul;
    t->cycles       = 0ul;
    t->depth_capped = 0ul;

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
        prt_str("  error: walk aborted (bios=");
        prt_dec((unsigned long)0u);
        prt_str(")\r\n");
        bitmap_release();
        return -1;
    }

    prt_str("  Totals: entries=");
    prt_dec((unsigned long)t.entries);
    prt_str(" dirs=");
    prt_dec((unsigned long)t.dirs);
    if (t.flagged) {
        prt_str(" flagged=");
        prt_dec((unsigned long)t.flagged);
    }
    if (t.cycles) {
        prt_str(" cycles=");
        prt_dec((unsigned long)t.cycles);
    }
    if (t.depth_capped) {
        prt_str(" depth-capped=");
        prt_dec((unsigned long)t.depth_capped);
    }
    prt_nl();

    bitmap_release();
    return (int)(t.flagged + t.cycles);
}
