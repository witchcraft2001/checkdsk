/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * bitmap.h -- cluster bitmap backed by DSS page memory.
 *
 * Stage 0 exposes a single global bitmap instance. Stage 3 will need
 * two (used + dirs) and we will revisit then -- options are to split
 * the underlying block in half or to refactor into instances.
 *
 * Lifecycle:
 *   bitmap_init(N)   -- allocate ceil(N/8/16384) DSS pages as one
 *                       block; returns 0 on out-of-memory.
 *   bitmap_set/get/test_and_set/clear_all -- access by bit index.
 *   bitmap_release() -- free the block. Must be called on every
 *                       exit path, including Ctrl+X/Ctrl+Z abort.
 *
 * Implementation maps one page at a time into WIN3 (0xC000..0xFFFF).
 * The currently-mapped page index is cached so sequential access does
 * not re-issue dss_setwin_page on every bit.
 */

#ifndef CHKDSK_BITMAP_H
#define CHKDSK_BITMAP_H

#include <sprinter/types.h>

/* 1 = success, 0 = out of page memory. */
int  bitmap_init(u32 num_bits);

void bitmap_release(void);

/* Return the bit value at `idx` (0 or 1). */
u8   bitmap_get(u32 idx);

/* Set the bit at `idx` to 1. */
void bitmap_set(u32 idx);

/* Atomically test-and-set: return previous value, then ensure bit is 1.
 * Used for cross-link detection in stage 3. */
u8   bitmap_test_and_set(u32 idx);

/* Zero every byte in every allocated page. */
void bitmap_clear_all(void);

#endif /* CHKDSK_BITMAP_H */
