/* vim: expandtab:tw=68:ts=4:sw=4:
 *
 * error.h - print error message and die.
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

#ifndef ___ERROR_H__GJp0myzKlvP6BokF___
#define ___ERROR_H__GJp0myzKlvP6BokF___ 1

    /* Provide C linkage for symbols declared here .. */
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <errno.h>
#include <stdlib.h>

extern void error(int doexit, int errnum, const char *fmt, ...);

/*
 * Handy shortcuts
 */
#define die(...)    error(1, 0, __VA_ARGS__)
#define warn(...)   error(0, 0, __VA_ARGS__)


/* argv[0] set by main() */
extern const char *program_name;

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* ! ___ERROR_H__GJp0myzKlvP6BokF___ */

/* EOF */
