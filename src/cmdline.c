/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dmitry Mikhalchenkov, Sprinter Team
 */
/*
 * cmdline.c -- minimal command-line helpers. Mirrors the patterns in
 * utils/deltree/src/util.c.
 */

#include <ctype.h>
#include <sprinter.h>
#include "cmdline.h"

void cmd_read_safe(char *out, int out_sz)
{
    char *src;
    int   i;
    int   saw_bad;

    src = dss_cmdline();
    i = 0;
    saw_bad = 0;
    while (*src != '\0' && *src != '\r' && *src != '\n' && i < out_sz - 1) {
        unsigned char ch;

        ch = (unsigned char)*src;
        if (ch >= 32u && ch <= 126u) {
            out[i++] = (char)ch;
        } else {
            saw_bad = 1;
            break;
        }
        src++;
    }
    out[i] = '\0';
    if (saw_bad) out[0] = '\0';
}

int cmd_parse(char *buf, char *argv[CHKDSK_MAX_ARGV])
{
    int   argc;
    char *p;

    argc = 0;
    p = buf;
    while (*p != '\0' && argc < CHKDSK_MAX_ARGV) {
        while (*p == ' ') p++;
        if (*p == '\0' || *p == '\r' || *p == '\n') break;
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\r' && *p != '\n') p++;
        if (*p != '\0') {
            *p = '\0';
            p++;
        }
    }
    return argc;
}

int cmd_strieq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}
