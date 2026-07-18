/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * dirent.h -- 32-byte directory-entry validator.
 *
 * Produces a bitmask of issues for an entry returned by dirwalk_next.
 * The walker emits raw 32-byte slots (deleted/LFN/SFN) and this module
 * decides which checks apply:
 *
 *   - deleted (e[0] == 0xE5): no checks, returns 0.
 *   - LFN slot ((attr & 0x3F) == 0x0F): type byte / first-cluster word
 *     must be zero. LFN-sequence + checksum cross-checks are done by
 *     scan.c (it has the per-walker state to track the running group).
 *   - SFN: name character set, reserved attribute bits, attribute
 *     consistency (volume label, directory), first-cluster bounds,
 *     and -- on FAT12/16 -- the high cluster word at offset 20-21.
 *
 * Lowercase and stray space in SFN raise a flag bit like any other
 * check; the caller (scan.c) decides whether to fold them into its
 * own report/repair mask. They are deliberately left out of
 * DE_ANY_ERROR below because that mask also gates directory descent
 * (is_descendable_dir in scan.c) -- a lowercase or leading-space name
 * is cosmetic, not evidence of a corrupt cluster pointer, and must
 * not stop the walker from entering an otherwise-fine subdirectory.
 */

#ifndef CHKDSK_DIRENT_H
#define CHKDSK_DIRENT_H

#include "vol.h"

#define DE_NAME_BAD_CHAR     0x0001u  /* control or forbidden punctuation */
#define DE_NAME_LOWERCASE    0x0002u  /* lowercase letter in SFN */
#define DE_NAME_LEAD_SPACE   0x0004u  /* SFN starts with space */
#define DE_ATTR_RESERVED     0x0008u  /* attr bits 6 or 7 set */
#define DE_DIR_NONZERO_SIZE  0x0010u  /* directory entry size != 0 */
#define DE_VOL_NONZERO       0x0020u  /* volume label cluster or size != 0 */
#define DE_LFN_BAD           0x0040u  /* LFN type/cluster slot != 0 */
#define DE_CLUST_OOR         0x0080u  /* first cluster == 1 or >= n_fatent */
#define DE_FAT16_HI_CLUST    0x0100u  /* FAT12/16 with non-zero hi cluster */
#define DE_NTRES_RSV         0x0200u  /* NTRES has reserved bits or
                                          CrtTimeTenth > 199 */

#define DE_ANY_ERROR        (DE_NAME_BAD_CHAR    | DE_ATTR_RESERVED | \
                             DE_VOL_NONZERO      | DE_LFN_BAD       | \
                             DE_CLUST_OOR        | DE_FAT16_HI_CLUST | \
                             DE_DIR_NONZERO_SIZE | DE_NTRES_RSV)

/* Inspect the 32-byte entry `e` and return a bitmask of DE_* flags.
 * Returns 0 when the entry is clean (or is a deleted slot that we
 * skip by convention). */
UINT dirent_validate(vol_t *fs, const BYTE *e);

/* Print a space-separated list of flag tags ("bad-name", "clust-oor", ...).
 * Emits nothing if flags == 0. Does NOT print a leading or trailing space. */
void dirent_flags_print(UINT flags);

/* Build a spec-compliant 11-byte SFN name into out[11] from e's raw
 * name field: replaces a literal leading space, any forbidden
 * punctuation/control byte, and any literal lowercase a-z with '_' or
 * its uppercase equivalent. Padding spaces and the KANJI 0xE5-escape
 * byte are left untouched. Safe to call whenever dirent_validate
 * flagged DE_NAME_BAD_CHAR, DE_NAME_LOWERCASE or DE_NAME_LEAD_SPACE. */
void dirent_sanitize_name(const BYTE *e, BYTE *out);

#endif /* CHKDSK_DIRENT_H */
