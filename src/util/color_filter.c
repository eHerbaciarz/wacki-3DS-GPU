/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/util/color_filter.c — colour filter on the palette, for every
 * platform: the game is 8-bit with a 256-colour palette, so changing the
 * picture's colours means changing 256 entries — nothing per pixel.
 * Applied in FlushFrameToPrimary (graphics.c) to the palette handed to
 * the platform; recomputed only when the palette or the mode changes.
 *
 *   0 oryginalne   — as the game made them
 *   1 żywe         — a bit more saturation and contrast (for the pale
 *                    screens of the 3DS / handhelds)
 *   2 bardzo żywe  — more of both
 *   3 jaśniejsze   — brighter dark parts (gamma), a little saturation
 *
 * wacki.cfg: color_filter=0..3 (default 1 on the 3DS, 0 elsewhere).
 * 3DS: SELECT+Y cycles it (gamepad_3ds.c). */

#include "wacki.h"
#include "wacki/log.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifdef WACKI_3DS
#define COLOR_FILTER_DEFAULT 1
#else
#define COLOR_FILTER_DEFAULT 0
#endif

int g_color_filter = -1;                 /* -1 = platform default */

typedef struct { float sat, con, gamma; } ColorParams;
static const ColorParams k_modes[COLOR_FILTER_COUNT] = {
    { 1.00f, 1.00f, 1.00f },             /* oryginalne  */
    { 1.25f, 1.06f, 1.00f },             /* żywe        */
    { 1.50f, 1.12f, 0.95f },             /* bardzo żywe */
    { 1.10f, 1.00f, 0.80f },             /* jaśniejsze  */
};
static const char *const k_names[COLOR_FILTER_COUNT] = {
    "oryginalne", "\xC5\xBCywe", "bardzo \xC5\xBCywe", "ja\xC5\x9Bniejsze",
};

int ColorFilterMode(void)
{
    int m = g_color_filter < 0 ? COLOR_FILTER_DEFAULT : g_color_filter;
    return (m >= 0 && m < COLOR_FILTER_COUNT) ? m : 0;
}

const char *ColorFilterName(int mode)
{
    return (mode >= 0 && mode < COLOR_FILTER_COUNT) ? k_names[mode] : "?";
}

/* x^g for x in (0,1], without libm (not every target links it):
 * ln by range reduction + atanh series, exp by halving + Taylor. */
static float pow01(float x, float g)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    int k = 0;
    while (x < 0.5f) { x *= 2.0f; ++k; }
    float t = (x - 1.0f) / (x + 1.0f), t2 = t * t, term = t, ln = 0.0f;
    for (int n = 1; n < 24; n += 2) { ln += term / (float)n; term *= t2; }
    ln = 2.0f * ln - (float)k * 0.69314718f;
    float y = g * ln;                       /* ≤ 0 */
    int   h = 0;
    while (y < -0.5f) { y *= 0.5f; ++h; }
    float e = 1.0f, tt = 1.0f;
    for (int n = 1; n < 12; ++n) { tt *= y / (float)n; e += tt; }
    while (h-- > 0) e *= e;
    return e;
}

static uint8_t clamp8(float v)
{
    if (v <= 0.0f)   return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

void ColorFilterApply(int mode, const uint8_t *in, uint8_t *out)
{
    if (mode <= 0 || mode >= COLOR_FILTER_COUNT) {
        memcpy(out, in, 768);
        return;
    }
    const ColorParams *p = &k_modes[mode];
    float g_lut[256];
    for (int i = 0; i < 256; ++i)
        g_lut[i] = p->gamma == 1.0f ? (float)i
                 : 255.0f * pow01((float)i / 255.0f, p->gamma);
    for (int i = 0; i < 256; ++i) {
        float r = g_lut[in[i*3]], g = g_lut[in[i*3+1]], b = g_lut[in[i*3+2]];
        float l = 0.299f * r + 0.587f * g + 0.114f * b;
        r = l + (r - l) * p->sat;
        g = l + (g - l) * p->sat;
        b = l + (b - l) * p->sat;
        out[i*3]   = clamp8((r - 128.0f) * p->con + 128.0f);
        out[i*3+1] = clamp8((g - 128.0f) * p->con + 128.0f);
        out[i*3+2] = clamp8((b - 128.0f) * p->con + 128.0f);
    }
}

const uint8_t *ColorFilterPalette(const uint8_t *pal)
{
    static uint8_t s_in[768], s_out[768];
    static int     s_mode = -1;
    int mode = ColorFilterMode();
    if (mode == 0 || !pal) return pal;
    if (mode != s_mode || memcmp(s_in, pal, sizeof s_in) != 0) {
        memcpy(s_in, pal, sizeof s_in);
        ColorFilterApply(mode, s_in, s_out);
        s_mode = mode;
    }
    return s_out;
}

void ColorFilterCycle(void)
{
    extern void ConfigSave(void);
    g_color_filter = (ColorFilterMode() + 1) % COLOR_FILTER_COUNT;
    ConfigSave();
    char msg[64];
    snprintf(msg, sizeof msg, "Kolory: %s", ColorFilterName(g_color_filter));
    SubtitlesHint(msg, 1500);
    LOG_INFO("filter", "color_filter=%d", g_color_filter);
}
