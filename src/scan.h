/*
 * scan.h -- Phase 3 directory + FAT-chain scanner, Phase 4 lost-
 * cluster sweep.
 *
 * scan_run walks every directory entry starting from the root,
 * classifies each file/dir's cluster chain, cross-checks against the
 * FAT (lost clusters, cross-links, truncated chains, etc.), and
 * tracks used clusters in a DSS-page bitmap. After the directory
 * walk it runs Phase 4 -- a full FAT sweep that uses the bitmap to
 * find clusters in use but unreferenced from any directory entry.
 */

#ifndef CHKDSK_SCAN_H
#define CHKDSK_SCAN_H

#include "vol.h"

/* Returns the number of issues found (0 = clean). Negative on a
 * fatal error like out-of-memory for the bitmap. */
int scan_run(vol_t *fs);

#endif /* CHKDSK_SCAN_H */
