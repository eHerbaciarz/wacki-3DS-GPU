/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * include/wacki/log.h — leveled logging façade.
 *
 * Every call site that used to write `fprintf(stderr, "[tag] …\n", …)`
 * goes through one of the LOG_* macros below. The helper formats one
 * line per call as `[level/tag] message\n` on stderr, gated by:
 *
 *   - runtime severity threshold (g_log_min_level / --verbose / -q)
 *   - compile-time guard for the noisiest categories (-DWACKI_VERBOSE
 *     turns LOG_TRACE / LOG_DEBUG on; in release builds they're
 *     statement-expression no-ops, zero cost at the call site).
 *
 * Classification policy:
 *   TRACE   per-frame / per-tick / per-script-op churn
 *   DEBUG   one-shot dev breadcrumbs (interesting but not on-by-default)
 *   INFO    boot / scene transitions / asset load / menu navigation
 *   WARN    recoverable issues, fallbacks, suspicious-but-not-fatal
 *   ERROR   genuine failures (load failed, can't open device, etc.)
 *
 * Tag names are short snake-case or kebab-case strings, no brackets —
 * the helper supplies them. Keep tag names stable across releases so
 * users' grep patterns survive. */

#ifndef WACKI_LOG_H
#define WACKI_LOG_H

#include <stdarg.h>

typedef enum WackiLogLevel {
    WL_TRACE = 0,
    WL_DEBUG = 1,
    WL_INFO  = 2,
    WL_WARN  = 3,
    WL_ERROR = 4,
} WackiLogLevel;

/* Runtime severity threshold. Default WL_WARN in release builds (quiet),
 * WL_INFO under -DWACKI_VERBOSE (dev). Override via -v/-q or WACKI_LOG_LEVEL. */
extern WackiLogLevel g_log_min_level;

/* Optional extra destination for WARN and ERROR lines (NULL = none). Gets
 * every WARN/ERROR, even below g_log_min_level, already formatted. Set by
 * a platform that keeps its own log file (3DS: wacki_perf.log). */
extern void (*g_log_sink)(WackiLogLevel lvl, const char *tag, const char *msg);

/* The one printf-style helper. Adds the level/tag prefix + trailing
 * newline; the call site supplies only the body. Format and varargs
 * are checked at compile time. */
void wacki_log(WackiLogLevel lvl, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOG_INFO(tag,  ...)  wacki_log(WL_INFO,  (tag), __VA_ARGS__)
#define LOG_WARN(tag,  ...)  wacki_log(WL_WARN,  (tag), __VA_ARGS__)
#define LOG_ERROR(tag, ...)  wacki_log(WL_ERROR, (tag), __VA_ARGS__)

/* Diagnostics (src/log.c): wacki.log + RAM ring of the last lines. */
#include <stdint.h>
extern int        g_log_diag;            /* 1 = keep TRACE/DEBUG in the ring */
extern uint32_t (*g_log_clock)(void);    /* ms timestamps (SDL_GetTicks)     */
extern int        g_log_stderr_is_file;  /* Win32 GUI: stderr is wacki.log   */
void LogOpenFile(const char *path, const char *header);
void LogDumpRing(const char *reason);    /* ring → wacki.log                 */

#ifdef WACKI_VERBOSE
#  define LOG_TRACE(tag, ...) wacki_log(WL_TRACE, (tag), __VA_ARGS__)
#  define LOG_DEBUG(tag, ...) wacki_log(WL_DEBUG, (tag), __VA_ARGS__)
#else
   /* Release: compiled in, but only formatted when diagnostics are on
    * (g_log_diag) — one well-predicted branch otherwise. */
#  define LOG_TRACE(tag, ...) \
       do { if (g_log_diag) wacki_log(WL_TRACE, (tag), __VA_ARGS__); } while (0)
#  define LOG_DEBUG(tag, ...) \
       do { if (g_log_diag) wacki_log(WL_DEBUG, (tag), __VA_ARGS__); } while (0)
#endif

#endif /* WACKI_LOG_H */
