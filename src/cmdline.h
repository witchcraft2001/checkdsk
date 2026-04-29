/*
 * cmdline.h -- minimal command-line helpers, ported from
 * utils/deltree/src/util.c (subset needed for stage 0).
 */

#ifndef CHKDSK_CMDLINE_H
#define CHKDSK_CMDLINE_H

#define CHKDSK_MAX_CMDLINE  48
#define CHKDSK_MAX_ARGV     4

/* Read DSS command line into `out` (max out_sz - 1 chars) with ASCII
 * validation. On any non-printable byte the result is empty. */
void cmd_read_safe(char *out, int out_sz);

/* Tokenise `buf` in place by replacing separators with NUL and storing
 * argv pointers. Returns argc (may be 0). */
int  cmd_parse(char *buf, char *argv[CHKDSK_MAX_ARGV]);

/* Case-insensitive string compare; returns 1 if equal. */
int  cmd_strieq(const char *a, const char *b);

#endif /* CHKDSK_CMDLINE_H */
