/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
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

/* Toggled by main.c on `/C` parse. Modifier on /F that changes Phase 4
 * lost-cluster handling: instead of freeing orphan clusters silently,
 * convert each orphan chain into a FILE####.CHK entry in the root
 * directory, preserving the chain for the user to inspect/recover.
 * Has no effect without /F. */
void fix_enable_convert(void);
int  fix_convert_enabled(void);

/* Toggled by main.c on `/V` parse. When enabled, the long-running
 * passes (Phase 2 FAT scan, Phase 3 directory walk, Phase 4 sweep)
 * emit a progress indicator (one dot per chunk) so the user can tell
 * the program is working on a multi-second scan. Cosmetic only. */
void fix_enable_verbose(void);
int  fix_verbose_enabled(void);

/* Verbose-progress helpers. fix_verbose_tick() emits one progress
 * dot if /V is on and remembers it; fix_verbose_flush() emits a
 * trailing newline if a dot is currently sitting on the console line.
 * Diagnostic prints (flagged-entry lines, error / warning messages)
 * call flush before their first character so they always start at
 * column 0 instead of running into a half-finished progress streak. */
void fix_verbose_tick(void);
void fix_verbose_flush(void);

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

/* Non-zero if any fix_count_applied was invoked -- main.c uses this to
 * gate the post-/F BIOS DRV_DETECT rescan. */
int  fix_any_applied(void);

/* Directory-entry patch kinds for fix_dir_patch. */
#define FIX_DPATCH_SIZE          0u  /* set size DWORD (off+28..31) to value */
#define FIX_DPATCH_DELETE        1u  /* mark entry deleted (off byte = 0xE5) */
#define FIX_DPATCH_DOT_CLUST     2u  /* set FstClus(HI|LO) (off+20..21,26..27) */
#define FIX_DPATCH_ATTR_MASK     3u  /* attr (off+11) &= 0x3F (drop reserved) */
#define FIX_DPATCH_NTRES_FIX     4u  /* NTRES (off+12) &= 0x18; CrtTen=0 */
#define FIX_DPATCH_HI_CLUST_ZERO 5u  /* zero FstClusHI on FAT12/16 */

/* Write `value` (next-cluster, EOC, BAD, or 0=free) to the FAT entry
 * for `clust`. Mirrors into FAT 2 if n_fats == 2. Reads the FAT sector
 * into g_sect_a, patches the entry in place, writes back. Does NOT
 * bump the "applied" counter -- callers chain many calls into one
 * logical repair. Returns 0 on I/O error or if the FAT type is not
 * 16 / 32. */
int  fix_fat_set(vol_t *fs, DWORD clust, DWORD value);

/* Patch the 32-byte directory entry at on-disk position (sect, off).
 * Re-reads the sector into g_sect_a, applies the patch, writes it
 * back, bumps the "applied" counter. Returns 1 on success (or no-op
 * in read-only mode), 0 on I/O failure. The dirwalk cursor that
 * returned this entry must be marked buffer_dirty by the caller --
 * g_sect_a is clobbered on every call. */
int  fix_dir_patch(LBA_t sect, WORD off, u8 kind, DWORD value);

/* Rewrite the 11-byte SFN name field at (sect, off) with new_name[11].
 * Touches only bytes 0..10 of the entry. Re-reads the sector into
 * g_sect_a, writes back, bumps the "applied" counter. Returns 1 on
 * success (or no-op in read-only mode), 0 on I/O failure. Caller must
 * mark its dirwalk cursor buffer_dirty afterward, same as
 * fix_dir_patch. */
int  fix_dir_name_set(LBA_t sect, WORD off, const BYTE *new_name);

/* An LFN group's checksum byte is computed over the 11 raw SFN bytes
 * of the entry it names, so rewriting those bytes (fix_dir_name_set)
 * orphans any preceding LFN run -- a real LFN-aware driver would then
 * see it as corrupt. Call this FIRST: it looks at the slot(s)
 * immediately before (sect, off) within the SAME sector and, if they
 * are a contiguous LFN run, deletes the whole run (0xE5) so no stale
 * checksum is left behind. `dir_start_sect` is the first sector of the
 * directory (sect, off) lives in (fs->dirbase for a FAT12/16 root,
 * chain_cluster_to_lba of the directory's start cluster otherwise) --
 * needed to tell "off==0 because nothing precedes at all" (the
 * directory's very first slot) apart from "off==0 because this is a
 * later sector we can't see past" (genuinely ambiguous). Returns 1 if
 * (sect, off) is now safe to rename (no run, or the whole run was
 * deleted), 0 if the run appears to continue into an earlier sector
 * we cannot inspect -- in that case NOTHING is written (no partial run
 * is ever left deleted) and the caller should leave the entry unfixed
 * this pass. Post-MVP: recompute the checksum in place instead of
 * deleting, which preserves the long name and does not need this
 * boundary restriction. */
int  fix_delete_preceding_lfn(LBA_t sect, WORD off, LBA_t dir_start_sect);

#endif /* CHKDSK_FIX_H */
