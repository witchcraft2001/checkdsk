/*
 * chkdsk_host.c -- host driver around the chkdsk core.
 *
 * Mirrors main.c's dispatch() but takes a disk-image path instead of a
 * DSS drive letter. Usage:
 *
 *   chkdsk_host <image> [/F] [/C] [/V]
 *
 * Exit code: 0 volume clean, 1 issues found (or repairs applied),
 * 2 usage / image error -- same convention as the target binary.
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

int host_disk_open(const char *path);
int host_disk_flush(void);

static vol_t g_fs;

int main(int argc, char **argv)
{
    const char *img = NULL;
    int i, mrc, total_errs = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcasecmp(a, "/F") || !strcasecmp(a, "-f"))      fix_enable();
        else if (!strcasecmp(a, "/C") || !strcasecmp(a, "-c")) fix_enable_convert();
        else if (!strcasecmp(a, "/V") || !strcasecmp(a, "-v")) fix_enable_verbose();
        else if (!img)                                         img = a;
        else { fprintf(stderr, "too many args\n"); return 2; }
    }
    if (!img) {
        fprintf(stderr, "usage: chkdsk_host <image> [/F] [/C] [/V]\n");
        return 2;
    }
    if (!host_disk_open(img)) return 2;

    prt_str("Mode: ");
    prt_str(fix_enabled() ? "write" : "read-only");
    prt_nl();

    mrc = vol_mount(&g_fs, 0u);
    total_errs += bpb_check(&g_fs, mrc);

    if (mrc == VOL_OK) {
        if (summary_print(&g_fs, 'H') != 0) return 1;
        total_errs += fat_check(&g_fs);
        {
            int srv = scan_run(&g_fs);
            if (srv > 0) total_errs += srv;
            if (srv < 0) { host_disk_flush(); return 1; }
        }
#if CHKDSK_FAT32
        if (fix_any_applied()) (void)fat_invalidate_fsinfo(&g_fs);
#endif
        vol_unmount(&g_fs);
    } else {
        prt_str("mount rc=");
        prt_dec((unsigned long)mrc);
        prt_nl();
    }

    fix_print_summary();
    if (!host_disk_flush()) return 2;
    return (total_errs > 0 || fix_any_found()) ? 1 : 0;
}
