/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * diskio_batch.h -- bulk sector reader backed by DSS page memory.
 *
 * Phase 2 (and later phases) walks the FAT / dir / data sectors
 * linearly. Reading them one sector at a time through the BIOS is
 * dominated by per-call overhead (~25 sec for a 1 GB FAT16 = 256
 * sectors). The batch reader allocates one or more DSS extended-
 * memory pages, maps a chosen page into WIN3 (0xC000), and asks
 * BIOS DRV_READ to drop the requested number of sectors directly
 * into that page in a single call. Cuts the per-call overhead by
 * the page size (32 sectors per 16 KB page).
 *
 * Lifecycle:
 *   diskio_batch_open(N)      -- alloc N pages, save WIN3 mapping.
 *   diskio_batch_read(...)    -- BIOS read into a chosen page.
 *   diskio_batch_map(idx)     -- map page idx to WIN3, return ptr.
 *   diskio_batch_close()      -- free pages, restore WIN3 mapping.
 *
 * Pages are 16 KB each (32 sectors of 512 bytes). LBAs are absolute;
 * batch_read does NOT add the partition offset, since the batch
 * machinery is used for raw scans above the FatFs abstraction.
 */

#ifndef CHKDSK_DISKIO_BATCH_H
#define CHKDSK_DISKIO_BATCH_H

#include <sprinter/types.h>

#define BATCH_PAGE_SIZE     16384u
#define BATCH_SECTORS_PER_PAGE  32u

/* Allocate `num_pages` extended-memory pages. Returns 1 on success,
 * 0 if DSS refused (out of page memory). */
int diskio_batch_open(u8 num_pages);

/* Free the block. Safe to call even if open failed. */
void diskio_batch_close(void);

/* Read `count` sectors (1..32) starting at FatFs-relative LBA `lba`
 * (the partition offset is added by disk_read internally) into page
 * `page_idx`. Maps the page into WIN3 first. Returns 1 on ok, 0 on
 * error (BIOS error code in diskio_dss_last_error()). */
int diskio_batch_read(unsigned long lba, u8 count, u8 page_idx);

/* Map page `page_idx` into WIN3 and return a pointer to its base
 * (0xC000). Subsequent reads of bytes [0..16383] reflect the page's
 * contents. Used to walk previously-read data. */
u8 *diskio_batch_map(u8 page_idx);

#endif /* CHKDSK_DISKIO_BATCH_H */
