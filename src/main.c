/*
 * main.c -- CHKDSK entry point.
 *
 * Pipeline (per specs.md):
 *   - parse a single argument "<drive>:" (or "/?" for help)
 *   - resolve the drive letter to a physical device + partition
 *   - Phase 1 (bpb_check): validate the boot sector and BPB
 *   - if mount succeeds: print summary and run Phase 2 (fat_check)
 *   - return non-zero if either phase reported errors
 */

#include <string.h>
#include <sprinter.h>
#include "ff.h"
#include "diskio.h"
#include "cmdline.h"
#include "diskio_dss.h"
#include "volume.h"
#include "summary.h"
#include "bpb.h"
#include "fat.h"
#include "scan.h"
#include "prt.h"

#ifndef CHKDISK_VERSION
#define CHKDISK_VERSION "0.0.dev"
#endif

static void print_banner(void)
{
    prt_str("checkdsk " CHKDISK_VERSION " for Sprinter DSS\r\n");
}

static void print_usage(void)
{
    print_banner();
    prt_str("Usage: CHKDSK <drive>:\r\n");
    prt_str("  <drive>:  A, B (floppies) or C, D, ... (IDE partitions)\r\n");
    prt_str("  /?        this help\r\n");
}

static u8 dispatch(char letter)
{
    volume_t vol;
    FATFS    fs;
    FRESULT  rc;
    int      vrc;
    int      total_errs;
    int      mount_ok;
    char     path[3];

    total_errs = 0;
    mount_ok   = 0;

    vrc = volume_resolve(letter, &vol);
    if (vrc == VOL_ERR_BAD_LETTER) {
        prt_str("checkdsk: invalid drive letter '");
        prt_chr(letter);
        prt_str("'\r\n");
        return 2u;
    }
    if (vrc == VOL_ERR_UNSUPPORTED) {
        prt_str("checkdsk: drive '");
        prt_chr(letter);
        prt_str(":' not present\r\n");
        return 2u;
    }

    volume_apply(&vol);

    /* Phase 1: BPB integrity. Runs before mount so unmountable volumes
     * still produce diagnostic output. After volume_apply the diskio
     * offset shifts sector 0 to the partition's VBR. */
    total_errs += bpb_check((LBA_t)0u);

    /* FatFs mount (FF_MULTI_PARTITION = 0; partition offset is set in
     * diskio so FatFs sees the partition as a whole disk). */
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
        /* Phase 2: FAT tables. */
        total_errs += fat_check(&fs);

        /* Phase 3: directory walk + cluster bitmap. */
        {
            int srv = scan_run(&fs);
            if (srv > 0) total_errs += srv;
        }
    } else {
        prt_str("checkdsk: f_mount rc=");
        prt_dec((unsigned long)rc);
        prt_str(" err=");
        prt_dec((unsigned long)diskio_dss_last_error());
        prt_str(" (summary skipped)\r\n");
    }

    if (mount_ok) f_unmount(path);

    return total_errs > 0 ? 1u : 0u;
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

    if (argv[0][0] == '\0' || argv[0][1] != ':' || argv[0][2] != '\0') {
        prt_str("checkdsk: argument must be a single drive like \"C:\"\r\n");
        dss_exit(2u);
        return;
    }

    letter = argv[0][0];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - ('a' - 'A'));
    print_banner();
    code = dispatch(letter);
    dss_exit(code);
}
