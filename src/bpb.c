/*
 * bpb.c -- Phase 1 boot-sector / BPB validator.  See bpb.h.
 *
 * Memory: two 512-byte static buffers in BSS (1 KB). Avoids putting
 * extra pressure on the stack and keeps the routine independent of
 * any FATFS scratch buffer that other phases may be using.
 */

#include <stdio.h>
#include <sprinter.h>
#include "ff.h"
#include "diskio.h"
#include "bpb.h"

#define SECTOR_SIZE 512u

static BYTE g_main_sec[SECTOR_SIZE];
static BYTE g_back_sec[SECTOR_SIZE];

extern char *_utoa32(unsigned long val, char *end, int base, int upper);

static u16 ld_word(const BYTE *p, UINT off)
{
    return (u16)(p[off] | ((u16)p[off + 1] << 8));
}

static unsigned long ld_dword(const BYTE *p, UINT off)
{
    return ((unsigned long)p[off])
         | ((unsigned long)p[off + 1] << 8)
         | ((unsigned long)p[off + 2] << 16)
         | ((unsigned long)p[off + 3] << 24);
}

static int is_pow2_u8(u8 v)
{
    if (v == 0u) return 0;
    return ((v & (u8)(v - 1u)) == 0u) ? 1 : 0;
}

/* Return 1 if media descriptor is a known valid value. */
static int valid_media(u8 m)
{
    if (m == 0xF0u) return 1;
    if (m >= 0xF8u) return 1;   /* 0xF8..0xFF */
    return 0;
}

/* Print "  ERROR: ..." line. */
static void bpb_err(const char *msg)
{
    printf("  ERROR: %s\r\n", msg);
}

/* Print "  WARN: ..." line. */
static void bpb_warn(const char *msg)
{
    printf("  WARN: %s\r\n", msg);
}

int bpb_check(LBA_t volbase)
{
    DRESULT       drc;
    int           errs = 0;
    int           warns = 0;
    u16           sig;
    u16           bps;
    u8            spc;
    u16           rsv;
    u8            nfats;
    u16           nroot;
    u16           tot16;
    u8            media;
    u16           fsz16;
    unsigned long tot32;
    unsigned long fsz32;
    unsigned long total_sectors;
    unsigned long fat_size;
    unsigned long root_dir_sectors;
    unsigned long data_sectors;
    unsigned long count_clusters;
    int           is_fat32_layout;
    const char   *type_name;
    char          buf[12];
    UINT          i;

    printf("Phase 1: boot sector and BPB\r\n");

    drc = disk_read(0, g_main_sec, volbase, 1);
    if (drc != RES_OK) {
        bpb_err("cannot read VBR");
        return 1;
    }

    /* 1. Signature. */
    sig = ld_word(g_main_sec, 510);
    if (sig != 0xAA55u) {
        bpb_err("missing 0xAA55 signature at offset 510");
        errs++;
    }

    /* 2. BPB fields. */
    bps   = ld_word (g_main_sec, 0x0B);
    spc   = g_main_sec[0x0D];
    rsv   = ld_word (g_main_sec, 0x0E);
    nfats = g_main_sec[0x10];
    nroot = ld_word (g_main_sec, 0x11);
    tot16 = ld_word (g_main_sec, 0x13);
    media = g_main_sec[0x15];
    fsz16 = ld_word (g_main_sec, 0x16);
    tot32 = ld_dword(g_main_sec, 0x20);
    fsz32 = ld_dword(g_main_sec, 0x24);

    if (bps != 512u) {
        bpb_err("bytes_per_sector != 512");
        errs++;
    }
    if (!is_pow2_u8(spc) || spc > 128u) {
        bpb_err("sectors_per_cluster not power-of-2 in [1..128]");
        errs++;
    }
    if (rsv == 0u) {
        bpb_err("reserved_sector_count == 0");
        errs++;
    }
    if (nfats != 1u && nfats != 2u) {
        bpb_err("num_fats not 1 or 2");
        errs++;
    }
    if (!valid_media(media)) {
        bpb_err("media descriptor not valid");
        errs++;
    }

    /* 3. Compute total_sectors and FAT size, then derive cluster count. */
    total_sectors = (tot16 != 0u) ? (unsigned long)tot16 : tot32;
    is_fat32_layout = (fsz16 == 0u && nroot == 0u);
    fat_size = is_fat32_layout ? fsz32 : (unsigned long)fsz16;

    if (total_sectors == 0u) {
        bpb_err("total_sectors == 0 in both fields");
        errs++;
    }
    if (fat_size == 0u) {
        bpb_err("fat_size == 0");
        errs++;
    }

    /* root_dir_sectors: ((nroot * 32) + (bps - 1)) / bps, but bps==512 here.
     * For FAT32 nroot==0 so this is 0. */
    root_dir_sectors = (((unsigned long)nroot * 32u) + (SECTOR_SIZE - 1u)) / SECTOR_SIZE;
    if (total_sectors == 0u || fat_size == 0u || rsv == 0u || spc == 0u) {
        count_clusters = 0u;     /* unsafe to compute */
    } else {
        unsigned long resv = (unsigned long)rsv;
        unsigned long fats_total = (unsigned long)nfats * fat_size;
        unsigned long meta = resv + fats_total + root_dir_sectors;
        if (total_sectors <= meta) {
            bpb_err("total_sectors <= reserved+FATs+root");
            errs++;
            count_clusters = 0u;
        } else {
            data_sectors   = total_sectors - meta;
            count_clusters = data_sectors / spc;
        }
    }

    /* 4. FAT type vs cluster count. */
    if (count_clusters > 0u) {
        if (count_clusters < 4085u) {
            type_name = "FAT12";
        } else if (count_clusters < 65525u) {
            type_name = "FAT16";
        } else {
            type_name = "FAT32";
        }
        printf("  Type: %s, clusters: %s\r\n",
               type_name,
               _utoa32(count_clusters, buf + 11, 10, 0));

        /* Cross-check with layout signals. */
        if (count_clusters >= 65525u) {
            /* Should be FAT32 layout: nroot=0, fsz16=0, fsz32>0. */
            if (!is_fat32_layout) {
                bpb_err("cluster count says FAT32 but BPB has FAT12/16 layout");
                errs++;
            }
        } else {
            if (is_fat32_layout) {
                bpb_err("BPB has FAT32 layout but cluster count is < 65525");
                errs++;
            }
        }
    }

    /* 5. FAT32-specific: FSInfo, root cluster, backup boot. */
    if (count_clusters >= 65525u && is_fat32_layout) {
        u16 fsinfo_sec   = ld_word(g_main_sec, 0x30);
        u16 backup_sec   = ld_word(g_main_sec, 0x32);
        unsigned long root_clst = ld_dword(g_main_sec, 0x2C);

        if (root_clst < 2u) {
            bpb_err("FAT32 root cluster < 2");
            errs++;
        }

        /* FSInfo. */
        if (fsinfo_sec != 0u) {
            BYTE   *fi = g_back_sec;     /* reuse */
            DRESULT fr = disk_read(0, fi, volbase + (LBA_t)fsinfo_sec, 1);
            if (fr != RES_OK) {
                bpb_err("cannot read FSInfo sector");
                errs++;
            } else {
                unsigned long sig0 = ld_dword(fi, 0);
                unsigned long sig1 = ld_dword(fi, 484);
                u16           sig2 = ld_word (fi, 510);
                if (sig0 != 0x41615252ul) {
                    bpb_err("FSInfo signature1 (offset 0) bad");
                    errs++;
                }
                if (sig1 != 0x61417272ul) {
                    bpb_err("FSInfo signature2 (offset 484) bad");
                    errs++;
                }
                if (sig2 != 0xAA55u) {
                    bpb_err("FSInfo trailing 0xAA55 missing");
                    errs++;
                }
                {
                    unsigned long free_count = ld_dword(fi, 488);
                    unsigned long next_free  = ld_dword(fi, 492);
                    if (free_count == 0xFFFFFFFFul) {
                        bpb_warn("FSInfo free_count == 0xFFFFFFFF (needs recalc)");
                        warns++;
                    }
                    if (next_free == 0xFFFFFFFFul) {
                        bpb_warn("FSInfo next_free == 0xFFFFFFFF (needs recalc)");
                        warns++;
                    }
                }
            }
        }

        /* Backup boot sector. */
        if (backup_sec != 0u && backup_sec != 0xFFFFu) {
            DRESULT br = disk_read(0, g_back_sec, volbase + (LBA_t)backup_sec, 1);
            if (br != RES_OK) {
                bpb_err("cannot read backup boot sector");
                errs++;
            } else {
                int diff = 0;
                for (i = 0u; i < SECTOR_SIZE; i++) {
                    if (g_main_sec[i] != g_back_sec[i]) {
                        diff = 1;
                        break;
                    }
                }
                if (diff) {
                    bpb_err("backup boot sector differs from main");
                    errs++;
                }
            }
        }
    }

    if (errs == 0 && warns == 0) {
        printf("  No issues\r\n");
    } else {
        printf("  %u error(s), %u warning(s)\r\n",
               (unsigned int)errs,
               (unsigned int)warns);
    }
    return errs;
}
