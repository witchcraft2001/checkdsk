/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
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
#include "sectbuf.h"
#include "prt.h"

#ifndef CHKDISK_VERSION
#define CHKDISK_VERSION "0.0.dev"
#endif

static void print_banner(void)
{
    prt_str("checkdsk " CHKDISK_VERSION "\r\n");
}

static void print_usage(void)
{
    print_banner();
    prt_str("Usage: CHKDSK X: [/F] [/C] [/V]\r\n"
            "  /F  apply repairs\r\n"
            "  /C  with /F: orphans -> FILE####.CHK\r\n"
            "  /V  verbose progress\r\n");
}

/* Big aggregates kept in BSS so dispatch's stack frame stays small --
 * the deep call chain (scan_run -> walk_tree -> step -> walk_chain ->
 * chain_get_entry) needs the headroom.
 *
 * Left uninitialised: volume_resolve fully populates g_vol before
 * any read, vol_mount fully populates g_fs before any read. Keeping
 * them in _DATA (rather than _INITIALIZED) saves ~46 bytes of
 * _INITIALIZER, which the layout needs to keep stack above the
 * Phase 2 peak (sums[] back on stack costs ~128 bytes there). */
static volume_t g_vol;
static vol_t    g_fs;

static u8 dispatch(char letter)
{
    int mrc;
    int vrc;
    int total_errs;

    total_errs = 0;

    vrc = volume_resolve(letter, &g_vol);
    if (vrc == VOL_ERR_BAD_LETTER || vrc == VOL_ERR_UNSUPPORTED) {
        prt_str("bad drive: ");
        prt_chr(letter);
        prt_nl();
        return 2u;
    }

    volume_apply(&g_vol);

    prt_str("Mode: ");
    prt_str(fix_enabled() ? "write" : "read-only");
    prt_nl();

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
#if CHKDSK_FAT32
        /* Force DSS to recompute FSInfo free_count on next mount.
         * Done after all phase-2/4 writes so the recalc reflects the
         * post-fix state. Gated on fix_any_applied so a clean /F run
         * doesn't rewrite an already-correct FSInfo sector. */
        if (fix_any_applied()) (void)fat_invalidate_fsinfo(&g_fs);
#endif
        vol_unmount(&g_fs);
    } else {
        prt_str("mount rc=");
        prt_dec((unsigned long)mrc);
        prt_str(" be=");
        prt_dec((unsigned long)diskio_dss_last_error());
        prt_nl();
    }

    fix_print_summary();
    return (total_errs > 0 || fix_any_found()) ? 1u : 0u;
}

/* Same rationale as g_vol/g_fs above -- keep cmdbuf out of the stack.
 * Left uninitialized: cmd_read_safe fills g_cmdbuf in full before any
 * read; cmd_parse populates g_argv. Keeping them in _DATA (rather
 * than _INITIALIZED) avoids ~56 bytes of _INITIALIZER bloat at a
 * point in the layout where _INITIALIZER and _INITIALIZED would
 * otherwise overlap. */
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
        if (cmd_strieq(g_argv[i], "/C") || cmd_strieq(g_argv[i], "/CONVERT")) {
            fix_enable_convert();
            continue;
        }
        if (cmd_strieq(g_argv[i], "/V") || cmd_strieq(g_argv[i], "/VERBOSE")) {
            fix_enable_verbose();
            continue;
        }
        if (drive_arg != (char *)0) {
            prt_str("too many args\r\n");
            dss_exit(2u);
            return;
        }
        drive_arg = g_argv[i];
    }

    if (drive_arg == (char *)0 ||
        drive_arg[0] == '\0' || drive_arg[1] != ':' || drive_arg[2] != '\0') {
        prt_str("expected drive (e.g. C:)\r\n");
        dss_exit(2u);
        return;
    }

    letter = drive_arg[0];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - ('a' - 'A'));
    print_banner();

    /* sectbuf borrows a DSS page so its 512 B don't sit in static _DATA;
     * must be initialised before the first disk_read. */
    if (!sectbuf_init()) {
        prt_str("no page memory for sectbuf\r\n");
        dss_exit(255u);
        return;
    }

    code = dispatch(letter);

    /* If /F actually wrote anything, force DSS to drop its cached BPB
     * and FAT/dir buffers. Without this DSS would continue serving the
     * pre-fix volume state and the next file operation could overwrite
     * our repairs with stale buffers. specs.md:288 -- mandatory. */
    if (fix_any_applied()) diskio_dss_rescan();

    sectbuf_release();
    dss_exit(code);
}
