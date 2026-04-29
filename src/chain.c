/*
 * chain.c -- cluster/LBA helpers + cached FAT-entry reader.
 * See chain.h for the contract.
 */

#include <sprinter.h>
#include "vol.h"
#include "diskio.h"
#include "diskio_dss.h"
#include "sectbuf.h"
#include "chain.h"

/* Sentinel meaning "no FAT sector cached". FatFs LBA_t is DWORD, so
 * 0xFFFFFFFF can safely act as a not-a-real-sector marker. */
#define CHAIN_NO_CACHE 0xFFFFFFFFul

static LBA_t g_cached_sect = CHAIN_NO_CACHE;

void chain_invalidate(void)
{
    g_cached_sect = CHAIN_NO_CACHE;
}

LBA_t chain_cluster_to_lba(vol_t *fs, DWORD clust)
{
    return (LBA_t)(fs->database + (LBA_t)(clust - 2ul) * (LBA_t)fs->csize);
}

/* Returns 1 on success, 0 on read error. The buffer is g_sect_a. */
static u8 load_fat_sector(LBA_t fat_sect)
{
    if (g_cached_sect == fat_sect) return 1u;
    if (disk_read(0u, g_sect_a, fat_sect, 1u) != RES_OK) {
        g_cached_sect = CHAIN_NO_CACHE;
        return 0u;
    }
    g_cached_sect = fat_sect;
    return 1u;
}

DWORD chain_get_entry(vol_t *fs, DWORD clust)
{
    DWORD fat_byte_off;
    LBA_t fat_sect;
    u16   in_off;
    u8    b0, b1, b2, b3;

    if (clust >= fs->n_fatent) return CHAIN_READ_ERROR;

    switch (fs->fs_type) {
    case FS_FAT12:
        /* 1.5 bytes per entry. Entry may straddle a sector boundary. */
        fat_byte_off = clust + (clust >> 1);
        fat_sect     = (LBA_t)(fs->fatbase + (fat_byte_off >> 9));
        in_off       = (u16)(fat_byte_off & 0x1FFu);
        if (!load_fat_sector(fat_sect)) return CHAIN_READ_ERROR;
        b0 = g_sect_a[in_off];
        if (in_off == 511u) {
            if (!load_fat_sector(fat_sect + 1u)) return CHAIN_READ_ERROR;
            b1 = g_sect_a[0];
        } else {
            b1 = g_sect_a[in_off + 1u];
        }
        if (clust & 1ul) {
            return ((DWORD)b1 << 4) | (DWORD)(b0 >> 4);
        }
        return ((DWORD)(b1 & 0x0Fu) << 8) | (DWORD)b0;

    case FS_FAT16:
        fat_byte_off = clust << 1;
        fat_sect     = (LBA_t)(fs->fatbase + (fat_byte_off >> 9));
        in_off       = (u16)(fat_byte_off & 0x1FFu);
        if (!load_fat_sector(fat_sect)) return CHAIN_READ_ERROR;
        b0 = g_sect_a[in_off];
        b1 = g_sect_a[in_off + 1u];
        return ((DWORD)b1 << 8) | (DWORD)b0;

    case FS_FAT32:
        fat_byte_off = clust << 2;
        fat_sect     = (LBA_t)(fs->fatbase + (fat_byte_off >> 9));
        in_off       = (u16)(fat_byte_off & 0x1FFu);
        if (!load_fat_sector(fat_sect)) return CHAIN_READ_ERROR;
        b0 = g_sect_a[in_off];
        b1 = g_sect_a[in_off + 1u];
        b2 = g_sect_a[in_off + 2u];
        b3 = g_sect_a[in_off + 3u] & 0x0Fu;   /* FAT32 entries are 28-bit */
        return ((DWORD)b3 << 24) | ((DWORD)b2 << 16) | ((DWORD)b1 << 8) | (DWORD)b0;

    default:
        return CHAIN_READ_ERROR;
    }
}

u8 chain_is_bad(vol_t *fs, DWORD val)
{
    switch (fs->fs_type) {
    case FS_FAT12: return val == 0x0FF7ul ? 1u : 0u;
    case FS_FAT16: return val == 0xFFF7ul ? 1u : 0u;
    case FS_FAT32: return val == 0x0FFFFFF7ul ? 1u : 0u;
    default:       return 0u;
    }
}

u8 chain_is_eoc(vol_t *fs, DWORD val)
{
    switch (fs->fs_type) {
    case FS_FAT12: return val >= 0x0FF8ul     ? 1u : 0u;
    case FS_FAT16: return val >= 0xFFF8ul     ? 1u : 0u;
    case FS_FAT32: return val >= 0x0FFFFFF8ul ? 1u : 0u;
    default:       return 1u;
    }
}
