/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/video_sdl.c — video-output HAL, SDL backend.
 *
 * g_aspect_mode: "wide" / "wide-bars" / "4:3" — the frame is always placed
 * by hand (see apply_aspect_mode): 4:3 fitted with bars, or, while playing,
 * the panorama with the sliding inventory panel (src/util/wide_layout.c).
 *
 * g_aspect_mode is defined in src/config.c so it's available for all targets
 * (including PS2 which doesn't link this file). */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/video.h"
#include "sdl_internal.h"
#ifdef __APPLE__
#include "wacki/platform/macos.h"
#endif
#ifdef __ANDROID__
#include "wacki/platform/android_touch.h"
#endif

#include "wacki/api.h"

#include <SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARGB_BYTES_PER_PIXEL      4
#define ARGB_ALPHA_OPAQUE_SHIFTED (0xFFu << 24)
#define ARGB_R_SHIFT              16
#define ARGB_G_SHIFT              8
#define PALETTE_BYTES_PER_ENTRY   3
#define DEFAULT_SCALE_FACTOR      1

static SDL_Window   *s_win = NULL;
static SDL_Renderer *s_ren = NULL;
static SDL_Texture  *s_tex = NULL;
static uint32_t      s_pixels32[640 * 480];
static int           s_fb_w = 0, s_fb_h = 0;

/* ---- pixel-art 2x filter (F6 / pad MINUS·View + R): Scale2x, 2xSaI, xBR
 * (src/util/pixel_filters.c) into a 1280x960 texture. Runs only when the
 * frame or palette changed, split into row bands over worker threads. */
extern int g_pixel_filter;                    /* config.c: wacki.cfg pixel_filter= 0..3 */
static SDL_Texture *s_tex2x = NULL;
static uint32_t    *s_rgb  = NULL;            /* 640x480 0x00RRGGBB        */
static uint32_t    *s_up   = NULL;            /* 1280x960                  */
static uint8_t     *s_last = NULL;            /* last frame + palette      */
static int          s_last_valid = 0;

#define PF_MAX_THREADS 4
typedef struct { int kind, w, h, y0, y1; } PfJob;
static SDL_Thread  *s_pf_thr[PF_MAX_THREADS];
static SDL_sem     *s_pf_go[PF_MAX_THREADS], *s_pf_done;
static PfJob        s_pf_job[PF_MAX_THREADS];
static int          s_pf_nthr = -1;           /* -1: not started          */
static volatile int s_pf_quit = 0;

static int pf_worker(void *arg)
{
    int i = (int)(intptr_t)arg;
    for (;;) {
        SDL_SemWait(s_pf_go[i]);
        if (s_pf_quit) break;
        PfJob *j = &s_pf_job[i];
        PixelUpscale2xRows(j->kind, s_rgb, j->w, j->h, s_up, j->y0, j->y1);
        SDL_SemPost(s_pf_done);
    }
    return 0;
}

static void pf_start_threads(void)
{
    if (s_pf_nthr >= 0) return;
    int n = SDL_GetCPUCount() - 1;
#ifdef WACKI_SWITCH
    n = 2;                                    /* + this thread = 3 cores  */
#endif
    if (n < 0) n = 0;
    if (n > PF_MAX_THREADS) n = PF_MAX_THREADS;
    s_pf_done = n ? SDL_CreateSemaphore(0) : NULL;
    int ok = 0;
    for (int i = 0; i < n && s_pf_done; ++i) {
        s_pf_go[i] = SDL_CreateSemaphore(0);
        s_pf_thr[i] = s_pf_go[i] ? SDL_CreateThread(pf_worker, "pixfilter", (void *)(intptr_t)i) : NULL;
        if (!s_pf_thr[i]) break;
        ++ok;
    }
    s_pf_nthr = ok;
    LOG_INFO("video", "pixel filter: %d worker thread(s)", ok);
}

static void pf_stop_threads(void)
{
    if (s_pf_nthr <= 0) { s_pf_nthr = -1; return; }
    s_pf_quit = 1;
    for (int i = 0; i < s_pf_nthr; ++i) { SDL_SemPost(s_pf_go[i]); SDL_WaitThread(s_pf_thr[i], NULL); SDL_DestroySemaphore(s_pf_go[i]); }
    SDL_DestroySemaphore(s_pf_done);
    s_pf_nthr = -1;
    s_pf_quit = 0;
}

static void pf_run(int kind, int w, int h)
{
    PixelUpscale2xPrepare(kind, s_rgb, w, h);
    int bands = s_pf_nthr + 1;
    int step = (h + bands - 1) / bands;
    for (int i = 0; i < s_pf_nthr; ++i) {
        s_pf_job[i] = (PfJob){ kind, w, h, (i + 1) * step, (i + 2) * step };
        SDL_SemPost(s_pf_go[i]);
    }
    PixelUpscale2xRows(kind, s_rgb, w, h, s_up, 0, step);      /* own band */
    for (int i = 0; i < s_pf_nthr; ++i) SDL_SemWait(s_pf_done);
}

/* Filtered texture for this frame, or NULL (filter off / no memory). */
static SDL_Texture *pf_texture(const uint8_t *shadow, const uint8_t *pal, int w, int h)
{
    int kind = g_pixel_filter;
    if (kind <= 0 || w > 640 || h > 480) return NULL;
    size_t n = (size_t)w * h;
    if (!s_rgb)  s_rgb  = (uint32_t *)malloc(640 * 480 * 4);
    if (!s_up)   s_up   = (uint32_t *)malloc(1280 * 960 * 4);
    if (!s_last) s_last = (uint8_t *)malloc(640 * 480 + 768 + 8);
    if (!s_rgb || !s_up || !s_last) return NULL;
    if (!s_tex2x) {
        s_tex2x = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB888,
                                    SDL_TEXTUREACCESS_STREAMING, 1280, 960);
        if (!s_tex2x) return NULL;
        SDL_ScaleMode m = SDL_ScaleModeNearest;
        if (s_tex) SDL_GetTextureScaleMode(s_tex, &m);
        SDL_SetTextureScaleMode(s_tex2x, m);
        s_last_valid = 0;
    }
    pf_start_threads();
    uint8_t *lp = s_last;
    if (s_last_valid && lp[0] == (uint8_t)kind && lp[1] == (uint8_t)(w >> 4) &&
        memcmp(lp + 8, shadow, n) == 0 && memcmp(lp + 8 + n, pal, 768) == 0)
        return s_tex2x;                                  /* unchanged */
    lp[0] = (uint8_t)kind; lp[1] = (uint8_t)(w >> 4);
    memcpy(lp + 8, shadow, n);
    memcpy(lp + 8 + n, pal, 768);
    s_last_valid = 1;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *e = pal + shadow[i] * 3;
        s_rgb[i] = ((uint32_t)e[0] << 16) | ((uint32_t)e[1] << 8) | e[2];
    }
    pf_run(kind, w, h);
    SDL_UpdateTexture(s_tex2x, NULL, s_up, w * 2 * 4);
    return s_tex2x;
}

void plat_video_cycle_pixel_filter(void)
{
    extern void ConfigSave(void);
    static const char *const names[4] = {
        "Filtr pikseli: wy\xC5\x82\xC4\x85""czony", "Filtr pikseli: Scale2x (ostry)",
        "Filtr pikseli: 2xSaI (g\xC5\x82""adki)", "Filtr pikseli: xBR (kraw\xC4\x99""dzie)" };
    g_pixel_filter = (g_pixel_filter + 1) % 4;
    s_last_valid = 0;
    SubtitlesHint(names[g_pixel_filter], 2000);
    LOG_INFO("video", "pixel_filter=%d", g_pixel_filter);
    ConfigSave();
}

/* Defined in src/config.c — available for all targets. */
extern char g_aspect_mode[16];

/* Picture layout. The renderer never uses SDL_RenderSetLogicalSize (except
 * on Android, whose overlay lays itself out): every present computes the
 * destination rect itself, and platform_video_window_to_game() maps the
 * mouse back with the same numbers.
 *   4:3        — the frame fitted with bars (old "4:3");
 *   wide/bars  — while playing, the panorama with the sliding inventory
 *                panel (src/util/wide_layout.c); films/menus stay 4:3. */
static void apply_aspect_mode(int w, int h)
{
    (void)w; (void)h;
#ifndef __ANDROID__
    if (s_ren) SDL_RenderSetLogicalSize(s_ren, 0, 0);
#endif
}

static void output_size(int *ow, int *oh)
{
    *ow = s_fb_w; *oh = s_fb_h;
    if (s_ren && SDL_GetRendererOutputSize(s_ren, ow, oh) == 0 && *ow > 0 && *oh > 0)
        return;
    if (s_win) SDL_GetWindowSize(s_win, ow, oh);
}

static void fit_43(int ow, int oh, SDL_Rect *d)
{
    int fw = s_fb_w > 0 ? s_fb_w : 640, fh = s_fb_h > 0 ? s_fb_h : 480;
    if ((long)ow * fh <= (long)oh * fw) { d->w = ow; d->h = (int)((long)ow * fh / fw); }
    else                                { d->h = oh; d->w = (int)((long)oh * fw / fh); }
    d->x = (ow - d->w) / 2;
    d->y = (oh - d->h) / 2;
}

void platform_video_get_present_state(int *stretch_active,
                                      int *win_w, int *win_h,
                                      int *fb_w,  int *fb_h)
{
    *stretch_active = WideLayoutActive();
    *fb_w = s_fb_w; *fb_h = s_fb_h;
    if (s_win) SDL_GetWindowSize(s_win, win_w, win_h);
    else { *win_w = s_fb_w; *win_h = s_fb_h; }
}

/* Window (mouse) coordinates → game 640×480 coordinates. */
void platform_video_window_to_game(int wx, int wy, int *gx, int *gy)
{
    int ow, oh, ww = 0, wh = 0;
    output_size(&ow, &oh);
    if (s_win) SDL_GetWindowSize(s_win, &ww, &wh);
    float sx = (ww > 0) ? (float)wx * ow / ww : (float)wx;
    float sy = (wh > 0) ? (float)wy * oh / wh : (float)wy;
    if (WideLayoutScreenToGame(ow, oh, sx, sy, gx, gy)) return;
    SDL_Rect d; fit_43(ow, oh, &d);
    int x = d.w > 0 ? (int)((sx - d.x) * s_fb_w / d.w) : wx;
    int y = d.h > 0 ? (int)((sy - d.y) * s_fb_h / d.h) : wy;
    if (x < 0) x = 0;
    if (x >= s_fb_w) x = s_fb_w - 1;
    if (y < 0) y = 0;
    if (y >= s_fb_h) y = s_fb_h - 1;
    *gx = x; *gy = y;
}

int platform_video_game_to_window(int gx, int gy, int *wx, int *wy)
{
    int ow, oh, ww = 0, wh = 0;
    float sx, sy;
    output_size(&ow, &oh);
    if (!WideLayoutGameToScreen(ow, oh, gx, gy, &sx, &sy)) return 0;
    if (s_win) SDL_GetWindowSize(s_win, &ww, &wh);
    *wx = (ww > 0 && ow > 0) ? (int)(sx * ww / ow) : (int)sx;
    *wy = (wh > 0 && oh > 0) ? (int)(sy * wh / oh) : (int)sy;
    return 1;
}

void platform_video_toggle_aspect_mode(void)
{
    WideAspectCycle();              /* wide -> wide-bars -> 4:3, saved */
}

unsigned plat_video_sdl_init_flags(void)
{
    return SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO;
}

int plat_video_init(int w, int h, const char *title)
{
    plat_apply_video_prefs();

    s_fb_w = w; s_fb_h = h;

    int sf    = g_scale_factor > 0 ? g_scale_factor : DEFAULT_SCALE_FACTOR;
    int win_w = w * sf, win_h = h * sf;
    if (g_scale_mode && *g_scale_mode)
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, g_scale_mode);

    Uint32 win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (g_fullscreen) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    s_win = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, win_flags);
    if (!s_win) { LOG_INFO("log", "SDL_CreateWindow: %s", SDL_GetError()); return 0; }

    s_ren = SDL_CreateRenderer(s_win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_ren)
        s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_SOFTWARE);
    if (!s_ren) { LOG_INFO("log", "SDL_CreateRenderer: %s", SDL_GetError()); return 0; }

#ifndef __ANDROID__
    apply_aspect_mode(w, h);
#endif

    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!s_tex) { LOG_INFO("log", "SDL_CreateTexture: %s", SDL_GetError()); return 0; }
    {
        extern int g_smoothing;
        if (g_smoothing >= 0)              /* wacki.cfg smoothing= (F3) wins */
            SDL_SetTextureScaleMode(s_tex, g_smoothing ? SDL_ScaleModeLinear
                                                       : SDL_ScaleModeNearest);
    }

    extern unsigned char wacki_icon_bmp[];
    extern unsigned int  wacki_icon_bmp_len;
    SDL_RWops *rw = SDL_RWFromConstMem(wacki_icon_bmp, (int)wacki_icon_bmp_len);
    if (rw) {
        SDL_Surface *icon = SDL_LoadBMP_RW(rw, 1);
        if (icon) { SDL_SetWindowIcon(s_win, icon); SDL_FreeSurface(icon); }
    }

#ifdef __APPLE__
    PlatformSetupMacMenu();
#endif
    SDL_ShowCursor(SDL_DISABLE);

    LOG_INFO("platform", "SDL ready: %dx%d (%dx scale, fullscreen=%d, aspect=%s)",
             win_w, win_h, sf, g_fullscreen, g_aspect_mode);

    memset(s_pixels32, 0, (size_t)w * h * ARGB_BYTES_PER_PIXEL);
    SDL_UpdateTexture(s_tex, NULL, s_pixels32, w * ARGB_BYTES_PER_PIXEL);
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
    SDL_PumpEvents();
    return 1;
}

#ifdef WACKI_SWITCH
/* Switch: smoothing (linear filtering of the 640×480 frame as the GPU
 * scales it up to 720p/1080p) follows the FIRST button of the Grafika
 * menu (s_opt_gfx1 / video_mode, "better graphics") — on by default,
 * changed the moment the button is clicked, saved with the options. The
 * same button does this on the 3DS (video_3ds_c3d.c) and the Vita
 * (video_vita.c). Off = sharp pixels (nearest), as before. */
static int s_switch_smooth = -1;

static void switch_apply_smoothing(void)
{
    int on = GraphicsAlphaFxEnabled() ? 1 : 0;
    if (on == s_switch_smooth || !s_tex) return;
    if (SDL_SetTextureScaleMode(s_tex, on ? SDL_ScaleModeLinear
                                          : SDL_ScaleModeNearest) == 0) {
        if (s_tex2x) SDL_SetTextureScaleMode(s_tex2x, on ? SDL_ScaleModeLinear
                                                         : SDL_ScaleModeNearest);
        s_switch_smooth = on;
        LOG_INFO("platform", "smoothing %s", on ? "on" : "off");
    } else {
        static int s_logged;
        if (!s_logged) {
            LOG_WARN("platform", "SDL_SetTextureScaleMode: %s", SDL_GetError());
            s_logged = 1;
        }
        s_switch_smooth = on;   /* don't retry every frame */
    }
}
#endif

/* "Sterowanie współczesne": the panel (frame rows 400..479) in a frame in
 * the middle of the screen, the scene dimmed behind it. */
static void draw_popup(const WideRect *r, SDL_Texture *tex, int tk, int w, int h)
{
    int b = (int)(r->p_border + 0.5f);
    SDL_Rect pr = { (int)(r->p_x + 0.5f), (int)(r->p_y + 0.5f),
                    (int)(r->p_w + 0.5f), (int)(r->p_h + 0.5f) };
    SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 90);
    SDL_RenderFillRect(s_ren, NULL);                          /* dim */
    SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_NONE);
    SDL_Rect outer = { pr.x - 2 * b, pr.y - 2 * b, pr.w + 4 * b, pr.h + 4 * b };
    SDL_Rect inner = { pr.x - b, pr.y - b, pr.w + 2 * b, pr.h + 2 * b };
    SDL_SetRenderDrawColor(s_ren, 10, 10, 14, 255);
    SDL_RenderFillRect(s_ren, &outer);
    SDL_SetRenderDrawColor(s_ren, 222, 200, 120, 255);       /* gold rim */
    SDL_RenderFillRect(s_ren, &inner);
    SDL_Rect src = { 0, 400 * tk, w * tk, (h - 400) * tk };
    SDL_RenderCopy(s_ren, tex, &src, &pr);
}

void plat_video_present(const uint8_t *shadow, const uint8_t *pal, int w, int h)
{
    if (!s_tex || !shadow || !pal) return;
#ifdef WACKI_SWITCH
    switch_apply_smoothing();
#endif

    void *pixels = NULL; int pitch = 0;
    if (SDL_LockTexture(s_tex, NULL, &pixels, &pitch) == 0 && pixels) {
        uint32_t *out = (uint32_t *)pixels;
        int stride = pitch / ARGB_BYTES_PER_PIXEL;
        for (int y = 0; y < h; ++y) {
            uint32_t      *row = out + (size_t)y * stride;
            const uint8_t *src = shadow + (size_t)y * w;
            for (int x = 0; x < w; ++x) {
                const uint8_t *e = pal + src[x] * PALETTE_BYTES_PER_ENTRY;
                row[x] = ARGB_ALPHA_OPAQUE_SHIFTED
                       | ((uint32_t)e[0] << ARGB_R_SHIFT)
                       | ((uint32_t)e[1] << ARGB_G_SHIFT)
                       |  (uint32_t)e[2];
            }
        }
        SDL_UnlockTexture(s_tex);
    } else {
        int n = w * h < (int)(sizeof s_pixels32 / 4) ? w * h : (int)(sizeof s_pixels32 / 4);
        for (int i = 0; i < n; ++i) {
            const uint8_t *e = pal + shadow[i] * PALETTE_BYTES_PER_ENTRY;
            s_pixels32[i] = ARGB_ALPHA_OPAQUE_SHIFTED
                          | ((uint32_t)e[0] << ARGB_R_SHIFT)
                          | ((uint32_t)e[1] << ARGB_G_SHIFT)
                          |  (uint32_t)e[2];
        }
        SDL_UpdateTexture(s_tex, NULL, s_pixels32, w * ARGB_BYTES_PER_PIXEL);
    }

    SDL_Texture *tex = pf_texture(shadow, pal, w, h);   /* F6 filter */
    int tk = tex ? 2 : 1;
    if (!tex) tex = s_tex;

    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderClear(s_ren);

#ifdef __ANDROID__
    wacki_overlay_compute_layout(s_ren);
    SDL_Rect gr = wacki_overlay_game_rect();
    SDL_RenderCopy(s_ren, tex, NULL, gr.w > 0 ? &gr : NULL);
    wacki_overlay_draw(s_ren);
#else
    {
        int ow, oh;
        WideRect r;
        output_size(&ow, &oh);
        if (WideLayoutGet(ow, oh, &r)) {
            /* whole source rows, the fraction moved into the dest rect and
             * clipped — the pan stays smooth at any scale */
            int   sy0  = (int)r.src_y;
            int   sy1  = (int)(r.src_y + r.src_h + 0.999f);
            if (sy1 > h) sy1 = h;
            float frac = r.src_y - (float)sy0;
            SDL_Rect src  = { 0, sy0 * tk, w * tk, (sy1 - sy0) * tk };
            SDL_Rect clip = { (int)(r.dst_x + 0.5f), (int)(r.dst_y + 0.5f),
                              (int)(r.dst_w + 0.5f), (int)(r.dst_h + 0.5f) };
            SDL_Rect dst  = { clip.x, (int)(r.dst_y - frac * r.scale + 0.5f),
                              clip.w, (int)((sy1 - sy0) * r.scale + 0.5f) };
            SDL_RenderSetClipRect(s_ren, &clip);
            SDL_RenderCopy(s_ren, tex, &src, &dst);
            SDL_RenderSetClipRect(s_ren, NULL);
            if (r.popup) draw_popup(&r, tex, tk, w, h);
        } else {
            SDL_Rect d;
            fit_43(ow, oh, &d);
            SDL_RenderCopy(s_ren, tex, NULL, &d);
        }
    }
#endif
    SDL_RenderPresent(s_ren);
}

void plat_video_shutdown(void)
{
    pf_stop_threads();
    if (s_tex2x) { SDL_DestroyTexture(s_tex2x); s_tex2x = NULL; }
    free(s_rgb); free(s_up); free(s_last); s_rgb = s_up = NULL; s_last = NULL;
    if (s_tex) { SDL_DestroyTexture(s_tex);  s_tex = NULL; }
    if (s_ren) { SDL_DestroyRenderer(s_ren); s_ren = NULL; }
    if (s_win) { SDL_DestroyWindow(s_win);   s_win = NULL; }
}

extern int g_smoothing;                   /* config.c (wacki.cfg smoothing=) */

static void apply_smoothing_setting(void)
{
#ifndef WACKI_SWITCH
    if (!s_tex || g_smoothing < 0) return;   /* -1: as --scale-mode set it */
    SDL_SetTextureScaleMode(s_tex, g_smoothing ? SDL_ScaleModeLinear
                                               : SDL_ScaleModeNearest);
    if (s_tex2x) SDL_SetTextureScaleMode(s_tex2x, g_smoothing ? SDL_ScaleModeLinear
                                                              : SDL_ScaleModeNearest);
#endif
}

void plat_video_toggle_smoothing(void)
{
#ifdef WACKI_SWITCH
    SubtitlesHint("Wyg\xC5\x82""adzanie: pierwszy przycisk w menu Grafika", 2500);
#else
    if (g_smoothing < 0) {   /* first press: start from what is in effect */
        SDL_ScaleMode m = SDL_ScaleModeNearest;
        if (s_tex) SDL_GetTextureScaleMode(s_tex, &m);
        g_smoothing = (m != SDL_ScaleModeNearest);
    }
    g_smoothing = !g_smoothing;
    apply_smoothing_setting();
    extern void ConfigSave(void);
    ConfigSave();
    SubtitlesHint(g_smoothing ? "Wyg\xC5\x82""adzanie: w\xC5\x82\xC4\x85""czone"
                              : "Wyg\xC5\x82""adzanie: wy\xC5\x82\xC4\x85""czone", 1500);
#endif
}

void plat_video_toggle_fullscreen(void)
{
    if (!s_win) return;
    g_fullscreen = !g_fullscreen;
    SDL_SetWindowFullscreen(s_win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    LOG_INFO("platform", "fullscreen=%d", g_fullscreen);
    extern void ConfigSave(void);
    ConfigSave();
}

void plat_video_message_box(const char *title, const char *body)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, body, s_win);
}
