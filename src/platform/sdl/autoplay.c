/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/autoplay.c — scripted input for testing / bug hunts.
 *
 * WACKI_AUTOPLAY=file.txt makes the game read commands from the file, one
 * per line, in game coordinates (640×480), and run them in order from
 * PlatformPumpEvents():
 *
 *   wait MS          let MS milliseconds of game time pass
 *   move X Y         put the cursor at X,Y
 *   click X Y        move + left click
 *   rclick X Y       move + right click
 *   key NAME         F1..F12, esc, enter, tab, space
 *   shot NAME        save the current frame as NAME.bmp (8-bit, palette)
 *   swap             switch character (like Tab)
 *   room N           jump to komnata N of the stage (tests only)
 *   diag             diagnostics on (log ring)
 *   dump             write the log ring to wacki.log
 *   mem              log heap in use (glibc only)
 *   quit             leave the game
 *   # ...            comment
 *
 * Off unless the variable is set. Desktop SDL builds only.
 */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/api.h"

#include <SDL.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE    *s_fp;
static int      s_state = -1;          /* -1 not started, 0 off, 1 running */
static uint32_t s_wait_until;

static int keycode_of(const char *n)
{
    if ((n[0] == 'F' || n[0] == 'f') && n[1]) {
        int k = atoi(n + 1);
        if (k >= 1 && k <= 12) return SDLK_F1 + (k - 1);
    }
    if (!strcmp(n, "esc"))   return SDLK_ESCAPE;
    if (!strcmp(n, "enter")) return SDLK_RETURN;
    if (!strcmp(n, "tab"))   return SDLK_TAB;
    if (!strcmp(n, "space")) return SDLK_SPACE;
    return 0;
}

static void save_shot(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "%s.bmp", name);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(
        g_back_shadow, WACKI_SCREEN_W, WACKI_SCREEN_H, 8, WACKI_SCREEN_W,
        SDL_PIXELFORMAT_INDEX8);
    if (!s) return;
    SDL_Color c[256];
    for (int i = 0; i < 256; ++i) {
        c[i].r = g_palette_rgb[i * 3]; c[i].g = g_palette_rgb[i * 3 + 1];
        c[i].b = g_palette_rgb[i * 3 + 2]; c[i].a = 255;
    }
    SDL_SetPaletteColors(s->format->palette, c, 0, 256);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    LOG_INFO("autoplay", "shot %s", path);
}

void AutoplayTick(void)
{
    static int slow_ms = -1;
    if (slow_ms < 0) {                     /* WACKI_SLOW_MS: fake a slow device */
        const char *e = getenv("WACKI_SLOW_MS");
        slow_ms = e ? atoi(e) : 0;
    }
    if (slow_ms > 0) SDL_Delay((uint32_t)slow_ms);
    if (s_state == 0) return;
    if (s_state < 0) {
        const char *f = getenv("WACKI_AUTOPLAY");
        s_fp = (f && *f) ? fopen(f, "r") : NULL;
        s_state = s_fp ? 1 : 0;
        if (!s_fp) return;
        LOG_INFO("autoplay", "script %s", f);
    }
    uint32_t now = SDL_GetTicks();
    if ((int32_t)(now - s_wait_until) < 0) return;

    char line[256];
    while (fgets(line, sizeof line, s_fp)) {
        char cmd[32] = "", arg[64] = "";
        int x = 0, y = 0;
        if (line[0] == '#' || sscanf(line, "%31s", cmd) != 1) continue;
        LOG_INFO("autoplay", "> %s", strtok(line, "\r\n") ? line : "");
        if (!strcmp(cmd, "wait") && sscanf(line, "%*s %d", &x) == 1) {
            s_wait_until = now + (uint32_t)x;
            return;                            /* resume after the wait */
        } else if ((!strcmp(cmd, "move") || !strcmp(cmd, "click") || !strcmp(cmd, "rclick")) &&
                   sscanf(line, "%*s %d %d", &x, &y) == 2) {
            g_mouse_x = (int16_t)x; g_mouse_y = (int16_t)y;
            if (cmd[0] == 'c') g_lmb_clicked = 1;
            if (cmd[0] == 'r') g_rmb_clicked = 1;
            return;                            /* one input per frame */
        } else if (!strcmp(cmd, "key") && sscanf(line, "%*s %63s", arg) == 1) {
            SDL_Event ev; memset(&ev, 0, sizeof ev);
            ev.type = SDL_KEYDOWN; ev.key.keysym.sym = keycode_of(arg);
            SDL_PushEvent(&ev);
            ev.type = SDL_KEYUP;   SDL_PushEvent(&ev);
            return;
        } else if (!strcmp(cmd, "shot") && sscanf(line, "%*s %63s", arg) == 1) {
            save_shot(arg);
        } else if (!strcmp(cmd, "swap")) {
            g_swap_actor_request = 1;
            return;
        } else if (!strcmp(cmd, "room") && sscanf(line, "%*s %d", &x) == 1) {
            extern void ScriptGoToKomnata(uint16_t id);
            ScriptGoToKomnata((uint16_t)x);        /* jump (tests only) */
            return;
        } else if (!strcmp(cmd, "vars") && sscanf(line, "%*s %d %d", &x, &y) == 2) {
            extern uint32_t g_script_vars[];
            for (int i = x; i <= y; ++i)
                if (g_script_vars[i]) LOG_INFO("autoplay", "var[0x%X]=0x%X", i, (unsigned)g_script_vars[i]);
        } else if (!strcmp(cmd, "diag")) {
            g_log_diag = 1;
        } else if (!strcmp(cmd, "mem")) {
#ifdef __GLIBC__
            struct mallinfo2 mi = mallinfo2();
            LOG_INFO("autoplay", "mem in-use=%zu KB mmap=%zu KB", mi.uordblks / 1024, mi.hblkhd / 1024);
#endif
        } else if (!strcmp(cmd, "dump")) {
            LogDumpRing("autoplay");
        } else if (!strcmp(cmd, "quit")) {
            SDL_Event ev; memset(&ev, 0, sizeof ev);
            ev.type = SDL_QUIT; SDL_PushEvent(&ev);
            return;
        }
    }
    fclose(s_fp);
    s_fp = NULL;
    s_state = 0;
    LOG_INFO("autoplay", "script finished");
}
