/*
 * scan.h -- Phase 3 directory + FAT-chain scanner.
 *
 * Walks every directory entry starting from the root, classifies each
 * file/dir's cluster chain, and cross-checks against the FAT (lost
 * clusters, cross-links, truncated chains, etc.). Tracks used clusters
 * in a DSS-page bitmap so a later FAT scan can spot unreferenced
 * allocations.
 *
 * Stage 3.1 is just the wiring stub: allocate the bitmap, exercise
 * the cluster/FAT helpers, free it. Subsequent stages fill in the
 * directory walker (3.2), entry validation (3.3), recursion (3.4),
 * chain validation + lost cluster reporting (3.5), and LFN checks
 * (3.6).
 */

#ifndef CHKDSK_SCAN_H
#define CHKDSK_SCAN_H

#include "ff.h"

/* Returns the number of issues found (0 = clean). Negative on a
 * fatal error like out-of-memory for the bitmap. */
int scan_run(FATFS *fs);

#endif /* CHKDSK_SCAN_H */
