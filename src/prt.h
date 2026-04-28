/*
 * prt.h -- minimal print helpers built on dss_puts / dss_putchar.
 *
 * The SDK's printf pulls in stdio init, sprintf, fputc and the
 * variadic format engine (~3 KB). We only need a handful of output
 * shapes (string, decimal, fixed-width hex, newline) so this module
 * provides them directly. _utoa32 is reused from printf_long.rel.
 */

#ifndef CHKDSK_PRT_H
#define CHKDSK_PRT_H

#include <sprinter/types.h>

void prt_str(const char *s);
void prt_chr(char c);
void prt_nl(void);
void prt_dec(unsigned long v);
void prt_hex(unsigned long v, u8 width);   /* width chars, leading zeros */

#endif /* CHKDSK_PRT_H */
