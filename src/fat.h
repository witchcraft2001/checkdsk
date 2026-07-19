/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * fat.h -- Phase 2 FAT-table validator.
 *
 * Walks the primary FAT sector by sector; if n_fats == 2, reads the
 * matching sector of the secondary FAT in parallel and byte-compares.
 * Validates FAT[0] media descriptor, FAT[1] EOC + clean-shutdown /
 * hard-error flags (FAT16/32), and every cluster entry against the
 * (free / 2..max_cluster / BAD / EOC) categories. Anything else is
 * reported as an "invalid cluster reference".
 *
 * Stage 2 is read-only.
 */

#ifndef CHKDSK_FAT_H
#define CHKDSK_FAT_H

#include "vol.h"

/* Returns the number of issues found (0 = clean), or -1 if a FAT
 * sector read failed mid-scan (fatal -- the same FAT feeds Phase 3/4).
 * Errors and warnings print to stdout in compact form. */
int fat_check(vol_t *fs);

/* Free / bad cluster counts from the last fat_check, for the
 * end-of-run classic space report. Valid only after a fat_check that
 * returned >= 0; both read 0 before the first call or after a fatal
 * (-1) Phase 2. */
DWORD fat_free_clusters(void);
DWORD fat_bad_clusters(void);

#if CHKDSK_FAT32
/* On FAT32, mark FSI_Free_Count and FSI_Nxt_Free as "unknown"
 * (0xFFFFFFFF) in the FSInfo sector, so DSS recomputes them on next
 * mount. Called once at end of /F if any repair touched the FAT. The
 * spec calls for an explicit recompute, but writing the "unknown"
 * sentinel is FAT-spec-equivalent and avoids tracking deltas across
 * phase 2 (counts free) and phase 4 (frees more clusters). Skips
 * silently if FSInfo signatures are bad or the volume has no FSInfo
 * sector. Returns 1 on success or skip, 0 on I/O error. */
int fat_invalidate_fsinfo(vol_t *fs);

/* Non-zero when Phase 2 saw FSInfo's cached free_count disagree with
 * the actual FAT contents. Callers must invoke fat_invalidate_fsinfo
 * when this is set even if nothing else was repaired -- it is the one
 * case where a stale FSInfo is itself the reported issue. */
int fat_fsinfo_stale(void);
#endif

#endif /* CHKDSK_FAT_H */
