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

SUITE(filters)
{
    RUN_TEST(color_filter_modes);
    RUN_TEST(upscalers_keep_flat_areas_and_size);
    RUN_TEST(scale2x_known_pattern);
    RUN_TEST(xbr_blends_a_shallow_edge);
}
