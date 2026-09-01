/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PRISM_LOG_H
#define PRISM_LOG_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Everything goes to stderr so it interleaves with WINEDEBUG output and can be
 * captured with `wine Prism.exe 2>prism.log`. */
#define prism_info(...)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        fprintf(stderr, "[PrismCapture] ");                                                                            \
        fprintf(stderr, __VA_ARGS__);                                                                                  \
        fprintf(stderr, "\n");                                                                                         \
        fflush(stderr);                                                                                                \
    } while(0)

#define prism_warn(...)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        fprintf(stderr, "[PrismCapture] WARN ");                                                                       \
        fprintf(stderr, __VA_ARGS__);                                                                                  \
        fprintf(stderr, "\n");                                                                                         \
        fflush(stderr);                                                                                                \
    } while(0)

#ifdef PRISM_DEBUG
#define prism_debug(...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        fprintf(stderr, "[PrismCapture] DEBUG ");                                                                      \
        fprintf(stderr, __VA_ARGS__);                                                                                  \
        fprintf(stderr, "\n");                                                                                         \
        fflush(stderr);                                                                                                \
    } while(0)
#else
#define prism_debug(...)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
    } while(0)
#endif

static inline unsigned long long prism_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

#endif /* PRISM_LOG_H */
