/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/vita/perf_log_vita.h — performance log on the memory card
 * (ux0:/data/wacki/wacki_perf.log). Same lines as the 3DS one
 * (src/platform/3ds/perf_log_3ds.c), so the two ports compare 1:1:
 *
 *   t=12.5s fps=30.0 ticks=30.0 frame=33.3/41.2 engine=9.1/14.0
 *   sleep=21.0 draw=2.3/3.1 wait=0.4/1.2 skip=0 [gpu smooth=1 ...]
 *
 * The main thread only adds to counters; a low-priority SDL thread writes
 * the file every 0.5 s. On the Vita "wait" is the time SDL_RenderPresent
 * took (GXM end of scene + display queue); there is no separate GPU time. */
#ifndef WACKI_PERF_LOG_VITA_H
#define WACKI_PERF_LOG_VITA_H

#include <stdint.h>

void     perf_log_init(void);
void     perf_log_shutdown(void);
uint64_t perf_now(void);                 /* SDL_GetPerformanceCounter */
void     perf_log_present(int drew, uint64_t draw, uint64_t wait);
void     perf_log_sleep(uint64_t ticks); /* see g_pace_sleep_hook (timer.c) */
void     perf_log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void     perf_log_set_state(const char *state);

#endif /* WACKI_PERF_LOG_VITA_H */
