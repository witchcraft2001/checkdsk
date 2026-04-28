/*
 * scan.c -- Phase 3 stub. See scan.h.
 *
 * Stage 3.1 only verifies that the bitmap allocation, the cluster/LBA
 * helpers, and the FAT-entry reader all work end-to-end on a real
 * volume. The output line documents what was probed so we can spot
 * regressions during later stages.
 */

#include <sprinter.h>
#include "ff.h"
#include "bitmap.h"
#include "chain.h"
#include "prt.h"
#include "scan.h"

int scan_run(FATFS *fs)
{
    DWORD probe;

    prt_str("Phase 3: directory and chain walk\r\n");

    if (!bitmap_init((u32)fs->n_fatent)) {
        prt_str("  error: cannot allocate cluster bitmap (");
        prt_dec((unsigned long)fs->n_fatent);
        prt_str(" entries)\r\n");
        return -1;
    }

    /* Reserved entries 0 and 1 are always "used" -- they hold the
     * media descriptor and the EOC/flags marker. */
    bitmap_set(0u);
    bitmap_set(1u);

    /* Sanity-probe FAT[2]. n_fatent >= 3 for any non-empty volume. */
    chain_invalidate();
    probe = chain_get_entry(fs, 2ul);
    if (probe == CHAIN_READ_ERROR) {
        prt_str("  error: FAT[2] read failed (bios=");
        prt_dec((unsigned long)chain_last_error());
        prt_str(")\r\n");
        bitmap_release();
        return -1;
    }

    prt_str("  bitmap ok, FAT[2]=0x");
    prt_hex((unsigned long)probe, 8u);
    prt_nl();

    bitmap_release();
    return 0;
}
