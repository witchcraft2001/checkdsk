/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * bitmap.c -- cluster bitmap over DSS page memory. See bitmap.h.
 */

#include <sprinter.h>
#include <string.h>
#include "bitmap.h"
#include "diskio_batch.h"

#define BITMAP_PAGE_SIZE  16384u   /* DSS page granule */
#define BITMAP_WIN        3u       /* WIN3 = 0xC000..0xFFFF */
#define BITMAP_WIN_BASE   0xC000u

static u8  g_block      = 0xFFu;   /* dss block id; 0xFF = unallocated */
static u8  g_num_pages  = 0u;
static u8  g_cur_page   = 0xFFu;   /* index of page currently in WIN3 */
static u32 g_num_bits   = 0u;

static void bitmap_map_page(u8 page_idx)
{
    if (page_idx == g_cur_page) return;
    dss_setwin_page(BITMAP_WIN, g_block, page_idx);
    g_cur_page = page_idx;
    /* WIN3 now has a bitmap page. diskio_batch may still think its
     * batch page is mapped here -- tell it to re-map next time. Without
     * this, phase 4's main loop reads "FAT entries" from bitmap memory
     * after every bitmap_get/test_and_set call, producing garbage that
     * cascaded into LOSTCHN entries pointing at wrong clusters. */
    diskio_batch_invalidate();
}

void bitmap_invalidate(void)
{
    g_cur_page = 0xFFu;
}

int bitmap_init(u32 num_bits)
{
    u32 total_bytes;
    u32 pages_needed;

    bitmap_release();

    total_bytes  = (num_bits + 7u) >> 3;
    /* page_size is 16384 (2^14): shift, no _divulong. */
    pages_needed = (total_bytes + (BITMAP_PAGE_SIZE - 1u)) >> 14;
    if (pages_needed == 0u) pages_needed = 1u;
    if (pages_needed > 255u) return 0;     /* DSS block cannot exceed 255 pages */

    g_block = dss_getmem_pages((u8)pages_needed);
    if (g_block == 0xFFu) {
        g_num_pages = 0u;
        g_num_bits  = 0u;
        return 0;
    }
    g_num_pages = (u8)pages_needed;
    g_num_bits  = num_bits;
    g_cur_page  = 0xFFu;

    bitmap_clear_all();
    return 1;
}

void bitmap_release(void)
{
    if (g_block != 0xFFu) {
        dss_freemem(g_block);
        g_block = 0xFFu;
    }
    g_num_pages = 0u;
    g_num_bits  = 0u;
    g_cur_page  = 0xFFu;
}

void bitmap_clear_all(void)
{
    u8 i;
    for (i = 0u; i < g_num_pages; i++) {
        bitmap_map_page(i);
        memset((void *)BITMAP_WIN_BASE, 0, BITMAP_PAGE_SIZE);
    }
}

u8 bitmap_get(u32 idx)
{
    u32 byte_off;
    u8  page_idx;
    u16 in_page;
    u8  byte_val;

    if (idx >= g_num_bits) return 0;
    byte_off = idx >> 3;
    /* BITMAP_PAGE_SIZE is 16384 (2^14) -- shift/mask, no _divulong/_modulong. */
    page_idx = (u8)(byte_off >> 14);
    in_page  = (u16)(byte_off & 0x3FFFul);
    bitmap_map_page(page_idx);
    byte_val = *(u8 *)(BITMAP_WIN_BASE + in_page);
    return (u8)((byte_val >> (idx & 7u)) & 1u);
}

void bitmap_set(u32 idx)
{
    u32 byte_off;
    u8  page_idx;
    u16 in_page;
    u8 *p;

    if (idx >= g_num_bits) return;
    byte_off = idx >> 3;
    /* BITMAP_PAGE_SIZE is 16384 (2^14) -- shift/mask, no _divulong/_modulong. */
    page_idx = (u8)(byte_off >> 14);
    in_page  = (u16)(byte_off & 0x3FFFul);
    bitmap_map_page(page_idx);
    p = (u8 *)(BITMAP_WIN_BASE + in_page);
    *p = (u8)(*p | (1u << (idx & 7u)));
}

u8 bitmap_test_and_set(u32 idx)
{
    u32 byte_off;
    u8  page_idx;
    u16 in_page;
    u8 *p;
    u8  mask;
    u8  prev;

    if (idx >= g_num_bits) return 0;
    byte_off = idx >> 3;
    /* BITMAP_PAGE_SIZE is 16384 (2^14) -- shift/mask, no _divulong/_modulong. */
    page_idx = (u8)(byte_off >> 14);
    in_page  = (u16)(byte_off & 0x3FFFul);
    bitmap_map_page(page_idx);
    p = (u8 *)(BITMAP_WIN_BASE + in_page);
    mask = (u8)(1u << (idx & 7u));
    prev = (u8)((*p & mask) ? 1u : 0u);
    *p = (u8)(*p | mask);
    return prev;
}
