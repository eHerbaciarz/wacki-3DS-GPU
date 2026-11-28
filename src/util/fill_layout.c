/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/util/fill_layout.c — the "stretch" picture mode of the plain Wacki
 * edition, shared by the SDL (PC / Switch), Vita and 3DS GPU backends.
 *
 * The game frame is 640×480: the scene is rows 0..399 and the inventory
 * panel rows 400..479. The old "stretch" squashed the whole frame to the
 * screen (on 16:9 everything 33% too wide). Now, while the game is being
 * played (the HUD panel was painted into this frame):
 *
 *   - the panel is drawn exactly as in 4:3 mode — same size, centred at
 *     the bottom, black at its sides;
 *   - the scene fills the whole width above it. Its top FILL_CROP_TOP
 *     rows are cut off, so what is left (640×360) is a 16:9 picture,
 *     stretched only a little to the space left above the panel
 *     (Switch 1280×720: 2.0 × 1.67, Vita: 1.5 × 1.26, 3DS top: 0.625 ×
 *     0.556 — about 12-20% wider than the original instead of 33%).
 *
 * Why the top and not the bottom: checked in every room of stages 1-4
 * (each room's clickable spots and floor mapped with ClickHitTest /
 * is_walkable_at): the characters walk down to row ~392, so the bottom
 * rows are needed everywhere, while the top 40 rows are sky, ceilings and
 * the upper ends of things that reach further down (lianas, a net, a
 * power line) — still clickable below row 40. The cursor can't go above
 * row FILL_CROP_TOP in the scene (FillLayoutMinY).
 *
 * Films, menus and screens without the panel are shown in 4:3 with bars
 * (on the 3DS: its own proportional "fill" view, as before). Screens not
 * wider than ~7:5 get plain 4:3. The aspect button still toggles
 * stretch ↔ 4:3 (g_aspect_mode, saved in wacki.cfg). */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/api.h"

#include <stdint.h>
#include <string.h>

#define FRAME_W   640
#define FRAME_H   480
#define SCENE_H   400
#define PANEL_H   (FRAME_H - SCENE_H)

extern char     g_aspect_mode[16];
extern int16_t  g_mouse_y;
extern int      g_cutscene_playing;

static int s_marked;        /* HUD panel painted since the last present */
static int s_gameplay;      /* this frame is a gameplay frame           */
static int s_drawn;         /* a backend drew the layout since last frame */
static int s_on_screen;     /* ... so the top rows really are cut off     */

void FillLayoutMarkGameplay(void) { s_marked = 1; }
void FillLayoutKeep(void)         { if (s_gameplay) s_marked = 1; }

int FillLayoutStretchMode(void)
{
    return g_aspect_mode[0] != '4';
}

static int screen_is_wide(int w, int h)
{
    return w > 0 && h > 0 && w * 5 > h * 7;
}

void FillLayoutFrame(void)
{
    s_gameplay = s_marked && !g_cutscene_playing && FillLayoutStretchMode();
    s_marked = 0;
    /* Only where a backend really shows the layout (SDL, Vita, 3DS GPU on
     * a wide screen — not Android, PS2, handhelds with 4:3 screens...). */
    s_on_screen = s_gameplay && s_drawn;
    s_drawn = 0;
    /* the top rows of the scene aren't on the screen: keep the cursor out */
    if (s_on_screen && g_mouse_y >= 0 && g_mouse_y < FILL_CROP_TOP)
        g_mouse_y = FILL_CROP_TOP;
}

int FillLayoutMinY(void)
{
    return s_on_screen ? FILL_CROP_TOP : 0;
}

int FillLayoutGet(int scr_w, int scr_h, FillRects *r)
{
    memset(r, 0, sizeof *r);
    if (!s_gameplay || !screen_is_wide(scr_w, scr_h)) return 0;
    float W = (float)scr_w, H = (float)scr_h;
    float s = H / FRAME_H;                       /* 4:3 scale */
    if (FRAME_W * s > W) s = W / FRAME_W;
    r->p_w = FRAME_W * s;
    r->p_h = PANEL_H * s;
    r->p_x = (W - r->p_w) * 0.5f;
    r->p_y = H - r->p_h;
    r->s_x = 0.0f;
    r->s_y = 0.0f;
    r->s_w = W;
    r->s_h = H - r->p_h;
    r->src_y = (float)FILL_CROP_TOP;
    r->src_h = (float)(SCENE_H - FILL_CROP_TOP);
    s_drawn = 1;
    return 1;
}

int FillLayoutScreenToGame(int scr_w, int scr_h, float sx, float sy,
                           int *gx, int *gy)
{
    FillRects r;
    if (!FillLayoutGet(scr_w, scr_h, &r)) return 0;
    float x, y;
    if (sy >= r.p_y) {                           /* the panel */
        x = (sx - r.p_x) * FRAME_W / r.p_w;
        y = SCENE_H + (sy - r.p_y) * PANEL_H / r.p_h;
    } else {                                     /* the scene */
        x = (sx - r.s_x) * FRAME_W / r.s_w;
        y = r.src_y + (sy - r.s_y) * r.src_h / r.s_h;
    }
    int ix = (int)x, iy = (int)y;
    if (ix < 0) ix = 0;
    if (ix >= FRAME_W) ix = FRAME_W - 1;
    if (iy < FILL_CROP_TOP) iy = FILL_CROP_TOP;
    if (iy >= FRAME_H) iy = FRAME_H - 1;
    *gx = ix; *gy = iy;
    return 1;
}
