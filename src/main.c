/*
 * main.c -- CHKDSK stage 0 entry point.
 *
 * Stage 0 scope:
 *   - parse a single argument "<drive>:" (or "/?" for help)
 *   - resolve the drive letter to a physical device + partition
 *   - mount via FatFs and print the volume summary
 *   - allocate + release a bitmap to validate page memory machinery
 *
 * No integrity checks yet; those start at stage 1.
 */

#include <stdio.h>
#include <string.h>
#include <sprinter.h>
#include "ff.h"
#include "diskio.h"
#include "cmdline.h"
#include "diskio_dss.h"
#include "volume.h"
#include "bitmap.h"
#include "summary.h"
#include "bpb.h"

extern char *_utoa32(unsigned long val, char *end, int base, int upper);

#ifndef CHKDISK_VERSION
#define CHKDISK_VERSION "0.0.dev"
#endif

static void print_banner(void)
{
    printf("checkdsk " CHKDISK_VERSION " for Sprinter DSS\r\n");
}

static void print_usage(void)
{
    print_banner();
    printf("Usage: CHKDSK <drive>:\r\n");
    printf("  <drive>: A..J on stage 0 (A,B = floppy; C..F = IDE0 master\r\n");
    printf("           partitions 1..4; G..J = IDE0 slave 1..4)\r\n");
    printf("  /?       this help\r\n");
}

static u8 dispatch(char letter)
{
    volume_t vol;
    FATFS    fs;
    FRESULT  rc;
    int      vrc;
    int      phase1_errs;
    int      mount_ok;
    u32      partition_lba;
    char     path[3];

    phase1_errs = 0;
    mount_ok    = 0;

    vrc = volume_resolve(letter, &vol);
    if (vrc == VOL_ERR_BAD_LETTER) {
        printf("checkdsk: invalid drive letter '%c'\r\n", letter);
        return 2u;
    }
    if (vrc == VOL_ERR_UNSUPPORTED) {
        printf("checkdsk: drive '%c:' not supported on stage 0 (A..J only)\r\n",
               letter);
        return 2u;
    }

    volume_apply(&vol);
    partition_lba = vol.partition_lba;

    /* Phase 1: boot sector and BPB integrity check. Read-only.
     * Runs first so we still get diagnostic output for unmountable
     * volumes (like an unformatted IDE partition). */
    phase1_errs = bpb_check((LBA_t)partition_lba);

    /* FatFs mount + summary. If Phase 1 found problems, mount may
     * still succeed (FatFs is permissive) or fail; either is fine. */
    path[0] = '0';
    path[1] = ':';
    path[2] = '\0';

    memset(&fs, 0, sizeof(fs));
    rc = f_mount(&fs, path, 1);
    if (rc == FR_OK) {
        mount_ok = 1;
        if (summary_print(&fs, letter) != 0) {
            f_unmount(path);
            return 1u;
        }
    } else {
        printf("checkdsk: f_mount rc=%u, bios_err=%u (summary skipped)\r\n",
               (unsigned int)rc,
               (unsigned int)diskio_dss_last_error());
    }

    /* Stage 0 acceptance: confirm bitmap allocator works above 48KB.
     * Skip if mount failed -- without n_fatent we have no cluster count.
     * For a real volume we will base the size on cluster count; here
     * we use the actual cluster count of the mounted volume. */
    if (mount_ok) {
        u32 num_clusters = (u32)(fs.n_fatent - 2u);
        if (num_clusters > 0u) {
            if (!bitmap_init(num_clusters)) {
                printf("checkdsk: bitmap_init: out of page memory\r\n");
                f_unmount(path);
                return 1u;
            }
            bitmap_set(0u);
            bitmap_set(num_clusters - 1u);
            if (bitmap_get(0u) != 1u || bitmap_get(num_clusters - 1u) != 1u) {
                printf("checkdsk: bitmap self-test failed\r\n");
                bitmap_release();
                f_unmount(path);
                return 1u;
            }
            bitmap_release();
        }
        f_unmount(path);
    }

    return phase1_errs > 0 ? 1u : 0u;
}

void main(void)
{
    char  cmdbuf[CHKDSK_MAX_CMDLINE];
    char *argv[CHKDSK_MAX_ARGV];
    int   argc;
    char  letter;
    u8    code;

    cmd_read_safe(cmdbuf, (int)sizeof(cmdbuf));
    argc = cmd_parse(cmdbuf, argv);

    if (argc == 0) {
        print_usage();
        dss_exit(2u);
        return;
    }
    if (cmd_strieq(argv[0], "/?") || cmd_strieq(argv[0], "-h") ||
        cmd_strieq(argv[0], "--help")) {
        print_usage();
        dss_exit(0u);
        return;
    }

    /* Expect "X:" -- letter followed by colon. */
    if (argv[0][0] == '\0' || argv[0][1] != ':' || argv[0][2] != '\0') {
        printf("checkdsk: argument must be a single drive like \"C:\"\r\n");
        dss_exit(2u);
        return;
    }

    letter = argv[0][0];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - ('a' - 'A'));
    print_banner();
    code = dispatch(letter);
    dss_exit(code);
}
