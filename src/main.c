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

#include <sprinter.h>
#include "vol.h"
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
    vol_t    fs;
    int      mrc;
    int      vrc;
    int      total_errs;

    total_errs = 0;

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

    mrc = vol_mount(&fs, 0u);
    if (mrc == VOL_OK) {
        if (summary_print(&fs, letter) != 0) {
            vol_unmount(&fs);
            return 1u;
        }
        total_errs += fat_check(&fs);
        {
            int srv = scan_run(&fs);
            if (srv > 0) total_errs += srv;
        }
        vol_unmount(&fs);
    } else {
        prt_str("checkdsk: vol_mount rc=");
        prt_dec((unsigned long)mrc);
        prt_str(" err=");
        prt_dec((unsigned long)diskio_dss_last_error());
        prt_str(" (summary skipped)\r\n");
    }

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
