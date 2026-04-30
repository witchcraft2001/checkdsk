/*
 * fix.c -- write-mode infrastructure. See fix.h.
 */

#include <sprinter.h>
#include "vol.h"
#include "diskio.h"
#include "sectbuf.h"
#include "prt.h"
#include "fix.h"

static u8    g_fix_enabled = 0u;
static DWORD g_fix_found   = 0ul;
static DWORD g_fix_applied = 0ul;

void fix_enable(void)        { g_fix_enabled = 1u; }
int  fix_enabled(void)       { return g_fix_enabled ? 1 : 0; }
void fix_count_found(void)   { g_fix_found++; }
void fix_count_applied(void) { g_fix_applied++; }
int  fix_any_found(void)     { return (g_fix_found != 0ul) ? 1 : 0; }

int fix_write(LBA_t lba, const BYTE *buf, UINT count)
{
    if (!g_fix_enabled) return 1;
    return (disk_write(0u, buf, lba, count) == RES_OK) ? 1 : 0;
}

int fix_dir_patch(LBA_t sect, WORD off, u8 kind, DWORD value)
{
    const BYTE *vb = (const BYTE *)&value;  /* SDCC z80 stores LSB first */

    if (!g_fix_enabled) return 1;
    if (disk_read(0u, g_sect_a, sect, 1u) != RES_OK) return 0;
    switch (kind) {
    case FIX_DPATCH_DELETE:
        g_sect_a[off] = 0xE5u;
        break;
    case FIX_DPATCH_DOT_CLUST:
        g_sect_a[off + 20u] = vb[2];
        g_sect_a[off + 21u] = vb[3];
        g_sect_a[off + 26u] = vb[0];
        g_sect_a[off + 27u] = vb[1];
        break;
    default: /* FIX_DPATCH_SIZE */
        g_sect_a[off + 28u] = vb[0];
        g_sect_a[off + 29u] = vb[1];
        g_sect_a[off + 30u] = vb[2];
        g_sect_a[off + 31u] = vb[3];
        break;
    }
    if (!fix_write(sect, g_sect_a, 1u)) return 0;
    fix_count_applied();
    return 1;
}

void fix_print_summary(void)
{
    if (g_fix_found == 0ul) return;

    if (g_fix_enabled) {
        prt_str("Fixes: found=");
        prt_dec((unsigned long)g_fix_found);
        prt_str(" applied=");
        prt_dec((unsigned long)g_fix_applied);
        prt_nl();
    } else {
        prt_str("Found ");
        prt_dec((unsigned long)g_fix_found);
        prt_str(" issue(s); run with /F to apply repairs\r\n");
    }
}
