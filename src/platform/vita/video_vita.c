/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 * Vita speed-ups after ReClaw's reclaw-vita-gpu notes (Herbaciarz, GPL-3.0).
 *
 * src/platform/vita/video_vita.c — PS Vita video HAL on SDL2's GXM (GPU)
 * renderer. Replaces the shared src/platform/sdl/video_sdl.c on this
 * target only; desktop / Switch / handhelds keep that one.
 *
 * The Vita's SDL2 already draws with the GPU (GXM) — scaling 640×480 to
 * the 960×544 screen costs the CPU nothing. What was slow was the CPU side
 * of every frame in video_sdl.c: a 32-bit ARGB texture filled pixel by
 * pixel with three palette reads each (1.2 MB written per frame), and
 * every frame uploaded even when nothing changed. Here:
 *
 *   - RGB565 streaming texture: half the bytes; the palette becomes a
 *     256-entry RGB565 table built only when the palette changes, and the
 *     frame is converted 4 pixels per step (one 32-bit read, two writes);
 *   - TWO textures used in turn: SDL's GXM LockTexture hands out the
 *     texture's own memory with no wait, and with vsync off the GPU may
 *     still be reading last frame's texture — writing into the other one
 *     avoids tearing/garbage in the picture;
 *   - frames identical to the previous one (frame, palette, mode) are not
 *     drawn again at all; the screen keeps showing the last one;
 *   - smoothing (linear filtering) on by default, following the FIRST
 *     button of the Grafika menu (s_opt_gfx1 / video_mode) like the 3DS
 *     GPU backend; it changes the moment the button is clicked;
 *   - 4:3 (725×544, centred, black bars) or stretch (960×544), as before
 *     (Y button / F10); touch maps through the same rectangle
 *     (vita_video_touch_to_game, used by touch_vita.c);
 *   - every number goes to ux0:/data/wacki/wacki_perf.log
 *     (perf_log_vita.c), in the same format as the 3DS log. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/video.h"
#include "perf_log_vita.h"

#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 960
#define SCREEN_H 544
#define NUM_TEX  2

static SDL_Window   *s_win = NULL;
static SDL_Renderer *s_ren = NULL;
static SDL_Texture  *s_tex[NUM_TEX];
static int           s_cur = 0;          /* texture shown last */
static int           s_fb_w = 0, s_fb_h = 0;
static uint16_t      s_lut[256];
static uint8_t      *s_prev = NULL;      /* previous frame, for the skip */
static uint8_t       s_prev_pal[256 * 3];
static int           s_have_prev = 0;
static int           s_smooth = -1;
static SDL_Rect      s_game_rect = { 0, 0, SCREEN_W, SCREEN_H };

extern char g_aspect_mode[16];

static int g_stretch_active = 0;

static void compute_game_rect(void)
{
    if (g_aspect_mode[0] == '4') {
        /* 4:3 at full height: 544 * 4/3 = 725.33 → 725, centred. */
        int w = (SCREEN_H * 4 + 1) / 3;
        if (w > SCREEN_W) w = SCREEN_W;
        s_game_rect.x = (SCREEN_W - w) / 2;
        s_game_rect.y = 0;
        s_game_rect.w = w;
        s_game_rect.h = SCREEN_H;
        g_stretch_active = 0;
    } else {
        s_game_rect.x = 0;
        s_game_rect.y = 0;
        s_game_rect.w = SCREEN_W;
        s_game_rect.h = SCREEN_H;
        g_stretch_active = 1;
    }
}

/* Front touch panel (normalised 0..1 over the whole screen) → game
 * coordinates through the rectangle the picture is drawn in. Touches on
 * the black bars land on the nearest edge. */
void vita_video_touch_to_game(float nx, float ny, int *gx, int *gy)
{
    int w = s_fb_w > 0 ? s_fb_w : 640, h = s_fb_h > 0 ? s_fb_h : 480;
    float px = nx * (float)SCREEN_W, py = ny * (float)SCREEN_H;
    int x = (int)((px - (float)s_game_rect.x) * (float)w / (float)s_game_rect.w);
    int y = (int)((py - (float)s_game_rect.y) * (float)h / (float)s_game_rect.h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    *gx = x;
    *gy = y;
}

void platform_video_get_present_state(int *stretch_active,
                                      int *win_w, int *win_h,
                                      int *fb_w,  int *fb_h)
{
    *stretch_active = g_stretch_active;
    *fb_w = s_fb_w; *fb_h = s_fb_h;
    *win_w = SCREEN_W; *win_h = SCREEN_H;
}

void platform_video_toggle_aspect_mode(void)
{
    if (g_aspect_mode[0] == '4')
        strncpy(g_aspect_mode, "stretch", 15);
    else
        strncpy(g_aspect_mode, "4:3", 15);
    g_aspect_mode[15] = '\0';
    compute_game_rect();
    s_have_prev = 0;                     /* draw the next frame for sure */
    LOG_INFO("platform", "aspect_mode=%s", g_aspect_mode);
    extern void ConfigSave(void);
    ConfigSave();
}

unsigned plat_video_sdl_init_flags(void)
{
    return SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO;
}

static void set_state_note(void)
{
    char note[96];
    snprintf(note, sizeof note, "gpu smooth=%d aspect=%s", s_smooth,
             g_aspect_mode[0] == '4' ? "4:3" : "stretch");
    perf_log_set_state(note);
}

static void apply_smooth(int on)
{
    for (int i = 0; i < NUM_TEX; ++i)
        if (s_tex[i])
            SDL_SetTextureScaleMode(s_tex[i], on ? SDL_ScaleModeLinear
                                                 : SDL_ScaleModeNearest);
}

int plat_video_init(int w, int h, const char *title)
{
    perf_log_init();
    plat_apply_video_prefs();

    s_fb_w = w; s_fb_h = h;

    s_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
    if (!s_win) { LOG_ERROR("vita", "SDL_CreateWindow: %s", SDL_GetError()); return 0; }

    /* The GPU renderer; vsync off as before (plat_apply_video_prefs) —
     * the engine holds 30 fps itself (EnginePaceFrame). */
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    if (!s_ren) {
        LOG_ERROR("vita", "no GPU renderer (%s) — software", SDL_GetError());
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!s_ren) { LOG_ERROR("vita", "SDL_CreateRenderer: %s", SDL_GetError()); return 0; }

    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(s_ren, &info) == 0) {
        LOG_INFO("vita", "renderer: %s", info.name);
        perf_log_msg("renderer: %s%s", info.name,
                     (info.flags & SDL_RENDERER_ACCELERATED) ? " (GPU)" : " (SOFTWARE!)");
    }

    for (int i = 0; i < NUM_TEX; ++i) {
        s_tex[i] = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565,
                                     SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!s_tex[i]) {
            LOG_ERROR("vita", "SDL_CreateTexture: %s", SDL_GetError());
            return 0;
        }
    }

    s_prev = (uint8_t *)malloc((size_t)w * h);
    if (!s_prev) { LOG_ERROR("vita", "no memory for the previous frame"); return 0; }
    memset(s_prev, 0, (size_t)w * h);

    compute_game_rect();
    SDL_ShowCursor(SDL_DISABLE);
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderClear(s_ren);
    SDL_RenderPresent(s_ren);
    SDL_PumpEvents();

    LOG_INFO("platform", "Vita video: %dx%d RGB565 x%d -> %dx%d, aspect=%s",
             w, h, NUM_TEX, SCREEN_W, SCREEN_H, g_aspect_mode);
    return 1;
}

static void build_lut(const uint8_t *pal)
{
    for (int i = 0; i < 256; ++i) {
        const uint8_t *e = pal + i * 3;
        s_lut[i] = (uint16_t)(((e[0] >> 3) << 11) | ((e[1] >> 2) << 5) | (e[2] >> 3));
    }
}

static void convert(uint16_t *dst, int pitch_px, const uint8_t *src, int w, int h)
{
    int fast = ((((uintptr_t)src) & 3) == 0) && (((uintptr_t)dst) & 3) == 0 &&
               (w % 4) == 0 && (pitch_px % 2) == 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t *s = src + (size_t)y * w;
        uint16_t      *d = dst + (size_t)y * pitch_px;
        if (fast) {
            const uint32_t *s4 = (const uint32_t *)s;
            uint32_t       *d2 = (uint32_t *)d;
            for (int x = 0; x < w / 4; ++x) {
                uint32_t q = s4[x];
                d2[2 * x]     = (uint32_t)s_lut[q & 0xFF] |
                                ((uint32_t)s_lut[(q >> 8) & 0xFF] << 16);
                d2[2 * x + 1] = (uint32_t)s_lut[(q >> 16) & 0xFF] |
                                ((uint32_t)s_lut[q >> 24] << 16);
            }
        } else {
            for (int x = 0; x < w; ++x) d[x] = s_lut[s[x]];
        }
    }
}

void plat_video_present(const uint8_t *shadow, const uint8_t *pal, int w, int h)
{
    if (!s_ren || !s_tex[0] || !shadow || !pal) return;
    if (w != s_fb_w || h != s_fb_h) return;
    uint64_t t0 = perf_now();

    int smooth = GraphicsAlphaFxEnabled() ? 1 : 0;
    int changed_mode = 0;
    if (smooth != s_smooth) {
        if (s_smooth != -1) perf_log_msg("gpu: smoothing %s", smooth ? "on" : "off");
        s_smooth = smooth;
        apply_smooth(smooth);
        changed_mode = 1;
    }

    int pal_changed = !s_have_prev || memcmp(s_prev_pal, pal, sizeof s_prev_pal) != 0;
    if (!pal_changed && !changed_mode && s_have_prev &&
        memcmp(s_prev, shadow, (size_t)w * h) == 0) {
        perf_log_present(0, 0, 0);
        return;
    }
    if (pal_changed) {
        build_lut(pal);
        memcpy(s_prev_pal, pal, sizeof s_prev_pal);
    }
    memcpy(s_prev, shadow, (size_t)w * h);
    s_have_prev = 1;

    int next = (s_cur + 1) % NUM_TEX;
    void *pixels = NULL;
    int   pitch  = 0;
    if (SDL_LockTexture(s_tex[next], NULL, &pixels, &pitch) == 0 && pixels) {
        convert((uint16_t *)pixels, pitch / 2, shadow, w, h);
        SDL_UnlockTexture(s_tex[next]);
        s_cur = next;
    } else {
        static int s_logged;
        if (!s_logged) { LOG_ERROR("vita", "SDL_LockTexture: %s", SDL_GetError()); s_logged = 1; }
    }

    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex[s_cur], NULL, &s_game_rect);
    uint64_t t1 = perf_now();
    SDL_RenderPresent(s_ren);
    uint64_t t2 = perf_now();
    perf_log_present(1, t1 - t0, t2 - t1);

    static int s_note_smooth = -1, s_note_stretch = -1;
    if (s_note_smooth != s_smooth || s_note_stretch != g_stretch_active) {
        s_note_smooth = s_smooth;
        s_note_stretch = g_stretch_active;
        set_state_note();
    }
}

void plat_video_shutdown(void)
{
    for (int i = 0; i < NUM_TEX; ++i)
        if (s_tex[i]) { SDL_DestroyTexture(s_tex[i]); s_tex[i] = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { SDL_DestroyWindow(s_win);   s_win = NULL; }
    free(s_prev);
    s_prev = NULL;
    perf_log_shutdown();
}

/* No windows on the Vita: always full screen. */
void plat_video_toggle_fullscreen(void) {}

void plat_video_message_box(const char *title, const char *body)
{
    perf_log_msg("msgbox: %s: %s", title ? title : "", body ? body : "");
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, body, s_win);
}
