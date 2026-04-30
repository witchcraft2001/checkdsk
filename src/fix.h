/*
 * fix.h -- write-mode infrastructure (`/F` flag).
 *
 * In default (read-only) mode every fix_*() call no-ops and only
 * updates the per-class issue counter. With `/F` on the command line,
 * fix_enable() flips a global gate and fix_write() calls disk_write
 * for real. Each repair site goes through fix_write so we have a
 * single audit point for all mutations and a single counter that
 * tells the user how many fixes were actually applied.
 *
 * Repair sites (Stage 4.2..4.6) call fix_<class>() helpers (added in
 * those stages) which delegate to fix_write internally. The summary
 * is printed by fix_print_summary() at the end of dispatch().
 */

#ifndef CHKDSK_FIX_H
#define CHKDSK_FIX_H

#include "vol.h"
#include "diskio.h"

/* Toggled by main.c on `/F` parse. Default: read-only. */
void fix_enable(void);

/* True when `/F` was given. Repair sites use this to decide whether
 * to attempt the write or just log the would-be repair. */
int  fix_enabled(void);

/* Account for an issue we noticed: increments the "found" counter.
 * Called whether or not /F is set. */
void fix_count_found(void);

/* Try to write `count` sectors from `buf` to `lba` (vol-relative).
 * In read-only mode this is a no-op that returns 1. In write mode it
 * issues disk_write and returns 1 on success / 0 on failure. Does NOT
 * touch the "applied" counter -- callers must call fix_count_applied()
 * once per logical repair (which may span many fix_write sectors). */
int  fix_write(LBA_t lba, const BYTE *buf, UINT count);

/* Bump the "applied" (logical fixes successfully applied) counter.
 * One repair-site call per completed repair, regardless of how many
 * fix_write sector-writes it took. Read-only callers may also invoke
 * this safely; the counter only matters when /F is set. */
void fix_count_applied(void);

/* Print the trailing summary:
 *   read-only: "Found N issues; run with /F to fix"
 *   write:     "Found N issues, M fixed"
 * Called once at the end of dispatch(). */
void fix_print_summary(void);

/* Return non-zero if any fix-class issue was recorded -- used for
 * the process exit code so a script can branch on it. */
int  fix_any_found(void);

/* Stage 4.2: zero the size DWORD (offset 28-31) of the directory
 * entry at on-disk position (sect, off). Re-reads the sector into
 * g_sect_a, patches it, writes it back. Returns 1 on success
 * (or no-op in read-only mode), 0 on I/O failure. The dirwalk
 * cursor that returned this entry must be marked buffer_dirty by
 * the caller -- g_sect_a is clobbered on every call.
 *
 * Use when the entry IS a real subdir (its first cluster begins with
 * a "." entry) but its size DWORD is corrupted. */
int  fix_dir_size_zero(LBA_t sect, WORD off);

/* Stage 4.2 (A): mark the directory entry at (sect, off) as deleted
 * by writing 0xE5 to the first byte. Same I/O contract as above.
 *
 * Use when the entry pretends to be a directory (ATTR_DIR set) but
 * its first cluster contains user data, not subdir entries -- in
 * other words a corrupted file masquerading as an empty dir. The
 * cluster chain becomes orphaned and is later picked up by Stage 4.6
 * (lost cluster recovery) into FOUND####.CHK. */
int  fix_entry_delete(LBA_t sect, WORD off);

#endif /* CHKDSK_FIX_H */
