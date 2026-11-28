/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/menu/controls_help.c — what the buttons do, shown under the pause
 * question ("Ar ju siur?") that START / + / F12 opens during play.
 *
 * The list is per platform (Switch, Vita, 3DS, PC — the mapping each
 * gamepad_*.c / platform_sdl.c really has) and is drawn in a dark box over
 * the bottom of the picture (frame rows HELP_TOP..479: the characters'
 * feet and the inventory panel, which don't matter while the question is
 * up). Text: the game's own font (Futura.30, with its Polish letters) at
 * half size in two columns — on the 3DS at full size, since its screens
 * show the frame at half its size already.
 *
 * Nothing is painted into the engine's back buffer: ControlsHelpCompose
 * draws on a copy of the finished frame (graphics.c, just before it goes
 * to the screen), so the box is gone the moment the question closes. */

#include "wacki.h"
#include "wacki/log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_W     640
#define FRAME_H     480
#define HELP_TOP    362          /* below the question's board */
#define CELL_H      30           /* Futura.30 cell height */
#define TMP_W       1024
#if defined(__3DS__)
#define HELP_X0     6            /* full-size text: use the whole width */
#define HELP_COL    310
#else
#define HELP_X0     16
#define HELP_COL    (FRAME_W / 2)
#endif

extern uint8_t     g_pause_menu_open;      /* menu_loop.c */
extern FontHandle *g_default_font;         /* game.c: Futura.30 */
extern int MeasureTextLine(FontHandle *f, const uint8_t *text);

/* Two columns, filled top to bottom. Plain ASCII; Polish letters: k_pl. */
#if defined(WACKI_SWITCH)
static const char *k_help[] = {
    "A - klik (idz, uzyj)",
    "B - zmiana postaci",
    "Galka / krzyzak - kursor",
    "Dotyk ekranu - kursor i klik",
    "+ - pauza i ta pomoc",
    "X - obraz: panorama / 4:3",
    "- (minus) - tryb dotyku",
    "R - szybki zapis,  L - wczytaj",
};
#elif defined(WACKI_VITA)
static const char *k_help[] = {
    "Krzyzyk - klik (idz, uzyj)",
    "Kolko - zmiana postaci",
    "Galka / krzyzak - kursor",
    "Dotyk ekranu - kursor i klik",
    "START - pauza i ta pomoc",
    "Trojkat - obraz: panorama / 4:3",
    "Kwadrat - tryb dotyku",
    "R - szybki zapis,  L - wczytaj",
};
#elif defined(__3DS__)
/* L/ZL and R/ZR as in the default hand mode; SELECT swaps the pairs. */
static const char *k_help[] = {
    "A, L - klik",
    "B, ZL - postac",
    "Galka - kursor",
    "Dotyk - kursor, klik",
    "R - zapis, ZR - wczyt.",
    "Y - obraz, X - lupa",
    "START - pauza",
    "SELECT - L/R odwrotnie",
};
#else
static const char *k_help[] = {
    "Mysz: lewy - idz, uzyj",
    "Prawy lub Tab - zmiana postaci",
    "Strzalki - kursor",
    "F12 - pauza i ta pomoc",
    "F10 - obraz: panorama / 4:3",
    "F11 - pelny ekran",
    "F5 - szybki zapis,  F9 - wczytaj",
    "Pad: A klik, B postac, RB zapis",
};
#endif
#define HELP_N ((int)(sizeof k_help / sizeof k_help[0]))

/* The lines above in plain ASCII would be ugly Polish; they get their
 * letters here: each entry lists "plain word" → "word with diacritics"
 * written in the font's own glyph slots (TextTranslationLutInit in
 * text/balloon.c: ą=E2 ć=EA ę=EB ł=EE ń=EF ó=F3 ś=F4 ź=FA ż=FB,
 * Ł=CE Ś=D4 Ż=DB). */
static const struct { const char *from, *to; } k_pl[] = {
    { "idz",      "id\xFA"          },
    { "uzyj",     "u\xFByj"         },
    { "Galka",    "Ga\xEEka"        },
    { "krzyzak",  "krzy\xFB""ak"    },
    { "Krzyzyk",  "Krzy\xFByk"      },
    { "Kolko",    "K\xF3\xEEko"     },
    { "Trojkat",  "Tr\xF3jk\xE2t"   },
    { "powiekszenie", "powi\xEBkszenie" },
    { "Strzalki", "Strza\xEEki"     },
    { "pelny",    "pe\xEEny"        },
    { "postac",   "posta\xEA"       },
};

/* Copy `in` to `out`, swapping the words listed in k_pl. */
static int is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static void to_font(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    char prev = ' ';
    while (*in && n + 1 < cap) {
        int done = 0;
        if (in[0] >= 'A' && !is_letter(prev)) {
            for (size_t i = 0; i < sizeof k_pl / sizeof k_pl[0]; ++i) {
                size_t fl = strlen(k_pl[i].from);
                if (strncmp(in, k_pl[i].from, fl) == 0 && !is_letter(in[fl])) {
                    size_t tl = strlen(k_pl[i].to);
                    if (n + tl + 1 >= cap) break;
                    memcpy(out + n, k_pl[i].to, tl);
                    n += tl; in += fl; done = 1;
                    break;
                }
            }
        }
        if (!done) out[n++] = *in++;
        prev = in[-1];
    }
    out[n] = '\0';
}

/* Darkest and brightest entries of the palette now on screen. */
static void pick_colours(const uint8_t *pal, uint8_t *dark, uint8_t *bright)
{
    int lo = 1 << 30, hi = -1;
    for (int i = 0; i < 256; ++i) {
        const uint8_t *e = pal + i * 3;
        int l = e[0] * 3 + e[1] * 6 + e[2];
        if (l < lo) { lo = l; *dark = (uint8_t)i; }
        if (l > hi) { hi = l; *bright = (uint8_t)i; }
    }
}

/* One line at (x, y) of the frame, at num/den of the font's size (a
 * frame pixel is ink when any source pixel it covers is). The ink gets a
 * 1-px dark outline so it reads on anything. */
static void draw_line(uint8_t *frame, const char *text, int x, int y,
                      int num, int den, uint8_t dark, uint8_t bright)
{
    static uint8_t tmp[TMP_W * CELL_H];
    char buf[160];
    if (!g_default_font) return;
    to_font(text, buf, sizeof buf);
    memset(tmp, 0, sizeof tmp);
    TextRenderTarget t;
    memset(&t, 0, sizeof t);
    t.stride = TMP_W; t.x = 0; t.color_base = 1; t.pixels = tmp; t.font = g_default_font;
    RenderTextLineToBuffer(&t, (const uint8_t *)buf);
    int tw = t.x < TMP_W ? t.x : TMP_W;
    int ow = (tw * num + den - 1) / den, oh = CELL_H * num / den;
    static uint8_t ink[(TMP_W + 2) * (CELL_H + 2)];
    int iw = ow + 2;
    memset(ink, 0, sizeof ink);
    for (int yy = 0; yy < oh; ++yy)
        for (int xx = 0; xx < ow; ++xx) {
            int on = 0;
            int x0 = xx * den / num, x1 = ((xx + 1) * den + num - 1) / num;
            int y0 = yy * den / num, y1 = ((yy + 1) * den + num - 1) / num;
            for (int py = y0; py < y1 && !on; ++py)
                for (int px = x0; px < x1 && !on; ++px)
                    if (px < TMP_W && py < CELL_H && tmp[py * TMP_W + px]) on = 1;
            ink[(yy + 1) * iw + xx + 1] = (uint8_t)on;
        }
    for (int yy = -1; yy <= oh; ++yy) {
        int fy = y + yy;
        if (fy < 0 || fy >= FRAME_H) continue;
        for (int xx = -1; xx <= ow; ++xx) {
            int fx = x + xx;
            if (fx < 0 || fx >= FRAME_W) continue;
            const uint8_t *c = ink + (yy + 1) * iw + (xx + 1);
            if (xx >= 0 && xx < ow && yy >= 0 && yy < oh && *c) {
                frame[fy * FRAME_W + fx] = bright;
            } else {
                int near = 0;
                for (int dy = -1; dy <= 1 && !near; ++dy)
                    for (int dx = -1; dx <= 1 && !near; ++dx) {
                        int ax = xx + dx, ay = yy + dy;
                        if (ax >= 0 && ax < ow && ay >= 0 && ay < oh &&
                            ink[(ay + 1) * iw + ax + 1]) near = 1;
                    }
                if (near) frame[fy * FRAME_W + fx] = dark;
            }
        }
    }
}

static uint8_t *s_copy;

const uint8_t *ControlsHelpCompose(const uint8_t *shadow, const uint8_t *pal,
                                   int w, int h)
{
    if (!g_pause_menu_open || !shadow || !pal || w != FRAME_W || h != FRAME_H ||
        !g_default_font)
        return shadow;
    if (!s_copy) s_copy = (uint8_t *)malloc((size_t)FRAME_W * FRAME_H);
    if (!s_copy) return shadow;
    memcpy(s_copy, shadow, (size_t)FRAME_W * FRAME_H);

    uint8_t dark = 0, bright = 255;
    pick_colours(pal, &dark, &bright);
    memset(s_copy + (size_t)HELP_TOP * FRAME_W, dark,
           (size_t)(FRAME_H - HELP_TOP) * FRAME_W);

#if defined(__3DS__)
    const int num = 1, den = 1;   /* the 3DS shows the frame at half size */
#else
    const int num = 2, den = 3;   /* 20 of the font's 30 rows */
#endif
    const int line_h = CELL_H * num / den + (num < den ? 4 : 0);
    const int rows   = (HELP_N + 1) / 2;
    int y0 = HELP_TOP + (FRAME_H - HELP_TOP - rows * line_h) / 2;
    if (y0 < HELP_TOP) y0 = HELP_TOP;
    for (int i = 0; i < HELP_N; ++i) {
        int col = i / rows, row = i % rows;
        draw_line(s_copy, k_help[i], HELP_X0 + col * HELP_COL, y0 + row * line_h,
                  num, den, dark, bright);
    }
    return s_copy;
}
