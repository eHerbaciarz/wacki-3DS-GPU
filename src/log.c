/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/log.c — wacki_log + the runtime severity threshold.
 *
 * One line per call; format is `[level/tag] message\n`. Call sites
 * use the LOG_* macros in include/wacki/log.h. */

#include "wacki/log.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>   /* stderr is dropped on Android — route to logcat */
#endif

/* Default severity threshold. Release builds ship QUIET — only WARN/ERROR
 * reach stderr/logcat — so the distributed app doesn't spam a wall of
 * [info/...] breadcrumbs at end users. Dev builds (-DWACKI_VERBOSE, i.e.
 * `make debug`) default to INFO so those breadcrumbs are on while hacking.
 * Either default is overridable at runtime: `-v`/`-q` flags or the
 * WACKI_LOG_LEVEL env var (see src/main.c). */
#ifdef WACKI_VERBOSE
WackiLogLevel g_log_min_level = WL_INFO;
#else
WackiLogLevel g_log_min_level = WL_WARN;
#endif

#ifndef __ANDROID__
static const char *const k_level_name[] = {
    "trace",   /* WL_TRACE */
    "debug",   /* WL_DEBUG */
    "info",    /* WL_INFO  */
    "warn",    /* WL_WARN  */
    "error",   /* WL_ERROR */
};
#endif

void (*g_log_sink)(WackiLogLevel lvl, const char *tag, const char *msg) = NULL;

/* ---- dziennik (diagnostic log) --------------------------------------
 *
 * Besides stderr, every line from INFO up goes to wacki.log (next to
 * wacki.cfg; the previous run is kept as wacki.old.log), flushed per line
 * so it survives a crash. With diagnostics on (wacki.cfg diagnostyka=1,
 * --diag or WACKI_LOG_LEVEL=trace) also TRACE/DEBUG lines — the per-click,
 * per-script-op breadcrumbs — are kept, but only in a RAM ring of the last
 * LOG_RING_LINES lines; LogDumpRing() appends that ring to wacki.log (F3,
 * pad combo, crash). Release builds compile LOG_TRACE/LOG_DEBUG in too,
 * behind the g_log_diag check, so a user can switch them on.
 */
int             g_log_diag = 0;
uint32_t      (*g_log_clock)(void) = NULL;     /* ms; main.c: SDL_GetTicks */
int             g_log_stderr_is_file = 0;      /* Win32 GUI: stderr = wacki.log */

#define LOG_RING_LINES 2048
#define LOG_LINE_MAX   200
static char      s_ring[LOG_RING_LINES][LOG_LINE_MAX];
static unsigned  s_ring_next;
static FILE     *s_file;

static void put_file_line(const char *line)
{
    FILE *f = s_file ? s_file : (g_log_stderr_is_file ? stderr : NULL);
    if (!f) return;
    fputs(line, f);
    fputc('\n', f);
    fflush(f);
}

void LogOpenFile(const char *path, const char *header)
{
    if (!g_log_stderr_is_file && !s_file && path) {
        char old[300];
        snprintf(old, sizeof old, "%.*s.old.log", (int)(strlen(path) > 4 ? strlen(path) - 4 : strlen(path)), path);
        remove(old);
        rename(path, old);
        s_file = fopen(path, "w");
    }
    if (header) put_file_line(header);
}

void LogDumpRing(const char *reason)
{
    char head[160];
    snprintf(head, sizeof head, "==== zrzut dziennika (%s): ostatnie wpisy, od najstarszego ====",
             reason ? reason : "?");
    put_file_line(head);
    unsigned n = s_ring_next, start = n > LOG_RING_LINES ? n - LOG_RING_LINES : 0;
    for (unsigned i = start; i < n; ++i)
        if (s_ring[i % LOG_RING_LINES][0]) put_file_line(s_ring[i % LOG_RING_LINES]);
    put_file_line("==== koniec zrzutu ====");
}

void wacki_log(WackiLogLevel lvl, const char *tag, const char *fmt, ...)
{
    if (lvl < WL_TRACE || lvl > WL_ERROR) lvl = WL_INFO;
    int to_ring   = g_log_diag || lvl >= WL_INFO;
    int to_file   = lvl >= WL_INFO && (s_file || g_log_stderr_is_file);
    int to_stderr = lvl >= g_log_min_level && !(g_log_stderr_is_file && to_file);
    int to_sink   = g_log_sink && lvl >= WL_WARN;
    if (!to_ring && !to_file && !to_stderr && !to_sink) return;

    char    msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!tag) tag = "?";

    if (to_sink) g_log_sink(lvl, tag, msg);

#ifndef __ANDROID__
    static const char *const k_lv[] = { "trace", "debug", "info", "warn", "error" };
    char line[640];
    uint32_t ms = g_log_clock ? g_log_clock() : 0;
    snprintf(line, sizeof line, "%5u.%03u [%s/%s] %s",
             (unsigned)(ms / 1000), (unsigned)(ms % 1000), k_lv[lvl], tag, msg);
    if (to_ring) {
#ifdef WACKI_PS2
        /* EE has no __atomic_fetch_add_4 in libgcc; the PS2 threads are
         * cooperative, so a plain bump is enough there. */
        unsigned i = (s_ring_next++) % LOG_RING_LINES;
#else
        unsigned i = __atomic_fetch_add(&s_ring_next, 1, __ATOMIC_RELAXED) % LOG_RING_LINES;
#endif
        snprintf(s_ring[i], LOG_LINE_MAX, "%s", line);
    }
    if (to_file) put_file_line(line);
    if (to_stderr) fprintf(stderr, "[%s/%s] %s\n", k_level_name[lvl], tag, msg);
#else
    (void)to_ring; (void)to_file;
    /* Android drops native stderr; go through the logcat API instead so the
     * port's logs (and bug reports) are visible via `adb logcat -s wacki`. */
    static const int k_android_prio[] = {
        ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO,
        ANDROID_LOG_WARN,    ANDROID_LOG_ERROR,
    };
    if (to_stderr) __android_log_print(k_android_prio[lvl], "wacki", "[%s] %s", tag, msg);
#endif
}
