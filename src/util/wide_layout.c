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

/* "Sterowanie współczesne" (modern controls, g_modern_controls): no
 * sliding panel — the right button pops the inventory panel up in a frame
 * in the middle of the screen, a little smaller; Tab / wheel / pad combo
 * switch the character instead (scene_input.c). */
int             g_modern_controls = 0;   /* wacki.cfg modern_controls= */
#define POPUP_SCALE      0.8f
static int      s_popup;                 /* panel pop-up open            */
static int      s_popup_back_x, s_popup_back_y;   /* cursor before opening */
static uint16_t s_popup_held0;           /* held item when it opened     */
void (*g_pointer_warp_hook)(int gx, int gy) = NULL;  /* platform: move the OS pointer */

/* 16:10 within ~1.5%: the scene (640x400) fills the screen exactly. */
static int screen_is_16_10(int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    long d = (long)w * 10 - (long)h * 16;
    if (d < 0) d = -d;
    return d * 1000 <= (long)h * 16 * 15;
}

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
        if (screen_is_16_10(s_scr_w, s_scr_h)) {
            /* 16:10 (Steam Deck 1280x800, many laptops): the 640x400 scene
             * fits exactly — crop and bars are the same picture, skip one */
            strncpy(g_aspect_mode, "4:3", 15);
            msg = "Obraz: oryginalny 4:3";
        } else {
            strncpy(g_aspect_mode, "wide-bars", 15);
            msg = "Obraz: panorama z paskami po bokach";
        }
        break;
    case WIDE_BARS:
        strncpy(g_aspect_mode, "4:3", 15);
        msg = "Obraz: oryginalny 4:3";
        break;
    default:
        strncpy(g_aspect_mode, "wide", 15);
        msg = screen_is_16_10(s_scr_w, s_scr_h)
            ? "Obraz: panorama 16:10 (ca\xC5\x82""a scena)"
            : "Obraz: panorama (przyci\xC4\x99ta)";
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
int  WideLayoutPopupOpen(void)    { return s_active && s_popup; }

static void warp_to(int gx, int gy)
{
    extern int16_t g_mouse_x;
    g_mouse_x = (int16_t)gx;
    g_mouse_y = (int16_t)gy;
    s_cur_y   = gy;
    if (g_pointer_warp_hook) g_pointer_warp_hook(gx, gy);
}

static void popup_close(int restore)
{
    if (!s_popup) return;
    s_popup = 0;
    if (restore) warp_to(s_popup_back_x, s_popup_back_y);
}

/* Right button in modern-controls panorama: open / close the pop-up. */
void WideLayoutTogglePopup(void)
{
    extern int16_t  g_mouse_x;
    extern uint16_t g_held_item;
    if (!s_active) return;
    if (s_popup) { popup_close(1); return; }
    if (s_talking) return;
    s_popup = 1;
    s_popup_back_x = g_mouse_x;
    s_popup_back_y = g_mouse_y < SCENE_H ? g_mouse_y : SCENE_H - 1;
    s_popup_held0  = g_held_item;
    warp_to(FRAME_W / 2, SCENE_H + PANEL_H / 2);     /* into the panel */
}

/* A click outside the pop-up (scene_input.c): close it, swallow the click. */
int WideLayoutPopupClickOutside(void)
{
    if (!WideLayoutPopupOpen() || g_mouse_y >= SCENE_H) return 0;
    popup_close(0);
    return 1;
}

static int screen_is_wide(int w, int h)
{
    return w > 0 && h > 0 && w * 5 > h * 7;     /* wider than 7:5 */
}

void WideLayoutFrame(void)
{
    int was = s_active;
    /* panorama on a wide screen — or, with "sterowanie współczesne", on any
     * screen and in any aspect mode (then the scene without the panel strip,
     * fitted, and the panel pops up) */
    s_active = s_marked && !g_cutscene_playing && s_scr_w > 0 &&
               (g_modern_controls ||
                (WideAspectMode() != WIDE_OFF && screen_is_wide(s_scr_w, s_scr_h)));
    s_marked = 0;

    uint32_t now = SDL_GetTicks();
    uint32_t dt  = now - s_last_ms;
    s_last_ms = now;

    s_cur_y = g_mouse_y < 0 ? 0 : g_mouse_y >= FRAME_H ? FRAME_H - 1 : g_mouse_y;
    if (!s_active) { s_popup = 0; return; }

    if (g_modern_controls) {
        extern uint16_t g_held_item;
        s_t = 0.0f; s_target = 0;                  /* never slides */
        if (s_popup && s_talking) popup_close(1);
        /* an item picked up in the pop-up: back to the scene to use it */
        if (s_popup && g_held_item != s_popup_held0 &&
            g_held_item != 0 && g_held_item != 0x26) popup_close(1);
        if (!s_popup && s_cur_y >= SCENE_H) s_cur_y = SCENE_H - 1;
        return;
    }
    s_popup = 0;

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
    if (!s_active || scr_w <= 0 || scr_h <= 0) return 0;
    int fit = 0;                      /* whole scene, bars where needed */
    if (!screen_is_wide(scr_w, scr_h) || WideAspectMode() == WIDE_OFF) {
        if (!g_modern_controls) return 0;
        fit = 1;
    }

    float W = (float)scr_w, H = (float)scr_h;
    float L = SCENE_H + PANEL_H * (s_t * s_t * (3.0f - 2.0f * s_t)); /* ease */
    float s;
    if (fit || WideAspectMode() == WIDE_BARS) {
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

    if (g_modern_controls && s_popup) {             /* the pop-up panel */
        float ps = s * POPUP_SCALE;
        r->popup   = 1;
        r->p_scale = ps;
        r->p_w = FRAME_W * ps;
        r->p_h = PANEL_H * ps;
        r->p_x = (W - r->p_w) * 0.5f;
        r->p_y = (H - r->p_h) * 0.5f;
        r->p_border = s * 3.0f < 2.0f ? 2.0f : s * 3.0f;
    }
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
    if (r.popup && sx >= r.p_x && sx < r.p_x + r.p_w &&
        sy >= r.p_y && sy < r.p_y + r.p_h) {        /* inside the pop-up */
        int ix = (int)((sx - r.p_x) / r.p_scale);
        int iy = SCENE_H + (int)((sy - r.p_y) / r.p_scale);
        *gx = ix < 0 ? 0 : ix >= FRAME_W ? FRAME_W - 1 : ix;
        *gy = iy < SCENE_H ? SCENE_H : iy >= FRAME_H ? FRAME_H - 1 : iy;
        return 1;
    }
    float x = (sx - r.dst_x) / r.scale;
    /* screen_y = y·win·s/L  (see the header) */
    float y = (sy - r.dst_y) * r.band_l / (r.src_h * r.scale);
    int ix = (int)x, iy = (int)y;
    if (ix < 0) ix = 0;
    if (ix >= FRAME_W) ix = FRAME_W - 1;
    if (iy < 0) iy = 0;
    if (iy >= FRAME_H) iy = FRAME_H - 1;
    if (g_modern_controls && iy >= SCENE_H) iy = SCENE_H - 1;   /* no panel strip */
    *gx = ix; *gy = iy;
    return 1;
}

/* Game position → screen pixel (to move the OS pointer with it). */
int WideLayoutGameToScreen(int scr_w, int scr_h, int gx, int gy, float *sx, float *sy)
{
    WideRect r;
    if (!compute(scr_w, scr_h, s_cur_y, &r)) return 0;
    if (r.popup && gy >= SCENE_H) {
        *sx = r.p_x + (gx + 0.5f) * r.p_scale;
        *sy = r.p_y + (gy - SCENE_H + 0.5f) * r.p_scale;
        return 1;
    }
    *sx = r.dst_x + (gx + 0.5f) * r.scale;
    *sy = r.dst_y + (gy + 0.5f) * r.src_h * r.scale / r.band_l;
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

void ModernControlsToggle(void)
{
    extern void ConfigSave(void);
    g_modern_controls = !g_modern_controls;
    s_popup = 0;
    SubtitlesHint(g_modern_controls
        ? "Sterowanie wsp\xC3\xB3\xC5\x82""czesne: prawy przycisk = panel, Tab / k\xC3\xB3\xC5\x82""ko = zmiana postaci"
        : "Sterowanie klasyczne: prawy przycisk zmienia posta\xC4\x87", 3500);
    LOG_INFO("platform", "modern_controls=%d", g_modern_controls);
    ConfigSave();
}
