/*
 * chkdsk_host.c -- host driver around the chkdsk core.
 *
 * Mirrors main.c's dispatch() EXACTLY (same CHKDSK_RC_* exit-code
 * scheme, same phase sequence/error propagation) but takes a
 * disk-image path instead of a DSS drive letter. Usage:
 *
 *   chkdsk_host <image> [/F] [/C] [/V]
 *
 * Keeping this a faithful mirror -- not just "close enough" -- matters:
 * chkdsk_host.c used to have its own simplified dispatch logic that
 * happened to already handle scan_run()<0 correctly while main.c's
 * real dispatch() silently swallowed it (reported a fatal scan abort
 * as "clean"). The harness could never have caught that divergence
 * while the two implementations disagreed. Any future change to
 * main.c's dispatch() must be mirrored here too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sprinter.h>
#include "vol.h"
#include "diskio.h"
#include "sectbuf.h"
#include "bpb.h"
#include "fat.h"
#include "scan.h"
#include "fix.h"
#include "summary.h"
#include "prt.h"

/* Same scheme as main.c -- see specs.md "Коды возврата". */
#define CHKDSK_RC_CLEAN         0
#define CHKDSK_RC_FOUND_UNFIXED 1
#define CHKDSK_RC_ALL_FIXED     2
#define CHKDSK_RC_PARTIAL_FIXED 3
#define CHKDSK_RC_FATAL         255

int host_disk_open(const char *path);
int host_disk_flush(void);

static vol_t g_fs;

int main(int argc, char **argv)
{
    const char *img = NULL;
    int i, mrc, total_errs = 0, srv;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcasecmp(a, "/F") || !strcasecmp(a, "-f"))      fix_enable();
        else if (!strcasecmp(a, "/C") || !strcasecmp(a, "-c")) fix_enable_convert();
        else if (!strcasecmp(a, "/V") || !strcasecmp(a, "-v")) fix_enable_verbose();
        else if (!strcasecmp(a, "/Y") || !strcasecmp(a, "-y")) fix_enable_assume_yes();
        else if (!img)                                         img = a;
        else { fprintf(stderr, "too many args\n"); return CHKDSK_RC_FATAL; }
    }
    if (!img) {
        fprintf(stderr, "usage: chkdsk_host <image> [/F] [/C] [/V]\n");
        return CHKDSK_RC_FATAL;
    }
    if (!host_disk_open(img)) return CHKDSK_RC_FATAL;

    prt_str("Mode: ");
    prt_str(fix_enabled() ? "write" : "read-only");
    prt_nl();

    mrc = vol_mount(&g_fs, 0u);
    total_errs += bpb_check(&g_fs, mrc);

    if (mrc != VOL_OK) {
        prt_str("mount rc=");
        prt_dec((unsigned long)mrc);
        prt_nl();
        fix_print_summary();
        host_disk_flush();
        return CHKDSK_RC_FATAL;
    }

    if (summary_print(&g_fs, 'H') != 0) {
        host_disk_flush();
        return CHKDSK_RC_FATAL;
    }
    total_errs += fat_check(&g_fs);

    srv = scan_run(&g_fs);
    if (srv < 0) {
        host_disk_flush();
        return CHKDSK_RC_FATAL;
    }
    total_errs += srv;

#if CHKDSK_FAT32
    if (fix_any_applied()) (void)fat_invalidate_fsinfo(&g_fs);
#endif
    vol_unmount(&g_fs);

    fix_print_summary();
    if (!host_disk_flush()) return CHKDSK_RC_FATAL;

    if (total_errs == 0)      return CHKDSK_RC_CLEAN;
    if (!fix_enabled())       return CHKDSK_RC_FOUND_UNFIXED;
    if (fix_any_incomplete()) return CHKDSK_RC_PARTIAL_FIXED;
    return CHKDSK_RC_ALL_FIXED;
}
