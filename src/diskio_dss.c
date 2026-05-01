/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * diskio_dss.c -- glue layer over Sprinter BIOS sector I/O.
 *
 * Sector I/O is delegated to the SDK wrappers `bios_drv_read` and
 * `bios_drv_write` (see <sprinter/bios.h>), which issue DRV_READ /
 * DRV_WRITE (RST #08, C = 0x55 / 0x56). This module's job is just
 * to apply the configured device + partition offset and adapt the
 * 0/error-code return into the FatFs-style RES_OK / RES_ERROR.
 */

#include <sprinter.h>
#include "vol.h"
#include "diskio.h"
#include "diskio_dss.h"

/* Set by volume_apply() before vol_mount. Default 0xFF means uninitialised. */
static u8  g_dev_disk    = 0xFFu;
static u16 g_dev_desc    = 0u;
static u32 g_part_offset = 0ul;

/* Last BIOS error code captured from a failing bios_drv_*. */
static u8  g_bios_err    = 0u;

void diskio_dss_set_device(u8 disk_num, u16 desc_addr)
{
    g_dev_disk = disk_num;
    g_dev_desc = desc_addr;
}

void diskio_dss_set_partition_offset(unsigned long lba)
{
    g_part_offset = (u32)lba;
}

u8 diskio_dss_last_error(void)
{
    return g_bios_err;
}

/* ===== FatFs-style API ===== */

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0u) return STA_NOINIT;
    if (g_dev_disk == 0xFFu) return STA_NOINIT;
    g_bios_err = 0u;
    return 0;
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0u) return STA_NOINIT;
    if (g_dev_disk == 0xFFu) return STA_NOINIT;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buf, LBA_t sector, UINT count)
{
    UINT i;

    if (pdrv != 0u || g_dev_disk == 0xFFu) return RES_NOTRDY;
    if (count == 0u) return RES_PARERR;

    for (i = 0u; i < count; i++) {
        u8 err = bios_drv_read(g_dev_disk,
                                (u32)sector + (u32)i + g_part_offset,
                                buf + (i * 512u),
                                1u);
        if (err != 0u) {
            g_bios_err = err;
            return RES_ERROR;
        }
    }
    return RES_OK;
}

u8 diskio_dss_read_batch(unsigned long lba, u8 count, u8 *dst)
{
    u8 err;

    if (g_dev_disk == 0xFFu) return 1u;
    if (count == 0u) return 1u;

    err = bios_drv_read(g_dev_disk, (u32)lba + g_part_offset, dst, count);
    if (err != 0u) {
        g_bios_err = err;
        return 1u;
    }
    return 0u;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT count)
{
    UINT i;

    if (pdrv != 0u || g_dev_disk == 0xFFu) return RES_NOTRDY;
    if (count == 0u) return RES_PARERR;

    for (i = 0u; i < count; i++) {
        u8 err = bios_drv_write(g_dev_disk,
                                 (u32)sector + (u32)i + g_part_offset,
                                 buf + (i * 512u),
                                 1u);
        if (err != 0u) {
            g_bios_err = err;
            return RES_ERROR;
        }
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0u || g_dev_disk == 0xFFu) return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;

    case GET_SECTOR_SIZE:
        if (buff) *(WORD *)buff = 512u;
        return RES_OK;

    case GET_SECTOR_COUNT:
        if (buff) *(LBA_t *)buff = 0xFFFFFFFFul;
        return RES_OK;

    case GET_BLOCK_SIZE:
        if (buff) *(DWORD *)buff = 1u;
        return RES_OK;

    default:
        return RES_PARERR;
    }
}
