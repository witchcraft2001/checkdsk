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

#include "ff.h"

/* Returns the number of issues found (0 = clean). Errors and warnings
 * print to stdout in compact form. */
int fat_check(FATFS *fs);

#endif /* CHKDSK_FAT_H */
