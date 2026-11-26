/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * tests/test_filters.c — colour filter (util/color_filter.c) and the
 * pixel-art upscalers (util/pixel_filters.c). */

#include "test.h"
#include "wacki.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int sat_of(const uint8_t *c)
{
    int mx = c[0], mn = c[0];
    for (int i = 1; i < 3; ++i) { if (c[i] > mx) mx = c[i]; if (c[i] < mn) mn = c[i]; }
    return mx - mn;
}

TEST(color_filter_modes)
{
    uint8_t pal[768], out[768];
    for (int i = 0; i < 256; ++i) {
        pal[i*3] = (uint8_t)i; pal[i*3+1] = (uint8_t)(255 - i); pal[i*3+2] = 100;
    }
    pal[0] = pal[1] = pal[2] = 128;              /* mid grey */
    pal[3] = 40; pal[4] = 40; pal[5] = 40;       /* dark grey */

    ColorFilterApply(0, pal, out);
    ASSERT_MEMEQ(out, pal, 768);                 /* original: identity */

    ColorFilterApply(1, pal, out);               /* żywe */
    ASSERT_EQ(out[0], 128); ASSERT_EQ(out[1], 128); ASSERT_EQ(out[2], 128);
    ASSERT_TRUE(sat_of(out + 30*3) > sat_of(pal + 30*3));

    ColorFilterApply(3, pal, out);               /* jaśniejsze */
    ASSERT_TRUE(out[3] > pal[3]);

    /* the cached path follows g_color_filter */
    int keep = g_color_filter;
    g_color_filter = 0;
    ASSERT_TRUE(ColorFilterPalette(pal) == pal);
    g_color_filter = 2;
    const uint8_t *p = ColorFilterPalette(pal);
    ASSERT_TRUE(p != pal);
    ASSERT_TRUE(sat_of(p + 30*3) > sat_of(pal + 30*3));
    g_color_filter = keep;
}

TEST(upscalers_keep_flat_areas_and_size)
{
    enum { W = 8, H = 6 };
    uint32_t src[W*H], out[W*2*H*2];
    for (int i = 0; i < W*H; ++i) src[i] = 0x336699;
    for (int k = PIXF_SCALE2X; k <= PIXF_XBR2X; ++k) {
        memset(out, 0, sizeof out);
        PixelUpscale2x(k, src, W, H, out);
        for (int i = 0; i < W*2*H*2; ++i) ASSERT_EQ(out[i], 0x336699u);
    }
}

TEST(scale2x_known_pattern)
{
    /* a black 45° staircase on white: Scale2x fills the inner corner */
    uint32_t W_ = 0xFFFFFF, B_ = 0;
    uint32_t src[9] = { B_, W_, W_,
                        B_, B_, W_,
                        B_, B_, B_ };
    uint32_t out[36];
    PixelUpscale2x(PIXF_SCALE2X, src, 3, 3, out);
    /* centre pixel (1,1)=B with b=W(up) f=W(right): top-right sub-pixel
     * takes the edge colour W; the others stay B */
    ASSERT_EQ(out[2*6 + 3], W_);
    ASSERT_EQ(out[2*6 + 2], B_);
    ASSERT_EQ(out[3*6 + 2], B_);
    ASSERT_EQ(out[3*6 + 3], B_);
}

TEST(xbr_blends_a_shallow_edge)
{
    /* a shallow black/white slope: xBR mixes colours along it */
    enum { W = 12, H = 8 };
    uint32_t src[W*H], out[W*H*4];
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            src[y*W + x] = (y * 3 > x) ? 0x000000 : 0xFFFFFF;
    PixelUpscale2x(PIXF_XBR2X, src, W, H, out);
    int mixed = 0;
    for (int i = 0; i < W*H*4; ++i)
        if (out[i] != 0 && out[i] != 0xFFFFFF) ++mixed;
    ASSERT_TRUE(mixed > 0);
    /* corners of the picture: unchanged colours */
    ASSERT_EQ(out[0], 0xFFFFFFu);
    ASSERT_EQ(out[(H*2-1)*W*2], 0x000000u);
}

/* ---- util/wide_layout.c: panorama with the sliding panel ---------- */

extern char g_aspect_mode[16];

static void wide_frame(const char *mode, int mouse_y, int gameplay)
{
    strcpy(g_aspect_mode, mode);
    g_mouse_y = (int16_t)mouse_y;
    if (gameplay) WideLayoutMarkGameplay();
    WideLayoutFrame();
}

static void wide_reset(void)
{
    WideLayoutSetTalking(0);
    wide_frame("wide", 0, 0);              /* inactive: next entry snaps */
}

TEST(wide_layout_crop_and_mapping)
{
    WideRect r;
    int gx = -1, gy = -1;
    wide_reset();
    WideLayoutSetScreen(1280, 720);
    wide_frame("wide", 200, 1);
    ASSERT_TRUE(WideLayoutActive());
    ASSERT_TRUE(WideLayoutGet(1280, 720, &r));
    ASSERT_TRUE(r.scale > 1.99f && r.scale < 2.01f);
    ASSERT_TRUE(r.src_h > 359.9f && r.src_h < 360.1f);     /* 360 of 400 */
    ASSERT_TRUE(r.src_y > 19.9f && r.src_y < 20.1f);       /* 200·40/400 */
    ASSERT_TRUE(r.dst_x > -0.1f && r.dst_w > 1279.9f && r.dst_h > 719.9f);
    /* the cursor's own screen spot maps back onto it */
    ASSERT_TRUE(WideLayoutScreenToGame(1280, 720, 640.0f, (200 - 20) * 2.0f, &gx, &gy));
    ASSERT_EQ(gx, 320);
    ASSERT_TRUE(gy >= 199 && gy <= 200);
    /* bottom edge = row 400: brings the panel in */
    WideLayoutScreenToGame(1280, 720, 10.0f, 719.9f, &gx, &gy);
    ASSERT_TRUE(gy >= 396);
    int top, bottom;
    ASSERT_TRUE(WideLayoutBand(&top, &bottom));
    ASSERT_EQ(top, 20);
    ASSERT_EQ(bottom, 380);
}

TEST(wide_layout_bars_panel_and_off)
{
    WideRect r;
    wide_reset();
    WideLayoutSetScreen(1280, 720);
    wide_frame("wide-bars", 100, 1);
    ASSERT_TRUE(WideLayoutGet(1280, 720, &r));
    ASSERT_TRUE(r.scale > 1.79f && r.scale < 1.81f);       /* 720/400 */
    ASSERT_TRUE(r.dst_x > 63.9f && r.dst_x < 64.1f);       /* side bars */
    ASSERT_TRUE(r.src_y < 0.01f && r.src_h > 399.9f);      /* whole scene */

    /* cursor in the panel: it is shown (entry snaps, no slide) */
    wide_reset();
    wide_frame("wide", 450, 1);
    ASSERT_TRUE(WideLayoutGet(1280, 720, &r));
    ASSERT_TRUE(r.band_l > 479.9f);
    ASSERT_TRUE(r.src_y + r.src_h > 479.9f - 120.0f * (1.0f - 450.0f / 480.0f));

    /* ... but not while a dialogue line is spoken */
    wide_reset();
    WideLayoutSetTalking(1);
    wide_frame("wide", 450, 1);
    ASSERT_TRUE(WideLayoutGet(1280, 720, &r));
    ASSERT_TRUE(r.band_l < 400.1f);
    WideLayoutSetTalking(0);

    /* menus / films / 4:3 / a 4:3 screen: the old picture */
    wide_frame("wide", 100, 0);
    ASSERT_FALSE(WideLayoutActive());
    wide_frame("4:3", 100, 1);
    ASSERT_FALSE(WideLayoutActive());
    WideLayoutSetScreen(640, 480);
    wide_frame("wide", 100, 1);
    ASSERT_FALSE(WideLayoutActive());
    WideLayoutSetScreen(1280, 720);

    /* the button: wide -> wide-bars -> 4:3 -> wide; old "stretch" = wide */
    strcpy(g_aspect_mode, "stretch");
    ASSERT_EQ(WideAspectMode(), WIDE_CROP);
    WideAspectCycle(); ASSERT_STREQ(g_aspect_mode, "wide-bars");
    WideAspectCycle(); ASSERT_STREQ(g_aspect_mode, "4:3");
    WideAspectCycle(); ASSERT_STREQ(g_aspect_mode, "wide");
    wide_reset();
}

TEST(wide_layout_16_10_steam_deck)
{
    WideRect r;
    wide_reset();
    WideLayoutSetScreen(1280, 800);
    wide_frame("wide", 100, 1);
    ASSERT_TRUE(WideLayoutGet(1280, 800, &r));
    ASSERT_TRUE(r.scale > 1.99f && r.scale < 2.01f);      /* exact 2x    */
    ASSERT_TRUE(r.src_y < 0.01f && r.src_h > 399.9f);     /* whole scene */
    ASSERT_TRUE(r.dst_x < 0.01f && r.dst_w > 1279.9f && r.dst_h > 799.9f);
    /* panel in: the scene moves up, still full screen */
    wide_reset();
    wide_frame("wide", 470, 1);
    ASSERT_TRUE(WideLayoutGet(1280, 800, &r));
    ASSERT_TRUE(r.src_y + r.src_h > 470.0f);
    /* the button skips the identical "bars" step on 16:10 */
    strcpy(g_aspect_mode, "wide");
    WideAspectCycle(); ASSERT_STREQ(g_aspect_mode, "4:3");
    WideAspectCycle(); ASSERT_STREQ(g_aspect_mode, "wide");
    WideLayoutSetScreen(1280, 720);
    wide_reset();
}

TEST(wide_layout_3ds_top_screen)
{
    WideRect r;
    wide_reset();
    WideLayoutSetScreen(400, 240);
    wide_frame("wide", 0, 1);
    ASSERT_TRUE(WideLayoutGet(400, 240, &r));
    ASSERT_TRUE(r.src_h > 383.9f && r.src_h < 384.1f);     /* 0.625 */
    wide_frame("wide-bars", 0, 1);
    ASSERT_TRUE(WideLayoutGet(400, 240, &r));
    ASSERT_TRUE(r.dst_x > 7.9f && r.dst_x < 8.1f);
    WideLayoutSetScreen(1280, 720);
    wide_reset();
}

TEST(wide_layout_modern_popup)
{
    extern int g_modern_controls;
    extern int16_t g_mouse_x;
    extern uint16_t g_held_item;
    WideRect r;
    int gx, gy;
    wide_reset();
    g_modern_controls = 1;
    WideLayoutSetScreen(1280, 720);
    wide_frame("wide", 450, 1);                 /* cursor low: no sliding  */
    ASSERT_TRUE(WideLayoutGet(1280, 720, &r));
    ASSERT_TRUE(r.band_l < 400.1f);
    ASSERT_FALSE(r.popup);
    WideLayoutScreenToGame(1280, 720, 100.0f, 719.0f, &gx, &gy);
    ASSERT_TRUE(gy <= 399);                     /* panel strip unreachable */

    g_mouse_x = 100; g_mouse_y = 200; g_held_item = 0;
    WideLayoutTogglePopup();                    /* right button             */
    ASSERT_TRUE(WideLayoutPopupOpen());
    ASSERT_EQ(g_mouse_x, 320);                  /* cursor moved into it     */
    ASSERT_TRUE(g_mouse_y >= 400);
    wide_frame("wide", g_mouse_y, 1);
    ASSERT_TRUE(WideLayoutGet(1280, 720, &r));
    ASSERT_TRUE(r.popup);
    ASSERT_TRUE(r.p_scale > 1.59f && r.p_scale < 1.61f);   /* 2 x 0.8 */
    ASSERT_TRUE(r.p_x > 127.9f && r.p_x < 128.1f);         /* centred */
    /* the middle of the pop-up is the middle of the panel */
    ASSERT_TRUE(WideLayoutScreenToGame(1280, 720, 640.0f, 360.0f, &gx, &gy));
    ASSERT_EQ(gx, 320);
    ASSERT_TRUE(gy >= 435 && gy <= 445);
    float sx, sy;
    ASSERT_TRUE(WideLayoutGameToScreen(1280, 720, gx, gy, &sx, &sy));
    ASSERT_TRUE(sx > 638.0f && sx < 643.0f && sy > 356.0f && sy < 364.0f);

    /* picking an item up closes it, back to where the cursor was */
    g_held_item = 5;
    wide_frame("wide", g_mouse_y, 1);
    ASSERT_FALSE(WideLayoutPopupOpen());
    ASSERT_EQ(g_mouse_x, 100);
    ASSERT_EQ(g_mouse_y, 200);
    g_held_item = 0;

    /* a click beside it closes it and is swallowed */
    WideLayoutTogglePopup();
    g_mouse_y = 100;
    ASSERT_TRUE(WideLayoutPopupClickOutside());
    ASSERT_FALSE(WideLayoutPopupOpen());
    ASSERT_FALSE(WideLayoutPopupClickOutside());

    /* modern controls work in 4:3 too: the scene fitted, panel pops up */
    WideLayoutSetScreen(640, 480);
    wide_frame("4:3", 100, 1);
    ASSERT_TRUE(WideLayoutActive());
    ASSERT_TRUE(WideLayoutGet(640, 480, &r));
    ASSERT_TRUE(r.scale > 0.99f && r.scale < 1.01f);
    ASSERT_TRUE(r.dst_y > 39.9f && r.dst_y < 40.1f);     /* 640x400 in 640x480 */
    ASSERT_TRUE(r.src_h > 399.9f);
    WideLayoutTogglePopup();
    ASSERT_TRUE(WideLayoutPopupOpen());
    WideLayoutTogglePopup();
    WideLayoutSetScreen(1280, 720);

    g_modern_controls = 0;
    wide_frame("4:3", 100, 1);
    ASSERT_FALSE(WideLayoutActive());               /* classic 4:3: as before */
    wide_reset();
}

SUITE(filters)
{
    RUN_TEST(color_filter_modes);
    RUN_TEST(upscalers_keep_flat_areas_and_size);
    RUN_TEST(scale2x_known_pattern);
    RUN_TEST(xbr_blends_a_shallow_edge);
    RUN_TEST(wide_layout_crop_and_mapping);
    RUN_TEST(wide_layout_bars_panel_and_off);
    RUN_TEST(wide_layout_3ds_top_screen);
    RUN_TEST(wide_layout_16_10_steam_deck);
    RUN_TEST(wide_layout_modern_popup);
}
