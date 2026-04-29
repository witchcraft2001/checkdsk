/*
 * dirwalk.h -- iterative 32-byte directory-entry walker.
 *
 * Used by Phase 3 to stream entries from the root (Stage 3.2) and
 * later from sub-directories (Stage 3.4). Handles both layouts:
 *   FAT12/16 root  -- fixed `n_rootdir` entries starting at `dirbase`
 *   FAT32 / subdir -- cluster chain followed via chain_get_entry
 *
 * Each call to dirwalk_next yields a pointer into g_sect_a that is
 * valid until the next dirwalk_next call -- callers must copy any
 * fields they need before advancing.
 *
 * g_sect_a is shared with chain.c. dirwalk re-loads the directory
 * sector after every FAT-chain lookup, and invalidates chain's cache
 * before doing so. Concurrent users of g_sect_a outside dirwalk are
 * not supported during a walk.
 */

#ifndef CHKDSK_DIRWALK_H
#define CHKDSK_DIRWALK_H

#include "vol.h"

/* Walker state. Treat as opaque -- pass to dirwalk_next / dirwalk_open. */
typedef struct {
    vol_t *fs;
    DWORD  cluster;          /* current cluster, 0 in fixed-root mode */
    DWORD  entries_left;     /* fixed-root countdown; 0xFFFFFFFF in chain mode */
    LBA_t  cur_sect;         /* next sector LBA to load */
    WORD   sect_off;         /* byte offset within g_sect_a (0..512) */
    WORD   cluster_sects_left; /* sectors remaining in current cluster */
    BYTE   end_seen;         /* 1 once 0x00 marker observed */
    BYTE   buffer_dirty;     /* 1 if g_sect_a was clobbered by other code */
} dirwalk_t;

/* Open the root directory. Always returns 0 (no I/O yet). */
int  dirwalk_open_root(dirwalk_t *w, vol_t *fs);

/* Open a sub-directory rooted at `start_cluster`. Always 0 (no I/O). */
int  dirwalk_open_chain(dirwalk_t *w, vol_t *fs, DWORD start_cluster);

/* Fetch the next 32-byte entry. Returns:
 *   1 -- *out_entry points at a valid 32-byte entry (incl. 0xE5/LFN);
 *        the pointer stays valid until the next dirwalk_next call.
 *   0 -- end of directory (0x00 terminator seen, or fixed-root exhausted,
 *        or FAT32 chain reached EOC).
 *  -1 -- I/O or chain error. Inspect diskio_dss_last_error() for context.
 */
int  dirwalk_next(dirwalk_t *w, BYTE **out_entry);

/* Tell the walker that g_sect_a was overwritten by external code (e.g.
 * after recursing into a sub-directory). The walker re-reads its current
 * sector on the next dirwalk_next call so the iteration resumes at the
 * exact same offset. No-op if the walker is at a sector boundary. */
void dirwalk_buffer_dirty(dirwalk_t *w);

#endif /* CHKDSK_DIRWALK_H */
