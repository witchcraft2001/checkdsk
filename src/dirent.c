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

static DWORD ld_dword_le(const BYTE *p)
{
    DWORD x;
    BYTE *xb = (BYTE *)&x;
    xb[0] = p[0]; xb[1] = p[1]; xb[2] = p[2]; xb[3] = p[3];
    return x;
}

static WORD ld_word_le(const BYTE *p)
{
    return (WORD)((WORD)p[0] | ((WORD)p[1] << 8));
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
    if (flags == 0u) return;
    if (flags & DE_NAME_BAD_CHAR)    prt_str(" bad-name");
    if (flags & DE_NAME_LOWERCASE)   prt_str(" lower");
    if (flags & DE_NAME_LEAD_SPACE)  prt_str(" lead-sp");
    if (flags & DE_ATTR_RESERVED)    prt_str(" attr-rsv");
    if (flags & DE_DIR_NONZERO_SIZE) prt_str(" dir-size");
    if (flags & DE_VOL_NONZERO)      prt_str(" vol-nz");
    if (flags & DE_LFN_BAD)          prt_str(" lfn-bad");
    if (flags & DE_CLUST_OOR)        prt_str(" clust-oor");
    if (flags & DE_FAT16_HI_CLUST)   prt_str(" fat16-hi");
}
