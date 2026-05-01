/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * diskio.h -- minimal block I/O interface used by chkdsk.
 *
 * Original code used the FatFs header by ChaN; we replaced FatFs's
 * core (ff.c) with mount.c + vol.h, and the residual typedefs are
 * now native to this project. The names DSTATUS / DRESULT and the
 * RES_, STA_, CTRL_, GET_ values are kept verbatim so the disk
 * I/O glue (diskio_dss.c) reads naturally to anyone familiar with
 * the FatFs interface, but no FatFs source is bundled.
 */

#ifndef CHKDSK_DISKIO_H
#define CHKDSK_DISKIO_H

#include "vol.h"

typedef BYTE DSTATUS;

typedef enum {
    RES_OK    = 0,
    RES_ERROR = 1,
    RES_WRPRT = 2,
    RES_NOTRDY = 3,
    RES_PARERR = 4
} DRESULT;

#define STA_NOINIT  0x01u

#define CTRL_SYNC          0
#define GET_SECTOR_COUNT   1
#define GET_SECTOR_SIZE    2
#define GET_BLOCK_SIZE     3

DSTATUS disk_initialize(BYTE pdrv);
DSTATUS disk_status(BYTE pdrv);
DRESULT disk_read(BYTE pdrv, BYTE *buf, LBA_t sector, UINT count);
DRESULT disk_write(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT count);
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff);

#endif /* CHKDSK_DISKIO_H */
