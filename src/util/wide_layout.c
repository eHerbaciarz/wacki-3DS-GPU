/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/util/wide_layout.c — panoramic (widescreen) picture with a sliding
 * inventory panel, shared by every backend that can scale the frame
 * (SDL: PC/Switch, Vita, 3DS GPU).
 *
 * The game frame is 640×480: the scene is rows 0..399 (16:10) and the
 * inventory panel rows 400..479. On a wide screen the old "stretch"
 * mode squashed all of that to 16:9. Instead, while the game is being
 * played (the HUD was painted into this frame) the picture becomes a
 * vertical WINDOW into the frame:
 *
 *   - the panel is hidden: the window may only cover rows 0..L with
 *     L = 400; it slides in (L → 480, ~0.2 s) when the cursor reaches the
 *     bottom edge (row ≥ 396) and hides again once the cursor goes back
 *     up (row < 330). While a dialogue line is being spoken it stays
 *     hidden;
 *   - "wide" (version 1, crop): the window fills the screen width, so a
 *     few rows don't fit (16:9: 360 of 400) and the window pans with the
 *     cursor;
 *   - "wide-bars" (version 2): the whole 640×400 scene fits the height,
 *     with narrow bars at the sides.
 *
 * The pan is proportional: window top y0 = cursor_y·(L − win)/L. Then the
 * cursor's position on the screen is simply proportional to its game
 * row (screen_y = cursor_y·H/L), so mapping a mouse / touch position back
 * to the game never feeds back into the pan and never drifts.
 *
 * Films, menus and screens without the panel stay 4:3, as does a screen
 * that isn't wider than ~7:5. The aspect button cycles
 * wide → wide-bars → 4:3 (g_aspect_mode, saved in wacki.cfg).
 */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/api.h"

#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FRAME_W          640
#define FRAME_H          480
#define SCENE_H          400        /* panel starts here */
#define PANEL_H          (FRAME_H - SCENE_H)
#define REVEAL_Y         396        /* cursor row that brings the panel in */
#define HIDE_Y           330        /* ... and takes it away again */
#define SLIDE_MS         200

extern char     g_aspect_mode[16];
extern int16_t  g_mouse_y;
extern int      g_cutscene_playing;

static int      s_marked;           /* HUD painted since last flush       */
static int      s_active;           /* this frame uses the wide layout    */
static int      s_talking;          /* a dialogue line is playing         */
static int      s_target;           /* panel wanted (1) / hidden (0)      */
static float    s_t;                /* panel slide 0..1                   */
static uint32_t s_last_ms;
static int      s_cur_y;            /* cursor row this frame              */
static int      s_scr_w = 0, s_scr_h = 0;

int WideAspectMode(void)
{
    if (g_aspect_mode[0] == '4') return WIDE_OFF;
    if (strncmp(g_aspect_mode, "wide-bars", 9) == 0) return WIDE_BARS;
    return WIDE_CROP;               /* "wide", legacy "stretch" */
}

void WideAspectCycle(void)
{
    extern void ConfigSave(void);
    const char *msg;
    switch (WideAspectMode()) {
    case WIDE_CROP:
        strncpy(g_aspect_mode, "wide-bars", 15);
        msg = "Obraz: panorama z paskami po bokach";
        break;
    case WIDE_BARS:
        strncpy(g_aspect_mode, "4:3", 15);
        msg = "Obraz: oryginalny 4:3";
        break;
    default:
        strncpy(g_aspect_mode, "wide", 15);
        msg = "Obraz: panorama (przyci\xC4\x99ta)";
        break;
    }
    g_aspect_mode[15] = '\0';
    LOG_INFO("platform", "aspect_mode=%s", g_aspect_mode);
    SubtitlesHint(msg, 2000);
    ConfigSave();
}

void WideLayoutMarkGameplay(void) { s_marked = 1; }
void WideLayoutKeep(void)         { if (s_active) s_marked = 1; }
void WideLayoutSetTalking(int on) { s_talking = on ? 1 : 0; }
void WideLayoutSetScreen(int w, int h) { s_scr_w = w; s_scr_h = h; }
int  WideLayoutActive(void)       { return s_active; }

static int screen_is_wide(int w, int h)
{
    return w > 0 && h > 0 && w * 5 > h * 7;     /* wider than 7:5 */
}

void WideLayoutFrame(void)
{
    int was = s_active;
    s_active = s_marked && !g_cutscene_playing && WideAspectMode() != WIDE_OFF
            && (s_scr_w == 0 || screen_is_wide(s_scr_w, s_scr_h));
    s_marked = 0;

    uint32_t now = SDL_GetTicks();
    uint32_t dt  = now - s_last_ms;
    s_last_ms = now;

    s_cur_y = g_mouse_y < 0 ? 0 : g_mouse_y >= FRAME_H ? FRAME_H - 1 : g_mouse_y;
    if (!s_active) return;

    if (s_talking)                 s_target = 0;
    else if (s_cur_y >= REVEAL_Y)  s_target = 1;
    else if (s_cur_y <  HIDE_Y)    s_target = 0;

    if (!was) { s_t = (float)s_target; return; }   /* no slide on entry */
    if (dt > 100) dt = 100;
    float step = (float)dt / SLIDE_MS;
    if (s_target) { s_t += step; if (s_t > 1.0f) s_t = 1.0f; }
    else          { s_t -= step; if (s_t < 0.0f) s_t = 0.0f; }
}

/* Geometry for a screen of scr_w×scr_h pixels. */
static int compute(int scr_w, int scr_h, int cur_y, WideRect *r)
{
    memset(r, 0, sizeof *r);
    if (!s_active || !screen_is_wide(scr_w, scr_h)) return 0;

    float W = (float)scr_w, H = (float)scr_h;
    float L = SCENE_H + PANEL_H * (s_t * s_t * (3.0f - 2.0f * s_t)); /* ease */
    float s;
    if (WideAspectMode() == WIDE_BARS) {
        s = H / SCENE_H;
        if (FRAME_W * s > W) s = W / FRAME_W;
    } else {
        s = W / FRAME_W;
    }
    float win = H / s;
    if (win > L) win = L;
    float y0 = (float)cur_y * (L - win) / L;
    if (y0 < 0) y0 = 0;
    if (y0 > L - win) y0 = L - win;

    r->active = 1;
    r->scale  = s;
    r->band_l = L;
    r->src_x = 0;   r->src_w = FRAME_W;
    r->src_y = y0;  r->src_h = win;
    r->dst_w = FRAME_W * s;
    r->dst_h = win * s;
    r->dst_x = (W - r->dst_w) * 0.5f;
    r->dst_y = (H - r->dst_h) * 0.5f;
    return 1;
}

int WideLayoutGet(int scr_w, int scr_h, WideRect *r)
{
    if (scr_w != s_scr_w || scr_h != s_scr_h) WideLayoutSetScreen(scr_w, scr_h);
    return compute(scr_w, scr_h, s_cur_y, r);
}

int WideLayoutScreenToGame(int scr_w, int scr_h, float sx, float sy,
                           int *gx, int *gy)
{
    WideRect r;
    if (!compute(scr_w, scr_h, s_cur_y, &r)) return 0;
    float x = (sx - r.dst_x) / r.scale;
    /* screen_y = y·win·s/L  (see the header) */
    float y = (sy - r.dst_y) * r.band_l / (r.src_h * r.scale);
    int ix = (int)x, iy = (int)y;
    if (ix < 0) ix = 0;
    if (ix >= FRAME_W) ix = FRAME_W - 1;
    if (iy < 0) iy = 0;
    if (iy >= FRAME_H) iy = FRAME_H - 1;
    *gx = ix; *gy = iy;
    return 1;
}

int WideLayoutBand(int *top, int *bottom)
{
    WideRect r;
    if (!s_scr_w || !compute(s_scr_w, s_scr_h, s_cur_y, &r)) return 0;
    *top    = (int)(r.src_y + 0.5f);
    *bottom = (int)(r.src_y + r.src_h);
    if (*bottom > SCENE_H) *bottom = SCENE_H;
    return 1;
}
