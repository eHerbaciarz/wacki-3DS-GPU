/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 * GPU (citro3d) part based on ReClaw's n3ds_gpu.cpp — Copyright (C) 2026
 * Herbaciarz, GPL-3.0 (ReClaw: Captain Claw port, 3DS GPU layer).
 *
 * src/platform/3ds/video_3ds_c3d.c — dual-screen video HAL drawn by the
 * 3DS GPU (citro3d). Replaces video_3ds_gl.c (CPU nearest-neighbour
 * writes straight into the framebuffers), which stays in the tree and can
 * still be built with `make TARGET=3ds VIDEO_3DS=fb`.
 *
 * PER FRAME
 *   1. The CPU turns the engine's 640×480 8-bit frame into RGB565 with a
 *      256-entry palette table (sequential reads and writes, 4 pixels at
 *      a time) into a linear "staging" buffer with a 1024-pixel stride.
 *   2. GX_DisplayTransfer (C3D_SyncDisplayTransfer, queued inside the
 *      frame) tiles it into a 1024×512 RGB565 texture — the GPU's DMA
 *      does the Morton tiling that made the old picaGL version slow
 *      (see video_3ds_gl.c's top comment), the CPU never touches it.
 *   3. One textured quad per screen:
 *        top    — 4:3: the frame in a centred 320×240 window with black
 *                 bars (exact 2:1, so smoothing = a clean 2×2 average);
 *                 "wide"/"wide-bars" while playing: the panorama with
 *                 the sliding inventory panel (src/util/wide_layout.c);
 *        bottom — the zoom view: a part of the SAME texture (1×/2×/4×
 *                 around the cursor, or the whole frame), no second copy.
 *      Stereo 3D (slider open): a second texture with the right eye's
 *      picture (foreground shifted, as before) goes to the right eye.
 *
 * SMOOTHING: linear filtering when the picture is made smaller (the top
 * screen, the bottom's whole-frame view); the bottom's 2×/4× zoom stays
 * sharp (nearest) so the magnified pixel art is not blurred. On by
 * default; it follows the FIRST button of the Grafika menu (s_opt_gfx1 /
 * video_mode, "better graphics"), read every frame through
 * GraphicsAlphaFxEnabled(), so it changes the moment the button is
 * clicked and is saved with the other options.
 *
 * TIMING: C3D_FrameBegin(0) — it waits only until the GPU has finished
 * the previous frame (needed: that frame may still read the texture we
 * are about to overwrite), NOT for the screen's refresh: the engine
 * already holds itself at 30 fps (EnginePaceFrame, 33 ms), and a vsync
 * wait on top of that would make frames uneven. citro3d's double-
 * buffered framebuffers are swapped at the next vblank, so no tearing.
 * Every number goes to sdmc:/3ds/wacki/wacki_perf.log (perf_log_3ds.c).
 *
 * TEXTURE ROW ORDER: rows are copied WITHOUT GX_TRANSFER_FLIP_VERT
 * (picture row y → texture memory row y, the layout ReClaw writes by hand)
 * and sampled with v = 1 - y/512 (ReClaw, checked on a New 3DS: the
 * texture's first memory row is the picture's top). Checked on a New 3DS
 * here too: FLIP_VERT(1) — as in libctru's texture examples — showed the
 * whole picture upside down (it mirrors the whole 512-row block, so row y
 * landed in row 511-y). -DWACKI_C3D_TEX_FLIP=1 brings that back. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/api.h"
#include "wacki/platform/video.h"
#include "perf_log_3ds.h"
#include "gpu/wacki2d_shbin.h"
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOP_WIDTH  400
#define TOP_HEIGHT 240
#define BOT_WIDTH  320
#define BOT_HEIGHT 240

#define TOP_43_W 320                        /* 240 * 4/3, exactly */
#define TOP_43_X ((TOP_WIDTH - TOP_43_W)/2) /* 40 px bars          */

/* The texture (and the staging buffer's layout): power-of-two, ≥ 640×480.
 * 1024 is the GPU's largest texture side. */
#define TEX_W 1024
#define TEX_H 512

#ifndef WACKI_C3D_TEX_FLIP
#define WACKI_C3D_TEX_FLIP 0
#endif

/* Bottom-screen zoom filter (wacki.cfg zoom_filter, SELECT+X cycles):
 *   0 ostry    — nearest: big square pixels
 *   1 gładki   — linear: soft
 *   2 Scale2x  — sharp, straight/45° edges smoothed (default; cheap)
 *   3 2xSaI    — smooth edges
 *   4 xBR      — edges of any slope (most CPU: watch wacki_perf.log)
 * 2–4 run on the CPU over the visible part only (160×120 at 2×, 80×60
 * twice at 4×) into their own 512×256 texture (util/pixel_filters.c). */
#define ZOOM_FILTER_COUNT   5
#define ZOOM_FILTER_DEFAULT 2
#define ZTEX_W 512
#define ZTEX_H 256
extern int g_zoom_filter;                 /* config.c; -1 = default */
static const char *const k_zoom_filter_names[ZOOM_FILTER_COUNT] = {
    "ostry", "g\xC5\x82""adki", "Scale2x", "2xSaI", "xBR",
};
static int zoom_filter_mode(void)
{
    int m = g_zoom_filter < 0 ? ZOOM_FILTER_DEFAULT : g_zoom_filter;
    return (m >= 0 && m < ZOOM_FILTER_COUNT) ? m : ZOOM_FILTER_DEFAULT;
}

#define DISPLAY_TRANSFER_FLAGS                                              \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |                  \
     GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                         \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define TEXTURE_TRANSFER_FLAGS                                              \
    (GX_TRANSFER_FLIP_VERT(WACKI_C3D_TEX_FLIP) | GX_TRANSFER_OUT_TILED(1) | \
     GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |                       \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/* ---- zoom / touch state (same behaviour as video_3ds_gl.c) ----------- *
 * See video_3ds_gl.c for the full story of each of these; the GPU
 * backend keeps the exact same cycle (1× → 2× → 4× → whole frame), the
 * same frozen crop while a touch is held, and the same inverse mapping
 * for taps. */
static int g_zoom_level = 0;
static const int ZOOM_LEVELS[] = {1, 2, 4, 0};
#define NUM_ZOOM_LEVELS 4

static int s_zoom_src_x = 0, s_zoom_src_y = 0;
static int s_zoom_region_w = BOT_WIDTH, s_zoom_region_h = BOT_HEIGHT;
static int s_touch_active = 0;

/* Previous frame (for the skip of identical frames). */
static uint8_t *s_shadow  = NULL;
static uint8_t  s_palette[256 * 3];
static int s_fb_w = 0, s_fb_h = 0;
static int s_have_prev_frame = 0;
static int s_prev_mouse_x = -1, s_prev_mouse_y = -1;
static int s_prev_zoom_level = -1;
static int s_prev_smooth = -1;

/* Stereo depth at full slider, in frame (640-wide) pixels: 18 → ~9 px on
 * the top screen in 4:3, ~11 in stretch. (Was 6 → 3 px: barely visible.) */
#ifndef STEREO_MAX_DISPARITY_SRC_PX
#define STEREO_MAX_DISPARITY_SRC_PX  18
#endif

/* Sprite pixels that happen to have the background's colour at that spot
 * would count as background; gaps up to this wide between foreground
 * pixels on a row are filled so sprites don't get speckled holes. */
#define STEREO_FG_GAP_FILL  3
/* Aspect modes on the top screen (5:3): "wide" / "wide-bars" = the
 * panorama with the sliding inventory panel while playing — shared with
 * PC/Switch/Vita, see src/util/wide_layout.c (crop: 384 of 400 scene rows
 * at scale 0.625, the window following the cursor; bars: the whole scene
 * at 0.6 with 8-px side bars). Films and menus stay 4:3. Only the texture
 * coordinates move: no frame conversion for a pan or the slide. */
static WideRect s_prev_wr;

static int s_prev_stereo_enabled = -1;
static int s_have_right = 0;   /* right-eye texture built this frame */
static int s_prev_disparity_px   = -1;

/* ---- GPU state ------------------------------------------------------- */

typedef struct {
    float   x, y, u, v;
    uint8_t r, g, b, a;
} Vtx;

enum { QUAD_TOP_L = 0, QUAD_TOP_R = 1, QUAD_BOT = 2, QUAD_COUNT = 3 };

static C3D_RenderTarget *s_rt_top[2]; /* left, right eye */
static C3D_RenderTarget *s_rt_bot;

/* WIDE TOP SCREEN (800 px): the 3DS top screen can show 800 columns
 * instead of 400 when stereo 3D is off (gfxSetWide). The 4:3 picture is
 * then exactly 640 of those columns — the frame 1:1 across, only halved
 * in height — so text and outlines are much sharper, for no GPU cost.
 * On by default (wacki.cfg wide_top=, SELECT+START toggles); off by
 * itself while the 3D slider is up, and never on an original 2DS (no
 * wide mode there). Its own 240×800 render target. */
#define WIDE_WIDTH 800
extern int g_wide_top;                    /* config.c; -1 = default (on) */
static C3D_RenderTarget *s_rt_wide;
static int s_wide_supported = 1;
static int s_wide_active    = 0;
static DVLB_s          *s_dvlb;
static shaderProgram_s  s_prog;
static int              s_u_proj = -1;
static C3D_Tex          s_tex[2];     /* left eye (and bottom), right eye */
static int              s_tex_ok[2];
static uint16_t        *s_stage[2];   /* linear, TEX_W × TEX_H RGB565 */
static Vtx             *s_vbuf;       /* QUAD_COUNT × 4, linear */
static int              s_c3d_up;
static uint16_t         s_palette_lut[256];

static inline uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void build_palette_lut(const uint8_t *pal)
{
    for (int i = 0; i < 256; ++i) {
        const uint8_t *e = pal + i * 3;
        s_palette_lut[i] = pack_rgb565(e[0], e[1], e[2]);
    }
}

/* Repeat the last column and row once past the picture, so smooth
 * sampling right at the picture's edge mixes in the edge itself and not
 * whatever is next to it in the texture. */
static void pad_edges(uint16_t *dst, int w, int h)
{
    for (int y = 0; y < h; ++y)
        dst[(size_t)y * TEX_W + w] = dst[(size_t)y * TEX_W + w - 1];
    memcpy(dst + (size_t)h * TEX_W, dst + (size_t)(h - 1) * TEX_W,
           (size_t)(w + 1) * sizeof(uint16_t));
}

/* 8-bit frame → RGB565 staging rows, 4 pixels per step (one 32-bit read,
 * two 32-bit writes) when the source allows it. */
static void convert_frame(uint16_t *dst, const uint8_t *src, int w, int h)
{
    const uint16_t *lut = s_palette_lut;
    int fast = ((((uintptr_t)src) & 3) == 0) && (w % 4) == 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t *s = src + (size_t)y * w;
        uint16_t      *d = dst + (size_t)y * TEX_W;
        if (fast) {
            const uint32_t *s4 = (const uint32_t *)s;
            uint32_t       *d2 = (uint32_t *)d;
            for (int x = 0; x < w / 4; ++x) {
                uint32_t q = s4[x];
                d2[2 * x]     = (uint32_t)lut[q & 0xFF] |
                                ((uint32_t)lut[(q >> 8) & 0xFF] << 16);
                d2[2 * x + 1] = (uint32_t)lut[(q >> 16) & 0xFF] |
                                ((uint32_t)lut[q >> 24] << 16);
            }
        } else {
            for (int x = 0; x < w; ++x) d[x] = lut[s[x]];
        }
    }
    pad_edges(dst, w, h);
}

/* Right eye (stereo 3D). The scene's BACKGROUND goes behind the screen,
 * everything drawn over it (characters, objects, the HUD panel, cursor,
 * subtitles) stays on the screen's surface:
 *   foreground pixel (frame differs from the clean background snapshot)
 *     → shown unshifted, as in the left eye;
 *   background pixel → the background layer shifted right by disparity_px
 *     (uncrossed disparity = farther away).
 * The background snapshot is a complete layer, so shifting it never shows
 * holes behind the characters. (The old version shifted the foreground
 * the other way, which pushed the characters BEHIND the background —
 * depth that fights the picture's own overlap, so it looked unclear.) */
static void convert_frame_stereo_right(uint16_t *dst, const uint8_t *src,
                                       const uint8_t *bg, int w, int h,
                                       int disparity_px)
{
    const uint16_t *lut = s_palette_lut;
    static uint8_t  fg[TEX_W];
    for (int y = 0; y < h; ++y) {
        const uint8_t *row    = src + (size_t)y * w;
        const uint8_t *bg_row = bg  + (size_t)y * w;
        uint16_t      *d      = dst + (size_t)y * TEX_W;

        int last = -1000;
        for (int x = 0; x < w; ++x) {
            fg[x] = row[x] != bg_row[x];
            if (fg[x]) {
                if (x - last > 1 && x - last - 1 <= STEREO_FG_GAP_FILL)
                    memset(fg + last + 1, 1, (size_t)(x - last - 1));
                last = x;
            }
        }
        for (int x = 0; x < w; ++x) {
            if (fg[x]) {
                d[x] = lut[row[x]];
            } else {
                int sx = x - disparity_px;
                d[x] = lut[bg_row[sx < 0 ? 0 : sx]];
            }
        }
    }
    pad_edges(dst, w, h);
}

/* Staging buffer → tiled texture, queued in the current frame (runs on
 * the GPU before this frame's drawing, after the previous frame). */
static void upload_texture(int eye, int h)
{
    GSPGPU_FlushDataCache(s_stage[eye], (u32)(h + 1) * TEX_W * sizeof(uint16_t));
    C3D_SyncDisplayTransfer((u32 *)s_stage[eye], GX_BUFFER_DIM(TEX_W, TEX_H),
                            (u32 *)s_tex[eye].data, GX_BUFFER_DIM(TEX_W, TEX_H),
                            TEXTURE_TRANSFER_FLAGS);
}

static inline float tex_v(float y)
{
    return 1.0f - y / (float)TEX_H;
}

/* One quad (triangle strip) in target pixels → source rectangle. */
static void set_quad(int q, float x0, float y0, float x1, float y1,
                     float sx0, float sy0, float sx1, float sy1)
{
    Vtx  *v  = s_vbuf + q * 4;
    float u0 = sx0 / (float)TEX_W, u1 = sx1 / (float)TEX_W;
    float v0 = tex_v(sy0),         v1 = tex_v(sy1);
    Vtx a = { x0, y0, u0, v0, 255, 255, 255, 255 };
    Vtx b = { x0, y1, u0, v1, 255, 255, 255, 255 };
    Vtx c = { x1, y0, u1, v0, 255, 255, 255, 255 };
    Vtx d = { x1, y1, u1, v1, 255, 255, 255, 255 };
    v[0] = a; v[1] = b; v[2] = c; v[3] = d;
}

static void draw_on(C3D_RenderTarget *rt, int screen_w, int screen_h,
                    C3D_Tex *tex, int quad)
{
    /* Colour only: these targets have no depth buffer, and CLEAR_ALL
     * would also memory-fill the (NULL) depth address. */
    C3D_RenderTargetClear(rt, C3D_CLEAR_COLOR, 0x000000FF, 0);
    C3D_FrameDrawOn(rt);
    C3D_Mtx proj;
    Mtx_OrthoTilt(&proj, 0.0f, (float)screen_w, (float)screen_h, 0.0f,
                  1.0f, -1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_u_proj, &proj);
    C3D_TexBind(0, tex);
    C3D_DrawArrays(GPU_TRIANGLE_STRIP, quad * 4, 4);
}

static void apply_filter(int smooth)
{
    GPU_TEXTURE_FILTER_PARAM min = smooth ? GPU_LINEAR : GPU_NEAREST;
    /* magnifying = the bottom zoom: linear only for the "gładki" filter */
    GPU_TEXTURE_FILTER_PARAM mag = zoom_filter_mode() == 1 ? GPU_LINEAR : GPU_NEAREST;
    for (int i = 0; i < 2; ++i)
        if (s_tex_ok[i]) C3D_TexSetFilter(&s_tex[i], mag, min);
}

static int alloc_eye(int eye)
{
    if (s_tex_ok[eye]) return 1;
    int vram = 1;
    if (!C3D_TexInitVRAM(&s_tex[eye], TEX_W, TEX_H, GPU_RGB565)) {
        vram = 0;
        if (!C3D_TexInit(&s_tex[eye], TEX_W, TEX_H, GPU_RGB565)) {
            LOG_ERROR("3ds-gpu", "no memory for the %s texture (%dx%d)",
                      eye ? "right-eye" : "frame", TEX_W, TEX_H);
            return 0;
        }
    }
    C3D_TexSetWrap(&s_tex[eye], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    s_stage[eye] = (uint16_t *)linearAlloc(TEX_W * TEX_H * sizeof(uint16_t));
    if (!s_stage[eye]) {
        C3D_TexDelete(&s_tex[eye]);
        LOG_ERROR("3ds-gpu", "no linear memory for the %s staging buffer",
                  eye ? "right-eye" : "frame");
        return 0;
    }
    memset(s_stage[eye], 0, TEX_W * TEX_H * sizeof(uint16_t));
    s_tex_ok[eye] = 1;
    s_prev_smooth = -1;   /* filter set (again) by the present */
    perf_log_msg("gpu: %s texture %dx%d RGB565 in %s", eye ? "right-eye" : "frame",
                 TEX_W, TEX_H, vram ? "VRAM" : "linear memory");
    return 1;
}

/* No depth buffer (C3D_DEPTHTYPE(-1), the spelling ReClaw uses, is C++
 * only; in C the type is a union with an int member). */
#define NO_DEPTH ((C3D_DEPTHTYPE){ .__i = -1 })

static int gpu_init(void)
{
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        LOG_ERROR("3ds-gpu", "C3D_Init failed");
        return 0;
    }
    s_c3d_up = 1;

    /* (width, height) swapped: the 3DS screens are rotated panels. */
    s_rt_top[0] = C3D_RenderTargetCreate(TOP_HEIGHT, TOP_WIDTH, GPU_RB_RGBA8, NO_DEPTH);
    s_rt_top[1] = C3D_RenderTargetCreate(TOP_HEIGHT, TOP_WIDTH, GPU_RB_RGBA8, NO_DEPTH);
    s_rt_bot    = C3D_RenderTargetCreate(BOT_HEIGHT, BOT_WIDTH, GPU_RB_RGBA8, NO_DEPTH);
    s_rt_wide   = C3D_RenderTargetCreate(TOP_HEIGHT, WIDE_WIDTH, GPU_RB_RGBA8, NO_DEPTH);
    if (!s_rt_wide) perf_log_msg("gpu: no memory for the 800-px top target — wide mode off");
    {   /* the original 2DS has no wide mode */
        u8 model = 0;
        if (R_SUCCEEDED(cfguInit())) {
            if (R_SUCCEEDED(CFGU_GetSystemModel(&model)) && model == CFG_MODEL_2DS)
                s_wide_supported = 0;
            cfguExit();
        }
    }
    if (!s_rt_top[0] || !s_rt_top[1] || !s_rt_bot) {
        LOG_ERROR("3ds-gpu", "render targets not created");
        return 0;
    }
    C3D_RenderTargetSetOutput(s_rt_top[0], GFX_TOP,    GFX_LEFT,  DISPLAY_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(s_rt_top[1], GFX_TOP,    GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(s_rt_bot,    GFX_BOTTOM, GFX_LEFT,  DISPLAY_TRANSFER_FLAGS);

    s_dvlb = DVLB_ParseFile((u32 *)wacki2d_shbin, wacki2d_shbin_size);
    if (!s_dvlb) {
        LOG_ERROR("3ds-gpu", "vertex shader not loaded");
        return 0;
    }
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    C3D_BindProgram(&s_prog);
    s_u_proj = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");

    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 2);          /* position */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);          /* texcoord */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);  /* colour   */

    s_vbuf = (Vtx *)linearAlloc(sizeof(Vtx) * QUAD_COUNT * 4);
    if (!s_vbuf) {
        LOG_ERROR("3ds-gpu", "no linear memory for vertices");
        return 0;
    }
    memset(s_vbuf, 0, sizeof(Vtx) * QUAD_COUNT * 4);
    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, s_vbuf, sizeof(Vtx), 3, 0x210);

    /* Texture colour × vertex colour (white): the picture as it is. */
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    for (int i = 1; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));

    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

    if (!alloc_eye(0)) return 0;
    return 1;
}

/* ---- filtered bottom-screen zoom (modes 2–4) ------------------------ */

static C3D_Tex   s_ztex;
static int       s_ztex_ok;
static uint16_t *s_zstage;               /* linear, ZTEX_W × ZTEX_H */
static uint32_t *s_zbuf_a, *s_zbuf_b;    /* 0x00RRGGBB work buffers */
static int       s_zoom_failed;

static int alloc_zoom_tex(void)
{
    if (s_ztex_ok) return 1;
    if (s_zoom_failed) return 0;
    if (!C3D_TexInitVRAM(&s_ztex, ZTEX_W, ZTEX_H, GPU_RGB565) &&
        !C3D_TexInit(&s_ztex, ZTEX_W, ZTEX_H, GPU_RGB565)) goto fail;
    C3D_TexSetWrap(&s_ztex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&s_ztex, GPU_NEAREST, GPU_NEAREST);   /* drawn 1:1 */
    s_zstage = (uint16_t *)linearAlloc(ZTEX_W * ZTEX_H * sizeof(uint16_t));
    s_zbuf_a = (uint32_t *)malloc(BOT_WIDTH * BOT_HEIGHT * sizeof(uint32_t));
    s_zbuf_b = (uint32_t *)malloc(BOT_WIDTH * BOT_HEIGHT * sizeof(uint32_t));
    if (!s_zstage || !s_zbuf_a || !s_zbuf_b) {
        C3D_TexDelete(&s_ztex);
        goto fail;
    }
    memset(s_zstage, 0, ZTEX_W * ZTEX_H * sizeof(uint16_t));
    s_ztex_ok = 1;
    return 1;
fail:
    if (s_zstage) { linearFree(s_zstage); s_zstage = NULL; }
    free(s_zbuf_a); free(s_zbuf_b); s_zbuf_a = s_zbuf_b = NULL;
    s_zoom_failed = 1;
    LOG_ERROR("3ds-gpu", "no memory for the zoom filter — plain zoom");
    return 0;
}

/* The visible part of the frame (rw×rh at sx,sy; rw*zoom = 320) through
 * the pixel filter into s_zstage, queued for upload into s_ztex. */
static void build_zoom_filtered(const uint8_t *shadow, const uint8_t *pal,
                                int w, int sx, int sy, int rw, int rh,
                                int zoom, int kind)
{
    uint32_t lut[256];
    for (int i = 0; i < 256; ++i)
        lut[i] = ((uint32_t)pal[i*3] << 16) | ((uint32_t)pal[i*3+1] << 8) | pal[i*3+2];
    for (int y = 0; y < rh; ++y) {
        const uint8_t *src = shadow + (size_t)(sy + y) * w + sx;
        uint32_t      *dst = s_zbuf_a + (size_t)y * rw;
        for (int x = 0; x < rw; ++x) dst[x] = lut[src[x]];
    }
    uint32_t *res = s_zbuf_b;
    PixelUpscale2x(kind, s_zbuf_a, rw, rh, s_zbuf_b);           /* 2× */
    if (zoom == 4) {
        PixelUpscale2x(kind, s_zbuf_b, rw * 2, rh * 2, s_zbuf_a); /* 4× */
        res = s_zbuf_a;
    }
    for (int y = 0; y < BOT_HEIGHT; ++y) {
        const uint32_t *srow = res + (size_t)y * BOT_WIDTH;
        uint16_t       *d    = s_zstage + (size_t)y * ZTEX_W;
        for (int x = 0; x < BOT_WIDTH; ++x) {
            uint32_t c = srow[x];
            d[x] = (uint16_t)((((c >> 19) & 0x1F) << 11) | (((c >> 10) & 0x3F) << 5) |
                              ((c >> 3) & 0x1F));
        }
    }
    GSPGPU_FlushDataCache(s_zstage, ZTEX_W * BOT_HEIGHT * sizeof(uint16_t));
    C3D_SyncDisplayTransfer((u32 *)s_zstage, GX_BUFFER_DIM(ZTEX_W, ZTEX_H),
                            (u32 *)s_ztex.data, GX_BUFFER_DIM(ZTEX_W, ZTEX_H),
                            TEXTURE_TRANSFER_FLAGS);
}

static int s_zoom_filter_dirty = 0;

/* SELECT+X (gamepad_3ds.c): next zoom filter, saved, with a hint. */
void platform_video_cycle_zoom_filter(void)
{
    extern void ConfigSave(void);
    g_zoom_filter = (zoom_filter_mode() + 1) % ZOOM_FILTER_COUNT;
    ConfigSave();
    s_prev_smooth = -1;                  /* mag filter set again */
    s_zoom_filter_dirty = 1;
    char msg[80];
    snprintf(msg, sizeof msg, "Filtr zoomu: %s%s", k_zoom_filter_names[g_zoom_filter],
             ZOOM_LEVELS[g_zoom_level] == 2 || ZOOM_LEVELS[g_zoom_level] == 4
                 ? "" : " (dzia\xC5\x82""a przy zoomie 2x/4x, przycisk X)");
    SubtitlesHint(msg, 2000);
    perf_log_msg("zoom filter: %d", g_zoom_filter);
}

/* SELECT+START (gamepad_3ds.c): wide 800-px top screen on/off. */
void platform_video_toggle_wide(void)
{
    extern void ConfigSave(void);
    int on = (g_wide_top != 0);
    g_wide_top = on ? 0 : 1;
    ConfigSave();
    const char *msg;
    if (!s_wide_supported)
        msg = "G\xC3\xB3rny ekran 800 px: niedost\xC4\x99pny na tej konsoli (2DS)";
    else if (g_wide_top && osGet3DSliderState() > 0.0f)
        msg = "G\xC3\xB3rny ekran 800 px: w\xC5\x82\xC4\x85""czony (dzia\xC5\x82""a przy suwaku 3D w d\xC3\xB3\xC5\x82)";
    else
        msg = g_wide_top ? "G\xC3\xB3rny ekran 800 px: w\xC5\x82\xC4\x85""czony (ostrzejszy)"
                         : "G\xC3\xB3rny ekran 800 px: wy\xC5\x82\xC4\x85""czony";
    SubtitlesHint(msg, 2500);
}

static void gpu_shutdown(void)
{
    if (!s_c3d_up) return;
    if (s_ztex_ok) { C3D_TexDelete(&s_ztex); s_ztex_ok = 0; }
    if (s_zstage) { linearFree(s_zstage); s_zstage = NULL; }
    free(s_zbuf_a); free(s_zbuf_b); s_zbuf_a = s_zbuf_b = NULL;
    for (int i = 0; i < 2; ++i) {
        if (s_tex_ok[i]) C3D_TexDelete(&s_tex[i]);
        if (s_stage[i]) linearFree(s_stage[i]);
        s_tex_ok[i] = 0;
        s_stage[i]  = NULL;
    }
    if (s_vbuf) { linearFree(s_vbuf); s_vbuf = NULL; }
    for (int i = 0; i < 2; ++i)
        if (s_rt_top[i]) { C3D_RenderTargetDelete(s_rt_top[i]); s_rt_top[i] = NULL; }
    if (s_rt_bot) { C3D_RenderTargetDelete(s_rt_bot); s_rt_bot = NULL; }
    if (s_rt_wide) { C3D_RenderTargetDelete(s_rt_wide); s_rt_wide = NULL; }
    if (s_wide_active) { gfxSetWide(false); s_wide_active = 0; }
    if (s_dvlb) {
        shaderProgramFree(&s_prog);
        DVLB_Free(s_dvlb);
        s_dvlb = NULL;
    }
    C3D_Fini();
    s_c3d_up = 0;
}

/* ---- HAL entry points kept from video_3ds_gl.c ----------------------- */

void platform_video_cycle_zoom(void)
{
    g_zoom_level = (g_zoom_level + 1) % NUM_ZOOM_LEVELS;
    int zoom = ZOOM_LEVELS[g_zoom_level];
    if (zoom == 0)
        LOG_INFO("3ds", "zoom level: no zoom (full view)");
    else
        LOG_INFO("3ds", "zoom level: %dx", zoom);
}

/* While a touch is held the zoom crop does not follow the cursor — see
 * video_3ds_gl.c's platform_video_set_touch_active (runaway-drift fix). */
void platform_video_set_touch_active(int active)
{
    s_touch_active = active;
}

/* Touch-panel tap → game coordinates, through the exact region the last
 * frame showed on the bottom screen. */
void platform_video_touch_to_game(int tx, int ty, int *gx, int *gy)
{
    float x_ratio = (float)s_zoom_region_w / BOT_WIDTH;
    float y_ratio = (float)s_zoom_region_h / BOT_HEIGHT;
    int x = s_zoom_src_x + (int)(tx * x_ratio);
    int y = s_zoom_src_y + (int)(ty * y_ratio);

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > s_fb_w - 1) x = s_fb_w - 1;
    if (y > s_fb_h - 1) y = s_fb_h - 1;

    *gx = x;
    *gy = y;
}

void platform_video_get_present_state(int *stretch_active,
                                      int *win_w, int *win_h,
                                      int *fb_w,  int *fb_h)
{
    *stretch_active = 0;
    *win_w = TOP_WIDTH;
    *win_h = TOP_HEIGHT;
    *fb_w = s_fb_w;
    *fb_h = s_fb_h;
}

void platform_video_toggle_aspect_mode(void)
{
    WideAspectCycle();     /* wide -> wide-bars -> 4:3, saved, hint */
    s_have_prev_frame = 0; /* full redraw next frame */
}

void plat_video_message_box(const char *title, const char *body)
{
    LOG_INFO("msgbox", "%s: %s", title ? title : "", body ? body : "");
    perf_log_msg("msgbox: %s: %s", title ? title : "", body ? body : "");
}

static void update_state_note(int smooth, int stereo)
{
    extern char g_aspect_mode[16];
    char note[96];
    int  zoom = ZOOM_LEVELS[g_zoom_level];
    snprintf(note, sizeof note, "gpu smooth=%d%s aspect=%s zoom=%s%s", smooth,
             s_wide_active ? " wide800" : "",
             g_aspect_mode,
             zoom == 1 ? "1x" : zoom == 2 ? "2x" : zoom == 4 ? "4x" : "full",
             stereo ? " 3d" : "");
    perf_log_set_state(note);
}

int plat_video_init(int w, int h, const char *title)
{
    (void)title;
    plat_apply_video_prefs();

    s_fb_w = w;
    s_fb_h = h;

    if (w <= 0 || h <= 0 || w >= TEX_W || h >= TEX_H) {
        LOG_ERROR("3ds-gpu", "frame %dx%d does not fit a %dx%d texture", w, h, TEX_W, TEX_H);
        return 0;
    }

    s_shadow = (uint8_t *)malloc((size_t)w * h);
    if (!s_shadow) {
        LOG_ERROR("3ds-gpu", "no memory for the previous-frame copy");
        return 0;
    }
    memset(s_shadow, 0, (size_t)w * h);
    memset(s_palette, 0, sizeof s_palette);

    /* citro3d's display transfer writes RGB8 framebuffers; double
     * buffering on (libctru's default) — the swap happens at vblank. */
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);

    if (!gpu_init()) {
        perf_log_msg("gpu: init failed (see the errors above)");
        gpu_shutdown();
        return 0;
    }

    LOG_INFO("3ds", "citro3d video initialized: %dx%d game -> top=%dx%d bot=%dx%d",
             w, h, TOP_WIDTH, TOP_HEIGHT, BOT_WIDTH, BOT_HEIGHT);
    perf_log_msg("gpu: citro3d up, game %dx%d, texture %dx%d, linear free %luK, vram free %luK",
                 w, h, TEX_W, TEX_H, (unsigned long)(linearSpaceFree() / 1024),
                 (unsigned long)(vramSpaceFree() / 1024));
    return 1;
}

void plat_video_present(const uint8_t *shadow, const uint8_t *pal, int w, int h)
{
    if (!shadow || !pal || !s_c3d_up) return;
    uint64_t t_enter = perf_now();

    if (w != s_fb_w || h != s_fb_h) {
        /* The engine draws one fixed size; anything else is a bug. */
        static int s_logged;
        if (!s_logged) {
            LOG_ERROR("3ds-gpu", "present %dx%d, expected %dx%d — frame dropped",
                      w, h, s_fb_w, s_fb_h);
            s_logged = 1;
        }
        return;
    }

    /* ---- what changed ---------------------------------------------- */
    int top_dirty = !s_have_prev_frame ||
                    memcmp(s_shadow, shadow, (size_t)w * h) != 0 ||
                    memcmp(s_palette, pal, sizeof s_palette) != 0;

    int zoom = ZOOM_LEVELS[g_zoom_level];   /* 0 = whole frame */
    int bot_dirty = top_dirty ||
                    g_mouse_x != s_prev_mouse_x ||
                    g_mouse_y != s_prev_mouse_y ||
                    g_zoom_level != s_prev_zoom_level ||
                    s_zoom_filter_dirty;
    s_zoom_filter_dirty = 0;

    /* During cutscenes the bottom screen keeps its last picture (no
     * cursor to magnify) — same as video_3ds_gl.c. */
    if (g_cutscene_playing) bot_dirty = 0;

    /* ---- stereo 3D: the physical slider, read every frame ---------- */
    float slider = osGet3DSliderState();
    int stereo_enabled = slider > 0.0f;
    int disparity_px_now = (int)(slider * STEREO_MAX_DISPARITY_SRC_PX + 0.5f);
    g_stereo3d_bg_layer_wanted = stereo_enabled;
    if (stereo_enabled && !s_tex_ok[1]) {
        static int s_eye1_failed;    /* no memory once: don't retry every frame */
        if (s_eye1_failed || !alloc_eye(1)) {
            s_eye1_failed = 1;
            stereo_enabled = 0;      /* stay flat */
            g_stereo3d_bg_layer_wanted = 0;
        }
    }
    /* Wide (800 px) top screen when allowed; stereo and wide exclude
     * each other (gfxSet3D / gfxSetWide set the same mode). Switching
     * re-links the top screen's output to the matching render target —
     * done here, outside a frame. */
    int wide_now = (g_wide_top != 0) && s_wide_supported && s_rt_wide && !stereo_enabled;
    if (wide_now != s_wide_active) {
        C3D_RenderTargetSetOutput(wide_now ? s_rt_wide : s_rt_top[0],
                                  GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
        s_wide_active = wide_now;
        top_dirty = 1;
        perf_log_msg("gpu: top screen %s", wide_now ? "800 px (wide)" : "400 px");
    }
    if (wide_now) gfxSetWide(true);
    else          gfxSet3D(stereo_enabled);

    if (stereo_enabled != s_prev_stereo_enabled) {
        LOG_INFO("3ds", "stereo 3D %s (slider=%d%%)",
                 stereo_enabled ? "enabled" : "disabled", (int)(slider * 100.0f));
        s_prev_stereo_enabled = stereo_enabled;
        top_dirty = 1;
    }
    if (stereo_enabled && disparity_px_now != s_prev_disparity_px)
        top_dirty = 1;

    /* Grafika button 1 ("better graphics") → smoothing. (After the
     * stereo block: a just-made right-eye texture gets its filter here.) */
    int smooth = GraphicsAlphaFxEnabled() ? 1 : 0;
    if (smooth != s_prev_smooth) {
        apply_filter(smooth);
        if (s_prev_smooth != -1)
            perf_log_msg("gpu: smoothing %s", smooth ? "on" : "off");
        s_prev_smooth = smooth;
        top_dirty = bot_dirty = 1;
    }


    /* ---- panorama layout (util/wide_layout.c) ----------------------- */
    WideRect wr;
    int  wide_on   = WideLayoutGet(TOP_WIDTH, TOP_HEIGHT, &wr);
    int  use_43    = !wide_on;
    int  pan_dirty = memcmp(&wr, &s_prev_wr, sizeof wr) != 0;
    s_prev_wr = wr;
    int top_redraw = top_dirty || pan_dirty;     /* top_dirty = new content */

    if (!top_redraw && !bot_dirty) {
        perf_log_present(0, 0, 0, 0.0f);
        return;
    }

    memcpy(s_shadow, shadow, (size_t)w * h);
    memcpy(s_palette, pal, sizeof s_palette);
    s_have_prev_frame = 1;
    s_prev_mouse_x = g_mouse_x;
    s_prev_mouse_y = g_mouse_y;
    s_prev_zoom_level = g_zoom_level;

    /* ---- bottom screen's source region (kept for touch) ------------- */
    int src_region_w, src_region_h, src_x, src_y;
    if (zoom == 0) {
        src_region_w = w;
        src_region_h = h;
        src_x = 0;
        src_y = 0;
    } else {
        src_region_w = BOT_WIDTH / zoom;
        src_region_h = BOT_HEIGHT / zoom;
        if (s_touch_active) {
            src_x = s_zoom_src_x;
            src_y = s_zoom_src_y;
        } else {
            int cx = g_mouse_x, cy = g_mouse_y;
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            if (cx >= w) cx = w - 1;
            if (cy >= h) cy = h - 1;
            src_x = cx - src_region_w / 2;
            src_y = cy - src_region_h / 2;
            if (src_x < 0) src_x = 0;
            if (src_y < 0) src_y = 0;
            if (src_x + src_region_w > w) src_x = w - src_region_w;
            if (src_y + src_region_h > h) src_y = h - src_region_h;
            if (src_x < 0) src_x = 0;
            if (src_y < 0) src_y = 0;
        }
    }
    s_zoom_src_x = src_x;
    s_zoom_src_y = src_y;
    s_zoom_region_w = src_region_w;
    s_zoom_region_h = src_region_h;

    /* ---- GPU frame --------------------------------------------------- */
    uint64_t t_wait0 = perf_now();
    if (!C3D_FrameBegin(0)) {          /* waits for the GPU, not for vblank */
        static int s_logged;
        if (!s_logged) { LOG_ERROR("3ds-gpu", "C3D_FrameBegin failed"); s_logged = 1; }
        s_have_prev_frame = 0;         /* try the whole frame again next time */
        perf_log_present(0, 0, 0, 0.0f);
        return;
    }
    uint64_t t_wait1 = perf_now();
    float gpu_ms = C3D_GetDrawingTime();

    if (top_dirty) {
        build_palette_lut(pal);
        convert_frame(s_stage[0], shadow, w, h);
        upload_texture(0, h);
        s_have_right = 0;
        if (stereo_enabled && g_bg_layer_valid && g_bg_layer_shadow) {
            convert_frame_stereo_right(s_stage[1], shadow, g_bg_layer_shadow,
                                       w, h, disparity_px_now);
            upload_texture(1, h);
            s_have_right = 1;
        }
        /* Used up: the next gameplay frame takes a new snapshot
         * (frame_tick.c). Screens that paint and present on their own
         * (menus, Grafika over a paused scene) then stay flat instead of
         * getting depth from an old scene's background. */
        g_bg_layer_valid = 0;
    }

    /* top screen width in its own pixels: 400, or 800 in wide mode (then
     * 4:3 = 640 columns: the frame 1:1 across) */
    int   top_w = s_wide_active ? WIDE_WIDTH : TOP_WIDTH;
    int   w43   = s_wide_active ? TOP_43_W * 2 : TOP_43_W;
    float kx  = (float)top_w / (float)TOP_WIDTH;     /* 1, or 2 when wide */
    float tx0 = use_43 ? (float)((top_w - w43) / 2) : wr.dst_x * kx;
    float tx1 = use_43 ? (float)((top_w - w43) / 2 + w43) : (wr.dst_x + wr.dst_w) * kx;
    float ty0 = use_43 ? 0.0f : wr.dst_y;
    float ty1 = use_43 ? (float)TOP_HEIGHT : wr.dst_y + wr.dst_h;
    float sy0 = use_43 ? 0.0f : wr.src_y;
    float sy1 = use_43 ? (float)h : wr.src_y + wr.src_h;
    set_quad(QUAD_TOP_L, tx0, ty0, tx1, ty1, 0.0f, sy0, (float)w, sy1);
    set_quad(QUAD_TOP_R, tx0, ty0, tx1, ty1, 0.0f, sy0, (float)w, sy1);
    set_quad(QUAD_BOT, 0.0f, 0.0f, (float)BOT_WIDTH, (float)BOT_HEIGHT,
             (float)src_x, (float)src_y,
             (float)(src_x + src_region_w), (float)(src_y + src_region_h));
    GSPGPU_FlushDataCache(s_vbuf, sizeof(Vtx) * QUAD_COUNT * 4);

    if (top_redraw) {
        draw_on(s_wide_active ? s_rt_wide : s_rt_top[0], top_w, TOP_HEIGHT,
                &s_tex[0], QUAD_TOP_L);
        if (stereo_enabled) {
            /* Without a background snapshot this frame (menus paint and
             * present directly), both eyes get the same flat picture. */
            draw_on(s_rt_top[1], TOP_WIDTH, TOP_HEIGHT,
                    s_have_right ? &s_tex[1] : &s_tex[0], QUAD_TOP_R);
        }
        s_prev_disparity_px = disparity_px_now;
    }
    if (bot_dirty) {
        int zf = zoom_filter_mode();
        if (zf >= 2 && (zoom == 2 || zoom == 4) &&
            src_region_w * zoom == BOT_WIDTH && src_region_h * zoom == BOT_HEIGHT &&
            alloc_zoom_tex()) {
            int kind = zf == 2 ? PIXF_SCALE2X : zf == 3 ? PIXF_SAI2X : PIXF_XBR2X;
            build_zoom_filtered(shadow, pal, w, src_x, src_y,
                                src_region_w, src_region_h, zoom, kind);
            set_quad(QUAD_BOT, 0.0f, 0.0f, (float)BOT_WIDTH, (float)BOT_HEIGHT,
                     0.0f, 0.0f, (float)BOT_WIDTH, (float)BOT_HEIGHT);
            /* that quad's UVs are for a 1024×512 texture: rescale */
            Vtx *v = s_vbuf + QUAD_BOT * 4;
            for (int i = 0; i < 4; ++i) {
                v[i].u *= (float)TEX_W / (float)ZTEX_W;
                v[i].v  = 1.0f - (1.0f - v[i].v) * (float)TEX_H / (float)ZTEX_H;
            }
            GSPGPU_FlushDataCache(s_vbuf, sizeof(Vtx) * QUAD_COUNT * 4);
            draw_on(s_rt_bot, BOT_WIDTH, BOT_HEIGHT, &s_ztex, QUAD_BOT);
        } else {
            draw_on(s_rt_bot, BOT_WIDTH, BOT_HEIGHT, &s_tex[0], QUAD_BOT);
        }
    }

    C3D_FrameEnd(GX_CMDLIST_FLUSH);    /* we flushed our own buffers */
    uint64_t t_end = perf_now();

    uint64_t wait = t_wait1 - t_wait0;
    uint64_t draw = (t_end - t_enter) - wait;
    perf_log_present(1, draw, wait, gpu_ms);

    static int s_note_smooth = -1, s_note_43 = -1, s_note_zoom = -1, s_note_3d = -1;
    if (smooth != s_note_smooth || use_43 != s_note_43 ||
        g_zoom_level != s_note_zoom || stereo_enabled != s_note_3d) {
        s_note_smooth = smooth; s_note_43 = use_43;
        s_note_zoom = g_zoom_level; s_note_3d = stereo_enabled;
        update_state_note(smooth, stereo_enabled);
    }
}

void plat_video_shutdown(void)
{
    gpu_shutdown();
    free(s_shadow);
    s_shadow = NULL;
}
