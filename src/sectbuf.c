/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * sectbuf.c -- 512-byte sector buffer backed by a DSS page in WIN3.
 *
 * See sectbuf.h for the contract. WIN3 is shared with bitmap.c and
 * diskio_batch.c -- each module remaps when it needs its own page,
 * and invalidates the others' cached "current mapping" flag so the
 * next acquire re-issues setwin_page.
 */

#include <sprinter.h>
#include "sectbuf.h"
#include "bitmap.h"
#include "diskio_batch.h"

#define SECTBUF_WIN     3u
#define SECTBUF_BASE    0xC000u

static u8 g_block  = 0xFFu;     /* DSS block id */
static u8 g_mapped = 0u;        /* 1 = sectbuf page is currently in WIN3 */

int sectbuf_init(void)
{
    sectbuf_release();
    g_block = dss_getmem_pages(1u);
    if (g_block == 0xFFu) return 0;
    g_mapped = 0u;
    return 1;
}

void sectbuf_release(void)
{
    if (g_block != 0xFFu) {
        dss_freemem(g_block);
        g_block = 0xFFu;
    }
    g_mapped = 0u;
}

u8 *sectbuf_acquire(void)
{
    if (!g_mapped) {
        dss_setwin_page(SECTBUF_WIN, g_block, 0u);
        g_mapped = 1u;
        /* The other WIN3 sharers must not assume their pages are
         * still up. Tell them to re-map on next access. */
        bitmap_invalidate();
        diskio_batch_invalidate();
    }
    return (u8 *)SECTBUF_BASE;
}

void sectbuf_invalidate(void)
{
    g_mapped = 0u;
}
