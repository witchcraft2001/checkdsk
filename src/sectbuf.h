/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * sectbuf.h -- shared 512-byte sector buffer backed by DSS page memory.
 *
 * To free 512 B of static _DATA on the chkdsk binary (where the layout
 * is tight against the stack), the buffer lives in a borrowed DSS page
 * mapped to WIN3 (0xC000). The pointer `g_sect_a` is a thin macro that
 * ensures the sectbuf-page is currently mapped to WIN3 before each
 * access.
 *
 * Coexistence with WIN3-using modules (bitmap.c, diskio_batch.c):
 *   - sectbuf_acquire() invalidates bitmap's and batch's "current page"
 *     cache when it remaps WIN3 to its own page, so their next access
 *     triggers a re-mapping.
 *   - bitmap and batch both call sectbuf_invalidate() whenever they
 *     remap WIN3 to a different page, so sectbuf_acquire() will reissue
 *     the setwin_page on its next call.
 *
 * Lifecycle:
 *   sectbuf_init()    -- allocate one DSS page; must be called once at
 *                        startup before any disk I/O. Returns 1 on
 *                        success, 0 on out-of-page-memory.
 *   sectbuf_release() -- free the page. Required on every exit path.
 *   g_sect_a          -- u8* pointer to 512-byte buffer at 0xC000;
 *                        guaranteed valid until the next bitmap or
 *                        batch operation.
 */

#ifndef CHKDSK_SECTBUF_H
#define CHKDSK_SECTBUF_H

#include <sprinter/types.h>

int  sectbuf_init(void);
void sectbuf_release(void);

/* Map sectbuf-page into WIN3 if not already mapped. Returns the
 * fixed pointer 0xC000. Cheap when already mapped (one cache check). */
u8  *sectbuf_acquire(void);

/* Mark sectbuf as not currently mapped. Called by bitmap_map_page and
 * diskio_batch_map when they remap WIN3 to their own pages. */
void sectbuf_invalidate(void);

/* Compatibility: existing code refers to `g_sect_a` as a buffer name.
 * Routed through sectbuf_acquire so each access ensures correct
 * mapping. SDCC z80 inlines the call as long as it stays simple. */
#define g_sect_a (sectbuf_acquire())

#endif /* CHKDSK_SECTBUF_H */
