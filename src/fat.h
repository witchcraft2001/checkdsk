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

/* Returns the number of issues found (0 = clean). Errors and warnings
 * print to stdout in compact form. */
int fat_check(vol_t *fs);

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
#endif

#endif /* CHKDSK_FAT_H */
