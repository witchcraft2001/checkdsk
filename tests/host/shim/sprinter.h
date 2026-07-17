/* Host-build stand-in for the SDK's <sprinter.h>. Declares only what
 * the checked sources actually use; implementations in host_env.c. */
#ifndef HOST_SPRINTER_H
#define HOST_SPRINTER_H

#include <sprinter/types.h>
#include <string.h>

typedef struct { u16 year; u8 month; u8 day; u8 dow; } dss_date_t;
typedef struct { u8 hour; u8 minute; u8 second; }      dss_time_t;
typedef struct { u8 ascii; u8 scan; u8 mode; }         dss_key_t;

void dss_puts(const char *s);
void dss_putchar(u8 c);
void dss_getdate(dss_date_t *d);
void dss_gettime(dss_time_t *t);
int  dss_kbhit(void);
void dss_waitkey_ex(dss_key_t *k);
void dss_exit(u8 code);

#endif
