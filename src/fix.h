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

/* Toggled by main.c on `/Y` parse. Suppresses the one-time WARNING
 * gate fix_write() shows before its first actual disk write --
 * consent is assumed. Has no effect without /F (fix_write no-ops
 * before ever reaching the gate in that case). */
void fix_enable_assume_yes(void);
int  fix_assume_yes_enabled(void);

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

/* Account for a found issue that /F did NOT resolve -- either a write
 * genuinely failed (fix_write calls this itself on disk_write failure,
 * so every repair site that funnels through it is covered for free)
 * or a repair site deliberately abstained (e.g. the name-sanitize
 * pack's name-collision guard in scan.c, which calls this directly
 * since it never reaches fix_write at all). NOT
 * exhaustive: a disk_read failure inside a repair site before it ever
 * reaches fix_write is not separately tracked (rare -- transient read
 * fault mid-repair -- and the issue is still correctly counted as
 * "found" either way). main.c uses this to tell exit code 2 (all
 * found issues fixed) apart from 3 (found, some left unfixed) under
 * /F -- best-effort, not a guarantee equivalent to a clean re-scan. */
void fix_count_incomplete(void);
int  fix_any_incomplete(void);

/* Directory-entry patch kinds for fix_dir_patch. */
#define FIX_DPATCH_SIZE          0u  /* set size DWORD (off+28..31) to value */
#define FIX_DPATCH_DELETE        1u  /* mark entry deleted (off byte = 0xE5) */
#define FIX_DPATCH_DOT_CLUST     2u  /* set FstClus(HI|LO) (off+20..21,26..27) */
#define FIX_DPATCH_ATTR_MASK     3u  /* attr (off+11) &= 0x3F (drop reserved) */
#define FIX_DPATCH_NTRES_FIX     4u  /* NTRES (off+12) &= 0x18; CrtTen=0 */
#define FIX_DPATCH_HI_CLUST_ZERO 5u  /* zero FstClusHI on FAT12/16 */
#define FIX_DPATCH_TIMESTAMP     6u  /* reset the date/time fields named
                                        by a DE_TS_* mask in `value` to
                                        the FAT epoch 1980-01-01 00:00 */

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

/* Same as fix_dir_patch, but does NOT bump the "applied" counter --
 * for a caller that issues several patches against ONE logical repair
 * (e.g. an entry with both a reserved-attribute and a bad-timestamp
 * flag) and needs to count the whole group once, not once per patch.
 * The caller calls fix_count_applied() itself after the group succeeds. */
int  fix_dir_patch_raw(LBA_t sect, WORD off, u8 kind, DWORD value);

/* Rewrite the 11-byte SFN name field at (sect, off) with new_name[11].
 * Touches only bytes 0..10 of the entry. Re-reads the sector into
 * g_sect_a, writes back, bumps the "applied" counter. Returns 1 on
 * success (or no-op in read-only mode), 0 on I/O failure. Caller must
 * mark its dirwalk cursor buffer_dirty afterward, same as
 * fix_dir_patch. */
int  fix_dir_name_set(LBA_t sect, WORD off, const BYTE *new_name);

/* Maximum FAT LFN length is 255 UCS-2 characters, i.e. ceil(255/13)
 * directory slots. scan.c stores the exact on-disk location of every
 * slot while walking, which makes repairs work across sector and
 * fragmented-cluster boundaries without a backwards re-scan. */
#define FIX_LFN_MAX_SLOTS 20u

typedef struct {
    LBA_t sect;
    WORD  off;
} fix_lfn_slot_t;

/* Delete all live slots of one broken/orphan LFN group. Slots may span
 * sectors or non-contiguous directory clusters. Each touched sector is
 * changed by one read-modify-write, and the whole group counts as one
 * logical applied fix. Returns 0 on I/O failure.
 *
 * Unlike fix_lfn_name_set there is deliberately NO rollback: a failure
 * partway through a multi-sector group leaves some slots deleted and
 * some not. That is safe here because the group was already corrupt --
 * the surviving remainder is still detected as broken on the next pass
 * and finished off, so repeated runs converge. Rolling back would only
 * restore a group that has to be deleted anyway. */
int fix_lfn_delete(const fix_lfn_slot_t *slots, BYTE count);

/* Atomically within the SFN sector, rewrite the SFN name and every LFN
 * checksum slot that shares that sector. For a group spanning earlier
 * sectors, those sectors are updated first and rolled back to old_sum
 * if a later write fails; the SFN-containing sector is always written
 * last. This keeps an ordinary I/O failure from turning a valid group
 * into a mismatched one. One successful group+SFN update counts as one
 * logical applied fix. */
int fix_lfn_name_set(const fix_lfn_slot_t *slots, BYTE count,
                     BYTE old_sum, BYTE new_sum,
                     LBA_t sfn_sect, WORD sfn_off, const BYTE *new_name);

/* Non-blocking abort poll -- ESC or Ctrl+C (raw ascii, modifier+letter,
 * or modifier+scan-code, to work under both the LAT and RUS keyboard
 * layouts; Ctrl+X/Ctrl+Z are deliberately NOT treated as abort here,
 * per an earlier explicit user decision for this utility). Consumes
 * the key if one is pending. Call periodically from any long-running
 * scan loop (Phase 3 directory walk, Phase 4 lost-cluster sweep) --
 * works identically with or without /F, since even a read-only scan
 * on a large or badly damaged volume can run long enough that the
 * user needs a way out. Returns non-zero if the user wants to abort;
 * the caller does its own phase-specific cleanup (flush verbose dots,
 * close any open batch, print a message) before returning -1 up the
 * call chain the same way an I/O error would. */
int fix_poll_abort(void);

#endif /* CHKDSK_FIX_H */
