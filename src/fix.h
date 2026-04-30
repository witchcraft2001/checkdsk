/*
 * fix.h -- write-mode infrastructure (`/F` flag).
 *
 * In default (read-only) mode every fix_*() call no-ops and only
 * updates the per-class issue counter. With `/F` on the command line,
 * fix_enable() flips a global gate and fix_write() calls disk_write
 * for real. Each repair site goes through fix_write so we have a
 * single audit point for all mutations.
 *
 * Two counters: "found" bumps once per logical issue noticed (whether
 * /F is set or not), "applied" bumps once per logical repair that
 * actually wrote to disk. The summary is printed by fix_print_summary
 * at the end of dispatch().
 */

#ifndef CHKDSK_FIX_H
#define CHKDSK_FIX_H

#include "vol.h"
#include "diskio.h"

/* Toggled by main.c on `/F` parse. Default: read-only. */
void fix_enable(void);
int  fix_enabled(void);

/* Account for an issue we noticed (called whether or not /F is set). */
void fix_count_found(void);

/* Bump the "applied" (logical fixes successfully applied) counter.
 * One repair-site call per completed repair, regardless of how many
 * fix_write sector-writes it took. Called from inside fix_dir_patch
 * on success; high-level repair sites that bypass fix_dir_patch (e.g.
 * the multi-sector FAT 1/2 sync) call this directly once the whole
 * sweep succeeds. */
void fix_count_applied(void);

/* Try to write `count` sectors from `buf` to `lba` (vol-relative).
 * Read-only mode: no-op, returns 1. Write mode: issues disk_write,
 * returns 1 on success / 0 on failure. Does NOT touch counters --
 * those are bumped by the higher-level helper that orchestrates the
 * repair (fix_dir_patch, sync_fats, etc). */
int  fix_write(LBA_t lba, const BYTE *buf, UINT count);

/* Print the trailing summary. */
void fix_print_summary(void);

/* Non-zero if any fix_count_found was invoked -- used for the process
 * exit code so a script can branch on it. */
int  fix_any_found(void);

/* Directory-entry patch kinds for fix_dir_patch. */
#define FIX_DPATCH_SIZE       0u  /* set size DWORD (off+28..31) to value */
#define FIX_DPATCH_DELETE     1u  /* mark entry deleted (off byte = 0xE5) */
#define FIX_DPATCH_DOT_CLUST  2u  /* set FstClus(HI|LO) (off+20..21,26..27) */

/* Patch the 32-byte directory entry at on-disk position (sect, off).
 * Re-reads the sector into g_sect_a, applies the patch, writes it
 * back, bumps the "applied" counter. Returns 1 on success (or no-op
 * in read-only mode), 0 on I/O failure. The dirwalk cursor that
 * returned this entry must be marked buffer_dirty by the caller --
 * g_sect_a is clobbered on every call. */
int  fix_dir_patch(LBA_t sect, WORD off, u8 kind, DWORD value);


#endif /* CHKDSK_FIX_H */
