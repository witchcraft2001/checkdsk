/*
 * scan.c -- Phase 3 driver. See scan.h.
 *
 * Stage 3.2: walk the root directory and emit one line per entry,
 * truncating after SCAN_DUMP_LIMIT to keep output usable on populated
 * volumes. Bitmap allocation + FAT[2] sanity probe from Stage 3.1
 * remain so we keep covering the cluster-bitmap path.
 *
 * Cluster-bitmap marking and FAT-chain walking happen in Stage 3.5;
 * Stage 3.2 is purely the iterator + parser plus a debug emitter.
 */

#include <sprinter.h>
#include "vol.h"
#include "bitmap.h"
#include "chain.h"
#include "dirwalk.h"
#include "prt.h"
#include "scan.h"

#define SCAN_DUMP_LIMIT 16u

#define ATTR_RO    0x01u
#define ATTR_HID   0x02u
#define ATTR_SYS   0x04u
#define ATTR_VOLID 0x08u
#define ATTR_DIR   0x10u
#define ATTR_ARC   0x20u
#define ATTR_LFN   0x0Fu

static DWORD ld_dword(const BYTE *p)
{
    return  (DWORD)p[0]
         | ((DWORD)p[1] << 8)
         | ((DWORD)p[2] << 16)
         | ((DWORD)p[3] << 24);
}

static WORD ld_word(const BYTE *p)
{
    return (WORD)((WORD)p[0] | ((WORD)p[1] << 8));
}

/* Print "FILENAME.EXT" from a raw 8.3 name (space-padded). Replaces
 * any byte < 0x20 or >= 0x7F with '?' to keep terminal output sane. */
static void print_sfn(const BYTE *raw)
{
    UINT i;
    BYTE c;
    int  emitted_dot = 0;

    for (i = 0u; i < 8u; i++) {
        c = raw[i];
        if (c == ' ') break;
        if (c < 0x20u || c >= 0x7Fu) c = '?';
        prt_chr((char)c);
    }
    for (i = 8u; i < 11u; i++) {
        c = raw[i];
        if (c == ' ') continue;
        if (!emitted_dot) { prt_chr('.'); emitted_dot = 1; }
        if (c < 0x20u || c >= 0x7Fu) c = '?';
        prt_chr((char)c);
    }
}

static void print_attr(BYTE attr)
{
    prt_chr((attr & ATTR_DIR)   ? 'D' : '-');
    prt_chr((attr & ATTR_ARC)   ? 'A' : '-');
    prt_chr((attr & ATTR_RO)    ? 'R' : '-');
    prt_chr((attr & ATTR_HID)   ? 'H' : '-');
    prt_chr((attr & ATTR_SYS)   ? 'S' : '-');
    prt_chr((attr & ATTR_VOLID) ? 'V' : '-');
}

static void dump_entry(const BYTE *e)
{
    BYTE  attr;
    DWORD first_clust;
    DWORD size;

    prt_str("    ");
    if (e[0] == 0xE5u) {
        prt_str("[del] ");
    }

    attr = e[11];
    if ((attr & ATTR_LFN) == ATTR_LFN) {
        prt_str("[lfn ord=0x");
        prt_hex((unsigned long)(e[0] & 0xFFu), 2u);
        prt_chr(']');
        prt_nl();
        return;
    }

    print_sfn(e);
    /* Pad to a fixed column so the trailing fields line up. */
    {
        UINT name_len = 0u;
        UINT i;
        for (i = 0u; i < 8u && e[i] != ' '; i++) name_len++;
        if (e[8] != ' ' || e[9] != ' ' || e[10] != ' ') {
            UINT ext_len = 0u;
            for (i = 8u; i < 11u && e[i] != ' '; i++) ext_len++;
            name_len += 1u + ext_len;   /* '.' + ext */
        }
        while (name_len < 13u) { prt_chr(' '); name_len++; }
    }

    prt_chr(' ');
    print_attr(attr);

    first_clust = ((DWORD)ld_word(&e[20]) << 16) | (DWORD)ld_word(&e[26]);
    size        = ld_dword(&e[28]);

    prt_str(" c=");
    prt_hex((unsigned long)first_clust, 8u);
    prt_str(" s=");
    prt_hex((unsigned long)size, 8u);
    prt_nl();
}

static int walk_root(vol_t *fs)
{
    dirwalk_t w;
    BYTE     *e;
    DWORD     total;
    DWORD     dumped;
    int       rc;

    prt_str("  Root:\r\n");
    dirwalk_open_root(&w, fs);

    total  = 0ul;
    dumped = 0ul;
    while ((rc = dirwalk_next(&w, &e)) == 1) {
        total++;
        if (dumped < SCAN_DUMP_LIMIT) {
            dump_entry(e);
            dumped++;
        }
    }
    if (rc < 0) {
        prt_str("    error: dir read failed at sector\r\n");
        return -1;
    }

    if (total > dumped) {
        prt_str("    ... (");
        prt_dec((unsigned long)(total - dumped));
        prt_str(" more)\r\n");
    }
    prt_str("  Total entries: ");
    prt_dec((unsigned long)total);
    prt_nl();
    return 0;
}

int scan_run(vol_t *fs)
{
    DWORD probe;

    prt_str("Phase 3: directory and chain walk\r\n");

    if (!bitmap_init((u32)fs->n_fatent)) {
        prt_str("  error: cannot allocate cluster bitmap (");
        prt_dec((unsigned long)fs->n_fatent);
        prt_str(" entries)\r\n");
        return -1;
    }

    bitmap_set(0u);
    bitmap_set(1u);

    chain_invalidate();
    probe = chain_get_entry(fs, 2ul);
    if (probe == CHAIN_READ_ERROR) {
        prt_str("  error: FAT[2] read failed (bios=");
        prt_dec((unsigned long)chain_last_error());
        prt_str(")\r\n");
        bitmap_release();
        return -1;
    }

    if (walk_root(fs) < 0) {
        bitmap_release();
        return -1;
    }

    prt_str("  bitmap ok, FAT[2]=0x");
    prt_hex((unsigned long)probe, 8u);
    prt_nl();

    bitmap_release();
    return 0;
}
