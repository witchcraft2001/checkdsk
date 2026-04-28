/*
 * diskio_dss.c -- FatFs glue layer over Sprinter BIOS sector I/O.
 *
 * Sector I/O uses BIOS short read/write:
 *   DRV_READ  = 0x55  -- read N sectors
 *   DRV_WRITE = 0x56  -- write N sectors
 * (per Estex-DSS/Shared_Includes/constants/BIOS_equ.inc).
 *
 * NOTE: sdcc-sprinter-sdk/include/sprinter/bios.h:49-50 has the wrong
 * values for BIOS_DISK_READ / BIOS_DISK_WRITE -- those constants point
 * to DRV_WRITE_LONG (0x53) and DRV_VERIFY (0x54). The literal numbers
 * used below are correct; the SDK header bug is filed as a feature
 * request. See specs.md "Sector I/O" for the full discussion.
 *
 * Calling convention for BIOS short read/write (RST #08, C = func):
 *   In:  A     = disk number (high nibble = type)
 *        HL:IX = LBA (HL = high 16 bits, IX = low 16 bits)
 *        DE    = buffer address (main RAM)
 *        B     = sector count (0 means 256)
 *        C     = function code
 *   Out: CF    = 0 ok / 1 error
 *        A     = error code on failure
 *        HL:IX,DE updated past the transferred bytes.
 *
 * Single-sector reads/writes only for stage 0: each disk_read /
 * disk_write call loops one sector at a time. Multi-sector batching
 * is a stage 1 optimisation candidate.
 */

#include <sprinter.h>
#include "ff.h"
#include "diskio.h"
#include "diskio_dss.h"

/* Selected by volume.c before f_mount. Default 0xFF means uninitialised. */
static u8  g_dev_disk    = 0xFFu;
static u16 g_dev_desc    = 0u;

/* Globals consumed by the BIOS-call trampolines. */
static u8  g_bios_disk   = 0u;
static u16 g_bios_lba_lo = 0u;
static u16 g_bios_lba_hi = 0u;
static u16 g_bios_buf    = 0u;
static u8  g_bios_count  = 0u;
static u8  g_bios_err    = 0u;

void diskio_dss_set_device(u8 disk_num, u16 desc_addr)
{
    g_dev_disk = disk_num;
    g_dev_desc = desc_addr;
}

u8 diskio_dss_last_error(void)
{
    return g_bios_err;
}

/* RST #08 with C = 0x55 (DRV_READ).
 * Inputs from globals; returns 0 = ok, 1 = error (error code in g_bios_err). */
static u8 bios_drv_read(void) __naked
{
    __asm
        push    ix
        ld      a, (_g_bios_disk)
        ld      hl, (_g_bios_lba_hi)
        ld      ix, (_g_bios_lba_lo)
        ld      de, (_g_bios_buf)
        ld      a, (_g_bios_count)
        ld      b, a
        ld      a, (_g_bios_disk)
        ld      c, #0x55
        rst     #0x08
        jr      c, _bios_drv_read_err
        pop     ix
        ld      l, #0
        ret
    _bios_drv_read_err:
        ld      (_g_bios_err), a
        pop     ix
        ld      l, #1
        ret
    __endasm;
}

/* RST #08 with C = 0x56 (DRV_WRITE). Same shape as bios_drv_read. */
static u8 bios_drv_write(void) __naked
{
    __asm
        push    ix
        ld      a, (_g_bios_disk)
        ld      hl, (_g_bios_lba_hi)
        ld      ix, (_g_bios_lba_lo)
        ld      de, (_g_bios_buf)
        ld      a, (_g_bios_count)
        ld      b, a
        ld      a, (_g_bios_disk)
        ld      c, #0x56
        rst     #0x08
        jr      c, _bios_drv_write_err
        pop     ix
        ld      l, #0
        ret
    _bios_drv_write_err:
        ld      (_g_bios_err), a
        pop     ix
        ld      l, #1
        ret
    __endasm;
}

/* ===== FatFs API ===== */

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

    g_bios_disk  = g_dev_disk;
    g_bios_count = 1u;
    for (i = 0u; i < count; i++) {
        u32 lba = (u32)sector + (u32)i;
        g_bios_lba_lo = (u16)(lba & 0xFFFFu);
        g_bios_lba_hi = (u16)((lba >> 16) & 0xFFFFu);
        g_bios_buf    = (u16)(buf + (i * 512u));
        if (bios_drv_read() != 0u) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT count)
{
    UINT i;

    if (pdrv != 0u || g_dev_disk == 0xFFu) return RES_NOTRDY;
    if (count == 0u) return RES_PARERR;

    g_bios_disk  = g_dev_disk;
    g_bios_count = 1u;
    for (i = 0u; i < count; i++) {
        u32 lba = (u32)sector + (u32)i;
        g_bios_lba_lo = (u16)(lba & 0xFFFFu);
        g_bios_lba_hi = (u16)((lba >> 16) & 0xFFFFu);
        g_bios_buf    = (u16)(buf + (i * 512u));
        if (bios_drv_write() != 0u) return RES_ERROR;
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
        /* Conservative upper bound. With FF_MULTI_PARTITION = 1 the
         * partition entry's size from the MBR governs the active
         * volume bounds, so this value is rarely consulted. */
        if (buff) *(LBA_t *)buff = 0xFFFFFFFFul;
        return RES_OK;

    case GET_BLOCK_SIZE:
        if (buff) *(DWORD *)buff = 1u;
        return RES_OK;

    default:
        return RES_PARERR;
    }
}

DWORD get_fattime(void)
{
    dss_date_t date;
    dss_time_t time;

    dss_getdate(&date);
    dss_gettime(&time);

    /* FAT timestamp:
     *   bit31..25 = year - 1980 (7 bits)
     *   bit24..21 = month (4 bits)
     *   bit20..16 = day (5 bits)
     *   bit15..11 = hour (5 bits)
     *   bit10..5  = minute (6 bits)
     *   bit4..0   = second/2 (5 bits) */
    if (date.year < 1980u) {
        return ((DWORD)0u << 25) | ((DWORD)1u << 21) | ((DWORD)1u << 16);
    }
    return ((DWORD)(date.year - 1980u) << 25)
         | ((DWORD)date.month         << 21)
         | ((DWORD)date.day           << 16)
         | ((DWORD)time.hour          << 11)
         | ((DWORD)time.minute        <<  5)
         | ((DWORD)(time.second >> 1));
}
