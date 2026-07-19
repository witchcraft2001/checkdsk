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

/* CP866 lowercase Cyrillic, mirroring Estex-DSS's own UPPER routine
 * (DOS_Proc.asm, ~209-230): a-p (0xA0-0xAF), r-ya (0xE0-0xEF), yo
 * (0xF1). DSS folds these to uppercase on every name-based CREATE /
 * RENAME, so a raw SFN byte sitting in one of these ranges can only
 * have reached disk through a tool that bypassed DSS's own name
 * pipeline (e.g. a raw archive-extraction write) -- and DSS's own
 * DELETE / FIND can then never match it again, because the search
 * pattern gets folded to uppercase before the exact-byte compare
 * while the stored byte stays lowercase. Uppercase Cyrillic
 * (0x80-0x9F, 0xF0) and non-letter high bytes (0xB0-0xDF pseudo-
 * graphics, 0xF2-0xFF) round-trip fine and are legitimate -- treated
 * the same as any other byte, not touched here. */
static int is_dss_lower_cyrillic(BYTE c)
{
    return (c >= 0xA0u && c <= 0xAFu)
        || (c >= 0xE0u && c <= 0xEFu)
        || (c == 0xF1u);
}

/* Case-fold one SFN byte the same way DSS's own UPPER routine does:
 * ASCII a-z, or the CP866 lowercase-Cyrillic ranges above. Anything
 * else is returned unchanged. */
static BYTE dss_upper(BYTE c)
{
    if (c >= 'a' && c <= 'z')     return (BYTE)(c - ('a' - 'A'));
    if (c >= 0xA0u && c <= 0xAFu) return (BYTE)(c - 0x20u);
    if (c >= 0xE0u && c <= 0xEFu) return (BYTE)(c - 0x50u);
    if (c == 0xF1u)               return 0xF0u;
    return c;
}

/* Standard FAT LFN checksum of the 11 raw SFN bytes: an 8-bit
 * rotate-right accumulator plus each name byte in turn. Every LFN slot
 * of a group carries this value at offset 13, so scan.c can cross-check
 * a group against the SFN it precedes, and the name-sanitize repair can
 * recompute it after rewriting the SFN. */
BYTE dirent_sfn_checksum(const BYTE *e)
{
    BYTE sum = 0u;
    UINT i;
    for (i = 0u; i < 11u; i++)
        sum = (BYTE)(((sum >> 1) | (sum << 7)) + e[i]);
    return sum;
}

/* Chars forbidden in an LFN name (a smaller set than SFN -- LFN allows
 * '+ , ; = [ ] .' and spaces). Control chars (< 0x20) are handled
 * separately by the caller since 0x0000 doubles as the name terminator. */
static int is_forbidden_lfn_char(WORD c)
{
    switch (c) {
    case 0x22: case 0x2A: case 0x2F: case 0x3A:   /* " * / : */
    case 0x3C: case 0x3E: case 0x3F: case 0x5C:   /* < > ? \ */
    case 0x7C:                                    /* | */
        return 1;
    default:
        return 0;
    }
}

int dirent_is_current_lfn(const BYTE *e)
{
    return ((e[11] & 0x3Fu) == ATTR_LFN && e[0] != 0xE5u
            && e[12] == 0u) ? 1 : 0;
}

#define ld_dword_le(p) vol_ld_d(p)
#define ld_word_le(p)  vol_ld_w(p)

/* FAT packed date: bits 15-9 year-1980, 8-5 month, 4-0 day.
 * FAT packed time: bits 15-11 hour, 10-5 minute, 4-0 seconds/2. */
static int date_ok(WORD d)
{
    static const BYTE dim[12] =
        { 31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u };
    UINT m, day, lim;

    if (d == 0u) return 1;               /* "not set" -- see dirent.h */

    m   = (UINT)((d >> 5) & 0x0Fu);
    day = (UINT)(d & 0x1Fu);
    if (m == 0u || m > 12u) return 0;

    lim = (UINT)dim[m - 1u];
    if (m == 2u) {
        /* The 7-bit year field spans 1980..2107, so 2100 is the only
         * century year it can express -- which lets the /4 //100 //400
         * rule collapse to one extra comparison and keeps 16-bit modulo
         * (a called routine on z80) out of the entry-validation path. */
        UINT y = 1980u + (UINT)(d >> 9);
        if ((y & 3u) == 0u && y != 2100u) lim = 29u;
    }
    return (day >= 1u && day <= lim);
}

static int time_ok(WORD t)
{
    if ((UINT)(t >> 11)          > 23u) return 0;
    if ((UINT)((t >> 5) & 0x3Fu) > 59u) return 0;
    if ((UINT)(t & 0x1Fu)        > 29u) return 0;   /* seconds/2 */
    return 1;
}

UINT dirent_bad_timestamp_mask(const BYTE *e)
{
    UINT m = 0u;
    if (!time_ok(ld_word_le(&e[14]))) m |= DE_TS_CRT_TIME;
    if (!date_ok(ld_word_le(&e[16]))) m |= DE_TS_CRT_DATE;
    if (!date_ok(ld_word_le(&e[18]))) m |= DE_TS_ACC_DATE;
    if (!time_ok(ld_word_le(&e[22]))) m |= DE_TS_WRT_TIME;
    if (!date_ok(ld_word_le(&e[24]))) m |= DE_TS_WRT_DATE;
    return m;
}

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

    /* LFN slot: current type-0 structural and character checks. The
     * cross-slot sequence and SFN-checksum match need running group
     * state and are done in scan.c. */
    if ((attr & 0x3Fu) == ATTR_LFN) {
        BYTE ord_raw;
        BYTE ord;
        BYTE is_last;
        BYTE saw_nul;
        BYTE chars;

        /* Non-zero LDIR_Type denotes a reserved/future dirent type, not
         * a malformed current long-name component. FAT maintenance
         * utilities must preserve it without interpreting its remaining
         * fields using the type-0 layout. */
        if (e[12] != 0u) return 0u;

        /* UCS-2 name chars sit at offsets 1-10, 14-25, 28-31 (13 chars,
         * 2 bytes LE each). A non-final slot must contain 13 real chars.
         * The LAST slot contains either 13 real chars (an exact multiple)
         * or >=1 real char, 0x0000, then only 0xFFFF padding. */
        static const BYTE name_off[13] =
            { 1u, 3u, 5u, 7u, 9u, 14u, 16u, 18u, 20u, 22u, 24u, 28u, 30u };

        ord_raw = e[0];
        ord     = (BYTE)(ord_raw & 0x3Fu);
        is_last = (BYTE)(ord_raw & 0x40u);
        if ((ord_raw & 0x80u) != 0u || ord == 0u || ord > 20u)
            flags |= DE_LFN_BAD;
        if (e[26] != 0u || e[27] != 0u)   flags |= DE_LFN_BAD;

        saw_nul = 0u;
        chars   = 0u;
        for (i = 0u; i < 13u; i++) {
            WORD c = (WORD)((WORD)e[name_off[i]] | ((WORD)e[name_off[i] + 1u] << 8));
            if (saw_nul) {
                if (c != 0xFFFFu) flags |= DE_LFN_BAD;
                continue;
            }
            if (c == 0x0000u) {
                /* Empty tail or a terminator in a non-LAST slot would
                 * encode a shorter name than the ordinal sequence says. */
                if (!is_last || chars == 0u) flags |= DE_LFN_BAD;
                saw_nul = 1u;
                continue;
            }
            if (c == 0xFFFFu) {
                flags |= DE_LFN_BAD;                 /* padding before NUL */
                continue;
            }
            if (c < 0x0020u || is_forbidden_lfn_char(c)) {
                flags |= DE_LFN_CHAR;
            }
            chars++;
        }
        /* 20 slots can hold at most 255 chars: 19*13 + 8. */
        if (is_last && ord == 20u && chars > 8u) flags |= DE_LFN_BAD;
        return flags;
    }

    /* SFN entry. */
    if (e[0] == 0x20u) flags |= DE_NAME_LEAD_SPACE;

    for (i = 0u; i < 11u; i++) {
        BYTE c = e[i];
        if (i == 0u && c == 0x05u) continue; /* KANJI 0xE5 escape */
        if (c == 0x20u) continue;            /* padding */
        if ((c >= 'a' && c <= 'z') || is_dss_lower_cyrillic(c))
            flags |= DE_NAME_LOWERCASE;
        if (is_forbidden_sfn_char(c)) flags |= DE_NAME_BAD_CHAR;
    }

    if (attr & 0xC0u) flags |= DE_ATTR_RESERVED;

    /* DIR_NTRes (0x0C) -- only bits 3 and 4 (lowercase basename / ext)
     * are defined; the rest must be zero. CrtTimeTenth (0x0D) ranges
     * 0..199 (tens of milliseconds within a 2-second wrt_time slot).
     * Out-of-range values are RTC corruption or random garbage; both
     * map to a single repair (zero NTRES non-case bits, zero tenths). */
    if ((e[12] & ~0x18u) != 0u) flags |= DE_NTRES_RSV;
    if (e[13] > 199u)           flags |= DE_NTRES_RSV;

    /* Timestamp range validation. Restored 2026-07-19 after having been
     * dropped once for _CODE budget (the note that used to sit here
     * described that removal); the win0 extended layout has the room.
     * Checked here, in the SFN branch, because offsets 14..25 hold name
     * characters on an LFN slot. */
    if (dirent_bad_timestamp_mask(e) != 0u) flags |= DE_BAD_TIMESTAMP;

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

void dirent_sanitize_name(const BYTE *e, BYTE *out)
{
    UINT i;
    for (i = 0u; i < 11u; i++) {
        BYTE c = e[i];
        if (i == 0u && c == 0x05u) { out[i] = c; continue; }   /* KANJI escape */
        if (i == 0u && c == 0x20u) { out[i] = (BYTE)'_'; continue; } /* lead space */
        if (c == 0x20u)            { out[i] = c; continue; }        /* padding */
        if ((c >= 'a' && c <= 'z') || is_dss_lower_cyrillic(c)) {
            out[i] = dss_upper(c);
            continue;
        }
        out[i] = is_forbidden_sfn_char(c) ? (BYTE)'_' : c;
    }
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
        { DE_NTRES_RSV,        " ntres" },
        { DE_LFN_CHAR,         " lfn-char" },
        { DE_BAD_TIMESTAMP,    " bad-time" }
    };
    UINT i;
    if (flags == 0u) return;
    for (i = 0u; i < sizeof(tbl)/sizeof(tbl[0]); i++) {
        if (flags & tbl[i].mask) prt_str(tbl[i].s);
    }
}
