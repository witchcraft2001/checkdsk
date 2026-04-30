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
#include "fix.h"
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
    prt_str("Usage: CHKDSK <drive>: [/F]\r\n");
    prt_str("  <drive>:  A, B (floppies) or C, D, ... (IDE partitions)\r\n");
    prt_str("  /F        apply repairs (default: report only)\r\n");
    prt_str("  /?        this help\r\n");
}

/* Big aggregates kept in BSS so dispatch's stack frame stays small --
 * the deep call chain (scan_run -> walk_tree -> step -> walk_chain ->
 * chain_get_entry) needs the headroom. */
static volume_t g_vol;
static vol_t    g_fs;

static u8 dispatch(char letter)
{
    int mrc;
    int vrc;
    int total_errs;

    total_errs = 0;

    vrc = volume_resolve(letter, &g_vol);
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

    volume_apply(&g_vol);

    prt_str("Mode: ");
    prt_str(fix_enabled() ? "write (repairs will be applied)" : "read-only");
    prt_str("\r\n");

    /* Mount first so the BPB diagnostic can speak in terms of the
     * already-parsed vol_t -- avoids parsing the BPB twice. */
    mrc = vol_mount(&g_fs, 0u);
    total_errs += bpb_check(&g_fs, mrc);

    if (mrc == VOL_OK) {
        if (summary_print(&g_fs, letter) != 0) {
            vol_unmount(&g_fs);
            return 1u;
        }
        total_errs += fat_check(&g_fs);
        {
            int srv = scan_run(&g_fs);
            if (srv > 0) total_errs += srv;
        }
        vol_unmount(&g_fs);
    } else {
        prt_str("checkdsk: vol_mount rc=");
        prt_dec((unsigned long)mrc);
        prt_str(" err=");
        prt_dec((unsigned long)diskio_dss_last_error());
        prt_str(" (summary skipped)\r\n");
    }

    fix_print_summary();
    return (total_errs > 0 || fix_any_found()) ? 1u : 0u;
}

/* Same rationale as g_vol/g_fs above -- keep cmdbuf out of the stack. */
static char  g_cmdbuf[CHKDSK_MAX_CMDLINE];
static char *g_argv[CHKDSK_MAX_ARGV];

void main(void)
{
    int   argc;
    int   i;
    char *drive_arg = (char *)0;
    char  letter;
    u8    code;

    cmd_read_safe(g_cmdbuf, (int)sizeof(g_cmdbuf));
    argc = cmd_parse(g_cmdbuf, g_argv);

    if (argc == 0) {
        print_usage();
        dss_exit(2u);
        return;
    }

    for (i = 0; i < argc; i++) {
        if (cmd_strieq(g_argv[i], "/?") || cmd_strieq(g_argv[i], "-h") ||
            cmd_strieq(g_argv[i], "--help")) {
            print_usage();
            dss_exit(0u);
            return;
        }
        if (cmd_strieq(g_argv[i], "/F") || cmd_strieq(g_argv[i], "/FIX")) {
            fix_enable();
            continue;
        }
        if (drive_arg != (char *)0) {
            prt_str("checkdsk: too many arguments\r\n");
            dss_exit(2u);
            return;
        }
        drive_arg = g_argv[i];
    }

    if (drive_arg == (char *)0 ||
        drive_arg[0] == '\0' || drive_arg[1] != ':' || drive_arg[2] != '\0') {
        prt_str("checkdsk: argument must be a single drive like \"C:\"\r\n");
        dss_exit(2u);
        return;
    }

    letter = drive_arg[0];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - ('a' - 'A'));
    print_banner();
    code = dispatch(letter);
    dss_exit(code);
}
