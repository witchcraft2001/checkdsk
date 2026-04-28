/*
 * prt.c -- minimal print helpers. See prt.h.
 */

#include <sprinter.h>
#include "prt.h"

extern char *_utoa32(unsigned long val, char *end, int base, int upper);

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

void prt_dec(unsigned long v)
{
    char buf[12];
    char *p;
    p = _utoa32(v, buf + 11, 10, 0);
    dss_puts(p);
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
