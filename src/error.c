/* vim: expandtab:tw=68:ts=4:sw=4:
 *
 * error.c - print error message and die
 *
 * Copyright (c) 1999-2017 Sudhi Herle <sw at herle.net>
 *
 * Licensing Terms: GPLv2 
 *
 * If you need a commercial license for this work, please contact
 * the author.
 *
 * This software does not come with any express or implied
 * warranty; it is provided "as is". No claim  is made to its
 * suitability for any purpose.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "error.h"

const char * program_name = 0;

void
error(int doexit, int errnum, const char *fmt, ...)
{
    char buf[1024];

    if (errnum < 0) errnum = -errnum;

    fflush(stdout);
    fflush(stderr);

    va_list ap;
    size_t  n;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    n = strlen(buf);
    if (buf[n-1] == '\n') buf[--n] = 0;

    fprintf(stderr, "%s: %s", program_name, buf);
    if (errnum > 0) fprintf(stderr, ": %s (%d)", strerror(errnum), errnum);

    fputc('\n', stderr);
    fflush(stderr);

    if (doexit) exit(doexit);
}
/* EOF */
