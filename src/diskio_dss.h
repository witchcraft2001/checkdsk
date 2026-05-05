/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
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

/* Set the LBA offset added to every sector argument FatFs passes to
 * disk_read / disk_write. FatFs is built with FF_MULTI_PARTITION = 0
 * so it sees each volume as a whole disk; volume.c installs the real
 * partition LBA here so the BIOS receives the absolute sector. */
void diskio_dss_set_partition_offset(unsigned long lba);

/* Last BIOS error code captured by the inline-asm trampolines.
 * Useful for diagnostics; FatFs only sees RES_ERROR / RES_OK. */
u8 diskio_dss_last_error(void);

/* Multi-sector batched read in a single BIOS DRV_READ call.
 * `lba` is FatFs-relative (partition offset is added). count is 1..255.
 * Returns 0 on success, 1 on error. Used by diskio_batch only. */
u8 diskio_dss_read_batch(unsigned long lba, u8 count, u8 *dst);

/* Re-issue BIOS DRV_DETECT (#57). Re-scans the IDE/FDD descriptors at
 * #C1C0..#C1E8 and forces DSS to drop its cached BPB / FAT / dir
 * buffers, so subsequent file-system access reads what we just wrote
 * during /F. Must be called once after the last write of a /F run,
 * before dss_exit. The SDK exposes the constant BIOS_DRV_DETECT but
 * provides no C wrapper; this trampoline supplies one. */
void diskio_dss_rescan(void);

#endif /* CHKDSK_DISKIO_DSS_H */
