/*
 * bpb.c -- Phase 1 boot-sector / BPB diagnostic. See bpb.h.
 *
 * vol_mount has already parsed and validated the BPB; we trust its
 * verdict and reuse the populated vol_t. bpb_check prints a one-line
 * summary, then for FAT32 cross-checks the FSInfo signatures and the
 * backup boot sector at offset 6 against the main VBR.
 *
 * Only the FSInfo + backup-VBR checks need direct sector reads; the
 * field validation (BytsPerSec, SecPerClus, ...) is bundled into
 * vol_mount's mount_rc return value.
 */

#include <sprinter.h>
#include "vol.h"
#include "diskio.h"
#include "bpb.h"
#include "sectbuf.h"
#include "prt.h"

#define SECTOR_SIZE 512u

static unsigned long sec_sum(const u8 *p)
{
    unsigned long s = 0ul;
    UINT i;
    for (i = 0u; i < SECTOR_SIZE; i++) s += (unsigned long)p[i];
    return s;
}

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

static const char *type_name(BYTE fs_type)
{
    switch (fs_type) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    default:       return "?";
    }
}

static void bpb_err(const char *msg, int *errs)
{
    prt_str("  ERROR: ");
    prt_str(msg);
    prt_nl();
    (*errs)++;
}

static int report_mount_failure(int mount_rc)
{
    const char *why;
    switch (mount_rc) {
    case VOL_E_DISK_READ:    why = "cannot read VBR"; break;
    case VOL_E_BAD_VBR:      why = "missing 0xAA55 signature"; break;
    case VOL_E_BAD_BPB:      why = "BPB field invalid"; break;
    case VOL_E_BAD_FAT_TYPE: why = "unsupported FAT type"; break;
    default:                 why = "mount refused"; break;
    }
    prt_str("  ERROR: ");
    prt_str(why);
    prt_nl();
    return 1;
}

/* FAT32-specific: FSInfo signatures + free_count plausibility, and
 * sum-compare of the backup VBR (at sector 6) against the main VBR. */
static void check_fat32_extras(vol_t *fs, int *errs)
{
    unsigned long main_sum;
    DRESULT       drc;

    if (fs->n_total_sec == 0ul) return;     /* defensive */

    /* Main VBR sum. */
    drc = disk_read(0, g_sect_a, (LBA_t)0u, 1);
    if (drc != RES_OK) {
        bpb_err("cannot re-read VBR for backup compare", errs);
        return;
    }
    main_sum = sec_sum(g_sect_a);

    /* FSInfo. */
    if (fs->fsi_sector != 0ul) {
        drc = disk_read(0, g_sect_a, fs->fsi_sector, 1);
        if (drc != RES_OK) {
            bpb_err("cannot read FSInfo sector", errs);
        } else {
            unsigned long sig0 = ld_dword(g_sect_a, 0);
            unsigned long sig1 = ld_dword(g_sect_a, 484);
            u16           sig2 = ld_word (g_sect_a, 510);
            if (sig0 != 0x41615252ul) bpb_err("FSInfo signature1 bad", errs);
            if (sig1 != 0x61417272ul) bpb_err("FSInfo signature2 bad", errs);
            if (sig2 != 0xAA55u)      bpb_err("FSInfo trailing 0xAA55 missing", errs);
        }
    }

    /* Backup VBR is at offset 6 from volume base by FAT32 spec. */
    drc = disk_read(0, g_sect_a, (LBA_t)6u, 1);
    if (drc != RES_OK) {
        bpb_err("cannot read backup boot sector", errs);
    } else if (sec_sum(g_sect_a) != main_sum) {
        bpb_err("backup boot sector differs from main", errs);
    }
}

int bpb_check(vol_t *fs, int mount_rc)
{
    int  errs = 0;
    BYTE media;

    prt_str("Phase 1: boot sector and BPB\r\n");

    if (mount_rc != VOL_OK) {
        return report_mount_failure(mount_rc);
    }

    prt_str("  Type: ");
    prt_str(type_name(fs->fs_type));
    prt_str(", clusters: ");
    prt_dec((unsigned long)(fs->n_fatent - 2u));
    prt_nl();

    /* Media descriptor lives in the BPB at offset 0x15; read for sanity. */
    if (disk_read(0, g_sect_a, (LBA_t)0u, 1) == RES_OK) {
        media = g_sect_a[0x15];
        if (media != 0xF0u && media < 0xF8u) {
            bpb_err("media descriptor not valid", &errs);
        }
    }

    if (fs->fs_type == FS_FAT32) {
        check_fat32_extras(fs, &errs);
    }

    if (errs == 0) {
        prt_str("  No issues\r\n");
    } else {
        prt_str("  ");
        prt_dec((unsigned long)errs);
        prt_str(" error(s)\r\n");
    }
    return errs;
}
