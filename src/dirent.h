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
 *   - current LFN slot ((attr & 0x3F) == 0x0F, type == 0): order,
 *     first-cluster word, UCS-2 terminator/padding and characters.
 *     LFN-sequence + checksum cross-checks are done by scan.c. A
 *     non-zero type is a reserved extension and is preserved/ignored.
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
#define DE_NAME_LOWERCASE    0x0002u  /* lowercase a-z, or CP866 lowercase
                                          Cyrillic (see dirent.c) in SFN */
#define DE_NAME_LEAD_SPACE   0x0004u  /* SFN starts with space */
#define DE_ATTR_RESERVED     0x0008u  /* attr bits 6 or 7 set */
#define DE_DIR_NONZERO_SIZE  0x0010u  /* directory entry size != 0 */
#define DE_VOL_NONZERO       0x0020u  /* volume label cluster or size != 0 */
#define DE_LFN_BAD           0x0040u  /* malformed current LFN slot: order,
                                          cluster word or name padding */
#define DE_CLUST_OOR         0x0080u  /* first cluster == 1 or >= n_fatent */
#define DE_FAT16_HI_CLUST    0x0100u  /* FAT12/16 with non-zero hi cluster */
#define DE_NTRES_RSV         0x0200u  /* NTRES has reserved bits or
                                          CrtTimeTenth > 199 */
#define DE_LFN_CHAR          0x0400u  /* LFN name holds a forbidden UCS-2
                                          char (0x0000-0x001F control or
                                          the / \ : * ? " < > | set) */
#define DE_BAD_TIMESTAMP     0x0800u  /* >=1 date/time field out of range;
                                          see dirent_bad_timestamp_mask */

/* Deliberately excludes DE_NAME_* and DE_BAD_TIMESTAMP: this mask also
 * gates directory descent, and neither a cosmetic name nor a nonsense
 * date is evidence that the cluster pointer is untrustworthy. */
#define DE_ANY_ERROR        (DE_NAME_BAD_CHAR    | DE_ATTR_RESERVED | \
                             DE_VOL_NONZERO      | DE_LFN_BAD       | \
                             DE_CLUST_OOR        | DE_FAT16_HI_CLUST | \
                             DE_DIR_NONZERO_SIZE | DE_NTRES_RSV      | \
                             DE_LFN_CHAR)

/* Which of the five timestamp fields failed range validation. Doubles
 * as the `value` argument of a FIX_DPATCH_TIMESTAMP patch, so only the
 * offending fields get reset and a good creation date survives a
 * garbage access date. */
#define DE_TS_CRT_TIME       0x01u  /* offset 14..15 */
#define DE_TS_CRT_DATE       0x02u  /* offset 16..17 */
#define DE_TS_ACC_DATE       0x04u  /* offset 18..19 */
#define DE_TS_WRT_TIME       0x08u  /* offset 22..23 */
#define DE_TS_WRT_DATE       0x10u  /* offset 24..25 */

/* Inspect the 32-byte entry `e` and return a bitmask of DE_* flags.
 * Returns 0 when the entry is clean (or is a deleted slot that we
 * skip by convention). */
UINT dirent_validate(vol_t *fs, const BYTE *e);

/* Print a space-separated list of flag tags ("bad-name", "clust-oor", ...).
 * Emits nothing if flags == 0. Does NOT print a leading or trailing space. */
void dirent_flags_print(UINT flags);

/* Build a spec-compliant 11-byte SFN name into out[11] from e's raw
 * name field: replaces a literal leading space and any forbidden
 * punctuation/control byte with '_', and case-folds any lowercase a-z
 * or CP866 lowercase Cyrillic byte to its uppercase equivalent (the
 * same fold Estex-DSS itself applies on every name-based CREATE /
 * RENAME -- see dirent.c). Padding spaces and the KANJI 0xE5-escape
 * byte are left untouched. Safe to call whenever dirent_validate
 * flagged DE_NAME_BAD_CHAR, DE_NAME_LOWERCASE or DE_NAME_LEAD_SPACE. */
void dirent_sanitize_name(const BYTE *e, BYTE *out);

/* Standard FAT LFN checksum over the 11 raw SFN name bytes of `e`.
 * Every LFN slot of a group stores this at offset 13; scan.c compares
 * a group's stored value against this, and the name-sanitize repair
 * recomputes it after rewriting the SFN so a preceding LFN run stays
 * valid instead of being deleted. */
BYTE dirent_sfn_checksum(const BYTE *e);

/* Bitmask of DE_TS_* for the date/time fields of `e` that are out of
 * range. 0 when every field is sane. Only meaningful on an SFN entry --
 * on an LFN slot those offsets hold name characters, so dirent_validate
 * calls this from the SFN branch only.
 *
 * An all-zero date word counts as SANE, not corrupt: CrtDate and
 * LstAccDate are optional per the FAT spec and a great many writers
 * leave them zero, so rejecting that would flag most real volumes. A
 * zero TIME word is a genuine 00:00:00 and needs no special case. */
UINT dirent_bad_timestamp_mask(const BYTE *e);

/* Non-zero when e is an active, current-format (LDIR_Type == 0) LFN
 * name component. Entries with the LFN attribute but a non-zero type
 * are reserved extensions: scanners must preserve/ignore them rather
 * than classify and delete them as corrupt current-format LFN slots. */
int dirent_is_current_lfn(const BYTE *e);

#endif /* CHKDSK_DIRENT_H */
