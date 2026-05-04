/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * prt.c -- minimal print helpers. See prt.h.
 */

#include <sprinter.h>
#include "prt.h"

void prt_str(const char *s)
{
    dss_puts(s);
}

void prt_chr(char c)
{
    dss_putchar((u8)c);
}

void prt_nl(void)
{
    dss_puts("\r\n");
}

/* 32-bit unsigned decimal printer that does NOT use 32-bit `/` or `%`,
 * so the linker can drop _divulong/_modulong from _HOME (saves ~263 B).
 * Strategy: subtract powers of 10 in descending order, counting how
 * many fit. SDCC z80 implements 32-bit `-` and `>=` inline -- no
 * runtime helper. The pow10[] table is 40 B in _CODE. Worst case
 * ~45 subtractions for the largest 32-bit value -- negligible at
 * Sprinter clock speeds for our volume of output. */
void prt_dec(unsigned long v)
{
    static const unsigned long pow10[10] = {
        1000000000ul, 100000000ul, 10000000ul,  1000000ul,
        100000ul,     10000ul,     1000ul,      100ul,
        10ul,         1ul
    };
    char buf[11];
    u8   i, d;
    char *p = buf;
    u8   started = 0u;

    for (i = 0u; i < 10u; i++) {
        unsigned long pw = pow10[i];
        d = 0u;
        while (v >= pw) { v -= pw; d++; }
        if (d != 0u || started || i == 9u) {
            *p++ = (char)('0' + d);
            started = 1u;
        }
    }
    *p = '\0';
    dss_puts(buf);
}

void prt_hex(unsigned long v, u8 width)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[12];
    char *end = buf + 11;
    u8   i;
    *end = '\0';
    if (width == 0u) width = 1u;
    if (width > 8u)  width = 8u;
    for (i = 0u; i < width; i++) {
        end--;
        *end = hex[v & 0x0Ful];
        v >>= 4;
    }
    dss_puts(end);
}
