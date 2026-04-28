/*
 * diskio_dss.h -- internal API exposed by the FatFs glue layer.
 *
 * The FatFs disk_read / disk_write functions in diskio_dss.c address
 * a single physical device. volume.c selects the device before mount
 * by calling diskio_dss_set_device().
 */

#ifndef CHKDSK_DISKIO_DSS_H
#define CHKDSK_DISKIO_DSS_H

#include <sprinter/types.h>

/* Configure the physical device that subsequent disk_read/disk_write
 * calls will target.
 *
 *   disk_num: BIOS drive byte
 *               0x80 / 0x81 -- IDE0 master / slave
 *               0x82 / 0x83 -- IDE1 master / slave
 *               0x00 / 0x01 -- FDD0 / FDD1
 *
 *   desc_addr: address of the 8-byte device descriptor in system page,
 *              one of #C1C0/#C1C8/#C1D0/#C1D8/#C1E0/#C1E8 (kept for
 *              GET_SECTOR_COUNT computation in disk_ioctl).
 */
void diskio_dss_set_device(u8 disk_num, u16 desc_addr);

/* Last BIOS error code captured by the inline-asm trampolines.
 * Useful for diagnostics; FatFs only sees RES_ERROR / RES_OK. */
u8 diskio_dss_last_error(void);

/* LBA of the last sector that was attempted (read or write), split as
 * high and low 16-bit halves. Useful for narrowing down disk_err
 * failures during stage-0 bring-up. */
u16 diskio_dss_last_lba_hi(void);
u16 diskio_dss_last_lba_lo(void);

/* Same value as a 32-bit u32, for cross-checking the split halves. */
unsigned long diskio_dss_last_lba(void);

/* Raw sector argument FatFs handed to disk_read, captured before any
 * local (u16) conversion. Stage-0 only. */
unsigned long diskio_dss_dbg_raw_sector(void);

#endif /* CHKDSK_DISKIO_DSS_H */
