/*
 * sectbuf.h -- two shared 512-byte sector buffers reused across the
 * phases (volume resolve, BPB check, FAT walk).
 *
 * Phases run sequentially, so the buffers can be shared without
 * stepping on each other. This saves ~1.5 KB of static data versus
 * one set of buffers per module.
 */

#ifndef CHKDSK_SECTBUF_H
#define CHKDSK_SECTBUF_H

#include <sprinter/types.h>

extern u8 g_sect_a[512];

#endif /* CHKDSK_SECTBUF_H */
