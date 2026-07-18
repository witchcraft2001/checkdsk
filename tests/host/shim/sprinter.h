/* Host-build stand-in for the SDK's <sprinter.h>. Declares only what
 * the checked sources actually use; implementations in host_env.c. */
#ifndef HOST_SPRINTER_H
#define HOST_SPRINTER_H

#include <sprinter/types.h>
#include <string.h>

typedef struct { u16 year; u8 month; u8 day; u8 dow; } dss_date_t;
typedef struct { u8 hour; u8 minute; u8 second; }      dss_time_t;
typedef struct { u8 ascii; u8 scan; u8 modifiers; u8 locks; } dss_key_t;

#define DSS_KEYMOD_RALT     0x01u
#define DSS_KEYMOD_RCTRL    0x02u
#define DSS_KEYMOD_LALT     0x04u
#define DSS_KEYMOD_LCTRL    0x08u
#define DSS_KEYMOD_ALT      0x10u
#define DSS_KEYMOD_CTRL     0x20u
#define DSS_KEYMOD_RSHIFT   0x40u
#define DSS_KEYMOD_LSHIFT   0x80u

void dss_puts(const char *s);
void dss_putchar(u8 c);
void dss_getdate(dss_date_t *d);
void dss_gettime(dss_time_t *t);
int  dss_kbhit(void);
void dss_waitkey_ex(dss_key_t *k);
void dss_exit(u8 code);

#endif
