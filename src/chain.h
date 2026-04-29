/*
 * chain.h -- cluster <-> LBA arithmetic and random-access FAT-entry
 * reader used by Phase 3 (directory walk + FAT-chain check).
 *
 * Phase 2 walks FAT sectors linearly via the batch reader. Phase 3
 * needs the opposite shape: "give me the next-cluster value for
 * cluster N" -- random access into the FAT, called once per cluster
 * in every directory and file. That access is wrapped here, with a
 * single-sector cache so consecutive entries in the same FAT sector
 * do not re-issue disk_read.
 *
 * The cache lives in g_sect_a (sectbuf), so any caller that issues
 * its own disk_read into g_sect_a between two chain_get_entry calls
 * MUST invoke chain_invalidate() afterwards.
 *
 * Special return values from chain_get_entry mirror FatFs categories:
 *   0x00000000               free
 *   0x00000002 .. n_fatent-1 valid next-cluster
 *   0x0FFFFFF7 / 0xFFF7      BAD (FAT12/16 mapped to 0x0FFFFFF7 by caller)
 *   >= 0x0FFFFFF8            EOC
 *   0xFFFFFFFFul             read error (also reflected in chain_last_error())
 *
 * For uniformity, chain_get_entry zero-extends FAT12/16 values; callers
 * use chain_is_eoc / chain_is_bad helpers to classify regardless of
 * underlying width.
 */

#ifndef CHKDSK_CHAIN_H
#define CHKDSK_CHAIN_H

#include "vol.h"

#define CHAIN_READ_ERROR 0xFFFFFFFFul

/* FatFs-relative LBA of the first sector of cluster `clust`. Caller
 * must ensure clust >= 2 && clust < fs->n_fatent. */
LBA_t chain_cluster_to_lba(vol_t *fs, DWORD clust);

/* Read the FAT entry for `clust`. Always returns a 32-bit value with
 * unused upper bits zeroed. Returns CHAIN_READ_ERROR on disk failure. */
DWORD chain_get_entry(vol_t *fs, DWORD clust);

/* Drop the cached FAT sector. Call after any external disk_read that
 * overwrites g_sect_a. */
void  chain_invalidate(void);

/* BIOS error code from the last failing chain_get_entry, 0 if none. */
u8    chain_last_error(void);

/* Classify a value returned by chain_get_entry, taking the FS type
 * into account so FAT12 0xFF7 / FAT16 0xFFF7 / FAT32 0x0FFFFFF7 all
 * read as bad / EOC. clust is the value, not the index. */
u8 chain_is_bad(vol_t *fs, DWORD val);
u8 chain_is_eoc(vol_t *fs, DWORD val);

#endif /* CHKDSK_CHAIN_H */
