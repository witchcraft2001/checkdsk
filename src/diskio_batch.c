/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * diskio_batch.c -- DSS page-memory backed batch sector reader. See
 * diskio_batch.h.
 */

#include <sprinter.h>
#include <sprinter/ports.h>
#include "vol.h"
#include "diskio.h"
#include "diskio_dss.h"
#include "diskio_batch.h"
#include "bitmap.h"
#include "sectbuf.h"

#define BATCH_WINDOW   3u           /* WIN3 = 0xC000..0xFFFF */
#define BATCH_WIN_BASE 0xC000u

static u8 g_block       = 0xFFu;    /* DSS block id (0xFF = unallocated) */
static u8 g_pages       = 0u;
static u8 g_saved_win3  = 0u;
static u8 g_have_saved  = 0u;
static u8 g_cur_page    = 0xFFu;    /* page idx currently mapped to WIN3 */

int diskio_batch_open(u8 num_pages)
{
    diskio_batch_close();

    if (num_pages == 0u) return 0;

    g_saved_win3 = inp(PORT_WIN3);
    g_have_saved = 1u;

    g_block = dss_getmem_pages(num_pages);
    if (g_block == 0xFFu) {
        g_pages = 0u;
        return 0;
    }
    g_pages    = num_pages;
    g_cur_page = 0xFFu;
    return 1;
}

void diskio_batch_close(void)
{
    if (g_block != 0xFFu) {
        dss_freemem(g_block);
        g_block = 0xFFu;
    }
    g_pages    = 0u;
    g_cur_page = 0xFFu;
    if (g_have_saved) {
        outp(PORT_WIN3, g_saved_win3);
        g_have_saved = 0u;
    }
}

u8 *diskio_batch_map(u8 page_idx)
{
    if (g_block == 0xFFu)        return (u8 *)0;
    if (page_idx >= g_pages)     return (u8 *)0;
    if (page_idx != g_cur_page) {
        dss_setwin_page(BATCH_WINDOW, g_block, page_idx);
        g_cur_page = page_idx;
        /* WIN3 now has a batch page. bitmap shares WIN3 too -- it must
         * re-map on its next access, otherwise its bitmap_get reads from
         * batch FAT data and returns wrong reachability bits.
         *
         * sectbuf shares WIN3 too -- same invalidation rule applies. */
        bitmap_invalidate();
        sectbuf_invalidate();
    }
    return (u8 *)BATCH_WIN_BASE;
}

void diskio_batch_invalidate(void)
{
    g_cur_page = 0xFFu;
}

int diskio_batch_read(unsigned long lba, u8 count, u8 page_idx)
{
    u8 *base;

    if (count == 0u || count > BATCH_SECTORS_PER_PAGE) return 0;

    base = diskio_batch_map(page_idx);
    if (base == (u8 *)0) return 0;
    if (diskio_dss_read_batch(lba, count, base) != 0u) return 0;
    return 1;
}
