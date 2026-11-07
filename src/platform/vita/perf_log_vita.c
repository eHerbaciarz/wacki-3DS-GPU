/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/vita/perf_log_vita.c — see perf_log_vita.h. The Vita twin
 * of src/platform/3ds/perf_log_3ds.c, on SDL threads / mutex / timer
 * instead of libctru's. Field meanings: docs/vita-gpu.md. */

#include "perf_log_vita.h"
#include "wacki/log.h"

#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifndef WACKI_VERSION
#define WACKI_VERSION "dev"
#endif

#define PERF_LOG_PATH  "ux0:/data/wacki/wacki_perf.log"
#define PERF_WINDOW_MS 500
#define PERF_MSG_BUF   8192

typedef struct {
    uint32_t presents, drawn, skipped;
    uint64_t frame_sum, frame_max;
    uint64_t engine_sum, engine_max;
    uint64_t draw_sum, draw_max;
    uint64_t wait_sum, wait_max;
    uint64_t sleep_sum;
} PerfWindow;

static SDL_mutex  *s_lock;
static SDL_cond   *s_wake;
static SDL_Thread *s_thread;
static FILE       *s_file;
static volatile int s_stop;
static int         s_started;
static double      s_ticks_per_ms = 1.0;

static PerfWindow s_win;               /* guarded by s_lock */
static char       s_msgs[PERF_MSG_BUF];
static size_t     s_msgs_len;
static uint32_t   s_msgs_dropped;
static char       s_state[96];

/* main thread only */
static uint64_t   s_prev_present, s_sleep_pending, s_start;

uint64_t perf_now(void) { return SDL_GetPerformanceCounter(); }

static uint32_t tenths_ms(uint64_t ticks)
{
    return (uint32_t)((double)ticks * 10.0 / s_ticks_per_ms + 0.5);
}

static int fmt_ms(char *out, size_t n, uint64_t ticks)
{
    uint32_t t = tenths_ms(ticks);
    return snprintf(out, n, "%lu.%lu", (unsigned long)(t / 10), (unsigned long)(t % 10));
}

static void write_window(const PerfWindow *w, uint64_t now, uint64_t window,
                         const char *state)
{
    char a[16], b[16], line[512];
    int  n = 0;
    uint32_t t = (uint32_t)((double)(now - s_start) / (s_ticks_per_ms * 100.0));
    double   secs = (double)window / (s_ticks_per_ms * 1000.0);
    if (secs <= 0.0) secs = 0.5;
    unsigned fps10   = (unsigned)(w->drawn    / secs * 10.0 + 0.5);
    unsigned ticks10 = (unsigned)(w->presents / secs * 10.0 + 0.5);

    n += snprintf(line + n, sizeof line - n, "t=%lu.%lus fps=%u.%u ticks=%u.%u",
                  (unsigned long)(t / 10), (unsigned long)(t % 10),
                  fps10 / 10, fps10 % 10, ticks10 / 10, ticks10 % 10);
    if (w->presents) {
        uint32_t p = w->presents;
        fmt_ms(a, sizeof a, w->frame_sum / p);  fmt_ms(b, sizeof b, w->frame_max);
        n += snprintf(line + n, sizeof line - n, " frame=%s/%s", a, b);
        fmt_ms(a, sizeof a, w->engine_sum / p); fmt_ms(b, sizeof b, w->engine_max);
        n += snprintf(line + n, sizeof line - n, " engine=%s/%s", a, b);
        fmt_ms(a, sizeof a, w->sleep_sum / p);
        n += snprintf(line + n, sizeof line - n, " sleep=%s", a);
    }
    if (w->drawn) {
        uint32_t d = w->drawn;
        fmt_ms(a, sizeof a, w->draw_sum / d);   fmt_ms(b, sizeof b, w->draw_max);
        n += snprintf(line + n, sizeof line - n, " draw=%s/%s", a, b);
        fmt_ms(a, sizeof a, w->wait_sum / d);   fmt_ms(b, sizeof b, w->wait_max);
        n += snprintf(line + n, sizeof line - n, " wait=%s/%s", a, b);
    }
    n += snprintf(line + n, sizeof line - n, " skip=%lu", (unsigned long)w->skipped);
    if (state[0] && n < (int)sizeof line - 4)
        n += snprintf(line + n, sizeof line - n, " [%s]", state);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    fwrite(line, 1, (size_t)n, s_file);
}

static void flush_once(uint64_t *last)
{
    PerfWindow w;
    static char msgs[PERF_MSG_BUF];   /* writer thread only */
    size_t   msgs_len;
    uint32_t dropped;
    char     state[sizeof s_state];

    SDL_LockMutex(s_lock);
    w = s_win;
    memset(&s_win, 0, sizeof s_win);
    msgs_len = s_msgs_len;
    memcpy(msgs, s_msgs, msgs_len);
    s_msgs_len = 0;
    dropped = s_msgs_dropped;
    s_msgs_dropped = 0;
    memcpy(state, s_state, sizeof state);
    SDL_UnlockMutex(s_lock);

    uint64_t now = perf_now();
    if (s_file) {
        if (msgs_len) fwrite(msgs, 1, msgs_len, s_file);
        if (dropped) fprintf(s_file, "! (%lu messages dropped, buffer full)\n",
                             (unsigned long)dropped);
        if (w.presents || msgs_len) write_window(&w, now, now - *last, state);
        fflush(s_file);
    }
    *last = now;
}

static int writer_thread(void *arg)
{
    (void)arg;
    uint64_t last = perf_now();
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    SDL_LockMutex(s_lock);
    while (!s_stop) {
        SDL_CondWaitTimeout(s_wake, s_lock, PERF_WINDOW_MS);
        SDL_UnlockMutex(s_lock);
        flush_once(&last);
        SDL_LockMutex(s_lock);
    }
    SDL_UnlockMutex(s_lock);
    flush_once(&last);
    return 0;
}

static void sink(WackiLogLevel lvl, const char *tag, const char *msg)
{
    perf_log_msg("%s/%s: %s", lvl >= WL_ERROR ? "error" : "warn", tag, msg);
}

static void pace_sleep_ms(uint32_t ms)
{
    perf_log_sleep((uint64_t)((double)ms * s_ticks_per_ms));
}

void perf_log_init(void)
{
    if (s_started) return;
    s_lock = SDL_CreateMutex();
    s_wake = SDL_CreateCond();
    if (!s_lock || !s_wake) return;
    s_ticks_per_ms = (double)SDL_GetPerformanceFrequency() / 1000.0;
    s_start = s_prev_present = perf_now();
    s_started = 1;

    mkdir("ux0:/data", 0777);
    mkdir("ux0:/data/wacki", 0777);
    s_file = fopen(PERF_LOG_PATH, "w");
    if (s_file) {
        fprintf(s_file,
                "# wacki %s perf log (PS Vita, SDL2 GXM renderer)\n"
                "# per 0.5 s: avg/max ms; engine = frame - draw - wait - sleep\n",
                WACKI_VERSION);
        fflush(s_file);
    }
    s_stop = 0;
    s_thread = SDL_CreateThread(writer_thread, "wacki-perflog", NULL);
    if (!s_thread && s_file)
        fprintf(s_file, "! writer thread not started: written at exit only\n");
    g_log_sink = sink;
    {
        extern void (*g_pace_sleep_hook)(uint32_t ms);
        g_pace_sleep_hook = pace_sleep_ms;
    }
}

void perf_log_shutdown(void)
{
    if (!s_started) return;
    g_log_sink = NULL;
    {
        extern void (*g_pace_sleep_hook)(uint32_t ms);
        g_pace_sleep_hook = NULL;
    }
    if (s_thread) {
        SDL_LockMutex(s_lock);
        s_stop = 1;
        SDL_CondSignal(s_wake);
        SDL_UnlockMutex(s_lock);
        SDL_WaitThread(s_thread, NULL);
        s_thread = NULL;
    } else {
        uint64_t last = s_start;
        flush_once(&last);
    }
    if (s_file) { fclose(s_file); s_file = NULL; }
    s_started = 0;
}

void perf_log_present(int drew, uint64_t draw, uint64_t wait)
{
    if (!s_started) return;
    uint64_t now   = perf_now();
    uint64_t frame = now - s_prev_present;
    uint64_t sleep = s_sleep_pending;
    s_prev_present  = now;
    s_sleep_pending = 0;
    uint64_t busy   = draw + wait + sleep;
    uint64_t engine = frame > busy ? frame - busy : 0;

    SDL_LockMutex(s_lock);
    PerfWindow *w = &s_win;
    w->presents++;
    w->frame_sum += frame;   if (frame  > w->frame_max)  w->frame_max  = frame;
    w->engine_sum += engine; if (engine > w->engine_max) w->engine_max = engine;
    w->sleep_sum += sleep;
    if (drew) {
        w->drawn++;
        w->draw_sum += draw; if (draw > w->draw_max) w->draw_max = draw;
        w->wait_sum += wait; if (wait > w->wait_max) w->wait_max = wait;
    } else {
        w->skipped++;
    }
    SDL_UnlockMutex(s_lock);
}

void perf_log_sleep(uint64_t ticks) { s_sleep_pending += ticks; }

void perf_log_msg(const char *fmt, ...)
{
    if (!s_started) return;
    char line[256];
    uint32_t t = (uint32_t)((double)(perf_now() - s_start) / (s_ticks_per_ms * 100.0));
    int n = snprintf(line, sizeof line, "! t=%lu.%lus ", (unsigned long)(t / 10),
                     (unsigned long)(t % 10));
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';

    SDL_LockMutex(s_lock);
    if (s_msgs_len + (size_t)n <= sizeof s_msgs) {
        memcpy(s_msgs + s_msgs_len, line, (size_t)n);
        s_msgs_len += (size_t)n;
    } else {
        s_msgs_dropped++;
    }
    SDL_UnlockMutex(s_lock);
}

void perf_log_set_state(const char *state)
{
    if (!s_started) return;
    SDL_LockMutex(s_lock);
    if (state) {
        strncpy(s_state, state, sizeof s_state - 1);
        s_state[sizeof s_state - 1] = '\0';
    } else {
        s_state[0] = '\0';
    }
    SDL_UnlockMutex(s_lock);
}
