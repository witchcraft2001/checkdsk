/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * dirent.c -- 32-byte directory-entry validator. See dirent.h.
 */

#include <sprinter.h>
#include "vol.h"
#include "prt.h"
#include "dirent.h"

#define ATTR_RO    0x01u
#define ATTR_HID   0x02u
#define ATTR_SYS   0x04u
#define ATTR_VOLID 0x08u
#define ATTR_DIR   0x10u
#define ATTR_LFN   0x0Fu

static int is_forbidden_sfn_char(BYTE c)
{
    if (c < 0x20u) return 1;       /* control */
    switch (c) {
    case 0x22:                     /* " */
    case 0x2A:                     /* * */
    case 0x2B:                     /* + */
    case 0x2C:                     /* , */
    case 0x2E:                     /* . */
    case 0x2F:                     /* / */
    case 0x3A:                     /* : */
    case 0x3B:                     /* ; */
    case 0x3C:                     /* < */
    case 0x3D:                     /* = */
    case 0x3E:                     /* > */
    case 0x3F:                     /* ? */
    case 0x5B:                     /* [ */
    case 0x5C:                     /* \ */
    case 0x5D:                     /* ] */
    case 0x7C:                     /* | */
        return 1;
    default:
        return 0;
    }
}

#define ld_dword_le(p) vol_ld_d(p)
#define ld_word_le(p)  vol_ld_w(p)

UINT dirent_validate(vol_t *fs, const BYTE *e)
{
    UINT  flags = 0u;
    BYTE  attr;
    UINT  i;
    DWORD first_clust;
    DWORD size;
    WORD  hi_clust;

    if (e[0] == 0xE5u) return 0u;

    attr = e[11];

    /* LFN slot: limited checks here, full sequence/checksum in 3.6. */
    if ((attr & 0x3Fu) == ATTR_LFN) {
        if (e[12] != 0u)                  flags |= DE_LFN_BAD;
        if (e[26] != 0u || e[27] != 0u)   flags |= DE_LFN_BAD;
        return flags;
    }

    /* SFN entry. */
    if (e[0] == 0x20u) flags |= DE_NAME_LEAD_SPACE;

    for (i = 0u; i < 11u; i++) {
        BYTE c = e[i];
        if (i == 0u && c == 0x05u) continue; /* KANJI 0xE5 escape */
        if (c == 0x20u) continue;            /* padding */
        if (c >= 'a' && c <= 'z') flags |= DE_NAME_LOWERCASE;
        if (is_forbidden_sfn_char(c)) flags |= DE_NAME_BAD_CHAR;
    }

    if (attr & 0xC0u) flags |= DE_ATTR_RESERVED;

    /* DIR_NTRes (0x0C) -- only bits 3 and 4 (lowercase basename / ext)
     * are defined; the rest must be zero. CrtTimeTenth (0x0D) ranges
     * 0..199 (tens of milliseconds within a 2-second wrt_time slot).
     * Out-of-range values are RTC corruption or random garbage; both
     * map to a single repair (zero NTRES non-case bits, zero tenths).
     *
     * Note: full timestamp range-validation (5 fields per entry) was
     * dropped in favour of /F /C and FSInfo recalc -- bad timestamps
     * affect display only, never disk integrity, while the dropped
     * code freed ~280 B of _CODE that the FAT32 FSInfo updater needs. */
    if ((e[12] & ~0x18u) != 0u) flags |= DE_NTRES_RSV;
    if (e[13] > 199u)           flags |= DE_NTRES_RSV;

    {
        BYTE *fc = (BYTE *)&first_clust;
        fc[0] = e[26]; fc[1] = e[27]; fc[2] = e[20]; fc[3] = e[21];
    }
    hi_clust = ld_word_le(&e[20]);
    size     = ld_dword_le(&e[28]);

    if (attr & ATTR_VOLID) {
        if (first_clust != 0ul) flags |= DE_VOL_NONZERO;
        if (size        != 0ul) flags |= DE_VOL_NONZERO;
    } else if (attr & ATTR_DIR) {
        if (size != 0ul) flags |= DE_DIR_NONZERO_SIZE;
        if (first_clust == 1ul ||
            (first_clust >= 2ul && first_clust >= fs->n_fatent)) {
            flags |= DE_CLUST_OOR;
        }
    } else {
        if (first_clust == 1ul ||
            (first_clust >= 2ul && first_clust >= fs->n_fatent)) {
            flags |= DE_CLUST_OOR;
        }
        if (size != 0ul && first_clust < 2ul) flags |= DE_CLUST_OOR;
    }

    if (fs->fs_type != FS_FAT32 && hi_clust != 0u) {
        flags |= DE_FAT16_HI_CLUST;
    }

    return flags;
}

void dirent_flags_print(UINT flags)
{
    /* Table-driven: one loop, ~30 B vs N if-prt_str pairs. */
    static const struct { UINT mask; const char *s; } tbl[] = {
        { DE_NAME_BAD_CHAR,    " bad-name" },
        { DE_NAME_LOWERCASE,   " lower" },
        { DE_NAME_LEAD_SPACE,  " lead-sp" },
        { DE_ATTR_RESERVED,    " attr-rsv" },
        { DE_DIR_NONZERO_SIZE, " dir-size" },
        { DE_VOL_NONZERO,      " vol-nz" },
        { DE_LFN_BAD,          " lfn-bad" },
        { DE_CLUST_OOR,        " clust-oor" },
        { DE_FAT16_HI_CLUST,   " fat16-hi" },
        { DE_NTRES_RSV,        " ntres" }
    };
    UINT i;
    if (flags == 0u) return;
    for (i = 0u; i < sizeof(tbl)/sizeof(tbl[0]); i++) {
        if (flags & tbl[i].mask) prt_str(tbl[i].s);
    }
}
