/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/menu/subs_icons.c — the port's own icons in the options menus.
 *
 * Main Options screen: a light bulb = hints (text/hints.c) on/off, off by
 * default; lit with rays when on, grey and crossed out when off. Next to
 * it a mouse = "sterowanie współczesne" (util/wide_layout.c): right
 * button lit = on.
 *
 * Solund (sound) screen, next to the game's own buttons:
 *
 *   [ear + cupped hand]  napisy dla niesłyszących on/off (speaker names
 *                        + sound captions) — the same switch as the 5th
 *                        Solund button, crossed out red when off;
 *   [flag] [flag] …      one per napisy/<code>.txt found next to the game
 *                        data: click = subtitles in that language (loaded
 *                        at once, saved in wacki.cfg). The current one has
 *                        a bright frame, the rest are dimmed.
 *
 * The game has no sprites for this, so the icons are drawn here: a 32×32
 * two-tone bitmap and simple painted flags, with colours taken from the
 * nearest entries of the menu's palette. The row sits under the game's
 * buttons (their bounding box from the menu atlas), or above them when
 * there is no room. Hovering shows what an icon does at the top.
 */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern AnimAsset *g_menu_asset_10;     /* menu_loop.c: the scene's atlas */
extern uint8_t    g_menu_free_click;   /* menu_loop.c: click beside the buttons */
extern char       g_subtitles_lang[16];

#define EAR_N      32
#define FLAG_W     30
#define FLAG_H     20
#define GAP        10
#define MAX_LANGS  10
#define ROW_H      EAR_N

/* ear + hand: '#' fill, 'o' outline, '.' transparent */
static const char *const s_ear[EAR_N] = {
"................................",
"................................",
"................................",
".......oooooooo.................",
"......oo######oo......ooo.......",
".....oo########oo.....o#oo......",
"....oo###oooo###oo.oooo##oo.....",
"...oo###oooooo###ooo#ooo##ooo...",
"...o###ooo##ooo###oo##oo####oo..",
"...o##ooo####ooo##oo##ooo####o..",
"...o##oo##oo##oo##ooo##oo####oo.",
"...o##oooooo##oo##o.oo#ooo#o##o.",
"...o##o....o##oo##o..o##oo#o##o.",
"...oooo...oo######o..o##oo##o#o.",
".........oo###o##oo..oo#oo##o#oo",
"........oo#######o....o##o##o##o",
"........o#######oo....o##o##o##o",
"........o#oo###oo.....o##o##o#oo",
"........oooo##oo......o##o##o#o.",
"..........oo##o.......o##o##o#o.",
"..........o##oo.......o##o##o#o.",
".........oo##o........oo#o##o#o.",
".........o###o.........o####o#o.",
"....oooooo##oo.........o#####oo.",
"....o##oo###o........ooo#####o..",
"....o######oo.......oo#######o..",
"....oo####oo.......oo########o..",
".....oooooo........o###oo####o..",
"...................oo#ooo####o..",
"....................ooo.o####o..",
"........................o####o..",
"........................o####o..",

};

static char s_langs[MAX_LANGS][8];
static int  s_nlangs;
static int  s_row_x = -1, s_row_y = -1;   /* -1: not placed yet */
static int  s_hover = -2;                 /* -1 ear, 0.. flag, -2 none */

/* ---- colours: nearest entry of the current palette ------------------- */

static uint8_t pal_near(int r, int g, int b)
{
    int best = 0;
    long bd = 1L << 30;
    for (int i = 0; i < 256; ++i) {
        int dr = g_palette_rgb[i * 3] - r, dg = g_palette_rgb[i * 3 + 1] - g,
            db = g_palette_rgb[i * 3 + 2] - b;
        long d = 3L * dr * dr + 4L * dg * dg + 2L * db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return (uint8_t)best;
}

static void put(int x, int y, uint8_t c)
{
    if (x >= 0 && y >= 0 && x < WACKI_SCREEN_W && y < WACKI_SCREEN_H)
        g_back_shadow[y * WACKI_SCREEN_W + x] = c;
}

static void fill(int x, int y, int w, int h, uint8_t c)
{
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) put(x + i, y + j, c);
}

static void frame(int x, int y, int w, int h, uint8_t c)
{
    fill(x, y, w, 1, c); fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c); fill(x + w - 1, y, 1, h, c);
}

/* ---- flags ----------------------------------------------------------- */

typedef struct { int r, g, b; } RGB;
#define C_WHITE  { 245, 245, 245 }
#define C_RED    { 215,  30,  40 }
#define C_BLUE   {  20,  50, 150 }
#define C_LBLUE  {  60, 130, 220 }
#define C_YELLOW { 250, 205,  20 }
#define C_GREEN  {   0, 140,  70 }
#define C_BLACK  {  15,  15,  15 }

/* flag colour at (x,y) of a FLAG_W×FLAG_H flag; dim halves the brightness */
static RGB flag_px(const char *code, int x, int y)
{
    static const RGB W = C_WHITE, R = C_RED, B = C_BLUE, LB = C_LBLUE,
                     Y = C_YELLOW, G = C_GREEN, K = C_BLACK;
    int h3 = y * 3 / FLAG_H, v3 = x * 3 / FLAG_W, h2 = y * 2 / FLAG_H;
    if (!strcmp(code, "pl")) return h2 ? R : W;
    if (!strcmp(code, "de")) return h3 == 0 ? K : h3 == 1 ? R : Y;
    if (!strcmp(code, "fr")) return v3 == 0 ? B : v3 == 1 ? W : R;
    if (!strcmp(code, "it")) return v3 == 0 ? G : v3 == 1 ? W : R;
    if (!strcmp(code, "ru")) return h3 == 0 ? W : h3 == 1 ? B : R;
    if (!strcmp(code, "nl")) return h3 == 0 ? R : h3 == 1 ? W : B;
    if (!strcmp(code, "hu")) return h3 == 0 ? R : h3 == 1 ? W : G;
    if (!strcmp(code, "uk") || !strcmp(code, "ua")) return h2 ? Y : LB;
    if (!strcmp(code, "es")) { int q = y * 4 / FLAG_H; return (q == 0 || q == 3) ? R : Y; }
    if (!strcmp(code, "pt")) return x * 5 < FLAG_W * 2 ? G : R;
    if (!strcmp(code, "sk")) return h3 == 0 ? W : h3 == 1 ? B : R;
    if (!strcmp(code, "cs") || !strcmp(code, "cz")) {
        /* blue triangle from the hoist to the middle */
        int tx = (y < FLAG_H / 2 ? y : FLAG_H - 1 - y) * FLAG_W / FLAG_H;
        if (x <= tx) return B;
        return h2 ? R : W;
    }
    if (!strcmp(code, "sv") || !strcmp(code, "se")) {
        int cx = FLAG_W * 5 / 16, cy = FLAG_H / 2;
        return (x >= cx - 2 && x <= cx + 1) || (y >= cy - 2 && y <= cy + 1) ? Y : LB;
    }
    if (!strcmp(code, "en") || !strcmp(code, "gb")) {
        /* Union Jack, simplified: blue, white saltire with red, white +
         * red cross */
        int cx = FLAG_W / 2, cy = FLAG_H / 2;
        int dx = x - cx, dy = y - cy;
        if ((dx >= -2 && dx <= 1) || (dy >= -2 && dy <= 1)) return R;
        if ((dx >= -4 && dx <= 3) || (dy >= -4 && dy <= 3)) return W;
        int a = dx * FLAG_H - dy * FLAG_W, b = dx * FLAG_H + dy * FLAG_W;
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        int t = FLAG_W * 2;
        if (a < t || b < t) return (a < t / 2 || b < t / 2) ? R : W;
        return B;
    }
    return (RGB){ 120, 120, 125 };          /* unknown: grey + its code */
}

/* 3×5 letters for the code on an unknown flag */
static const uint16_t s_tiny[26] = {
    0x2BED,0x6BAE,0x3923,0x6B6E,0x79A7,0x79A4,0x396B,0x5BED,0x7497,0x126A,0x5BAD,0x4927,0x5FED,0x6B6D,0x2B6A,0x6BA4,0x2B7B,0x6BAD,0x388E,0x7492,0x5B6F,0x5B6A,0x5BFD,0x5AAD,0x5A92,0x72A7 };

static void tiny_text(int x, int y, const char *s, uint8_t c)
{
    for (; *s; ++s, x += 4) {
        if (*s < 'a' || *s > 'z') continue;
        uint16_t g = s_tiny[*s - 'a'];
        for (int j = 0; j < 5; ++j)
            for (int i = 0; i < 3; ++i)
                if (g & (1u << (14 - (j * 3 + i)))) put(x + i, y + j, c);
    }
}

static void paint_flag(int x, int y, const char *code, int current, int hover)
{
    uint8_t lut[8];
    RGB seen[8];
    int nseen = 0;
    for (int j = 0; j < FLAG_H; ++j)
        for (int i = 0; i < FLAG_W; ++i) {
            RGB c = flag_px(code, i, j);
            if (!current && !hover) { c.r = c.r * 9 / 16; c.g = c.g * 9 / 16; c.b = c.b * 9 / 16; }
            int k = 0;
            while (k < nseen && (seen[k].r != c.r || seen[k].g != c.g || seen[k].b != c.b)) ++k;
            if (k == nseen && nseen < 8) { seen[nseen] = c; lut[nseen] = pal_near(c.r, c.g, c.b); ++nseen; }
            put(x + i, y + j, k < 8 ? lut[k] : lut[0]);
        }
    if (flag_px(code, 0, 0).r == 120 && flag_px(code, 0, 0).b == 125)
        tiny_text(x + (FLAG_W - 4 * (int)strlen(code) + 1) / 2, y + (FLAG_H - 5) / 2,
                  code, pal_near(255, 255, 255));
    uint8_t dark = pal_near(0, 0, 0);
    frame(x - 1, y - 1, FLAG_W + 2, FLAG_H + 2, dark);
    if (current || hover) {
        uint8_t hi = current ? pal_near(255, 230, 90) : pal_near(255, 255, 255);
        frame(x - 3, y - 3, FLAG_W + 6, FLAG_H + 6, hi);
        frame(x - 2, y - 2, FLAG_W + 4, FLAG_H + 4, hi);
        frame(x - 4, y - 4, FLAG_W + 8, FLAG_H + 8, dark);
    }
}

/* ---- ear -------------------------------------------------------------- */

static void paint_ear(int x, int y, int on, int hover)
{
    uint8_t fillc = on ? pal_near(250, 248, 235) : pal_near(150, 150, 150);
    if (hover) fillc = pal_near(255, 255, 160);
    uint8_t dark = pal_near(0, 0, 0);
    for (int j = 0; j < EAR_N; ++j)
        for (int i = 0; i < EAR_N; ++i) {
            char c = s_ear[j][i];
            if (c == '#') put(x + i, y + j, fillc);
            else if (c == 'o') put(x + i, y + j, dark);
        }
    if (!on) {                        /* red slash, dark edged */
        uint8_t red = pal_near(230, 25, 25);
        for (int t = 2; t < EAR_N - 2; ++t) {
            int px = x + t, py = y + EAR_N - 1 - t;
            for (int k = -2; k <= 2; ++k) put(px + k, py, (k == -2 || k == 2) ? dark : red);
        }
    }
}

/* ---- layout ------------------------------------------------------------ */

static int row_width(void)
{
    return EAR_N + (s_nlangs ? GAP + s_nlangs * (FLAG_W + GAP) - GAP : 0);
}

/* Under the game's buttons (bbox of the menu atlas frames), or above
 * them when there is no room; centred on them. */
static void place_box(int rw, int rh, int *ox, int *oy)
{
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1;
    AnimAsset *a = g_menu_asset_10;
    if (a && a->off_widths) {
        for (int f = 0; f < a->frame_count && f < 12; ++f) {   /* def + hover */
            int w = a->off_widths[f], h = a->off_heights[f];
            if (w < 2 || h < 2) continue;
            int x = a->off_drawX[f], y = a->off_drawY[f];
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x + w > x1) x1 = x + w;
            if (y + h > y1) y1 = y + h;
        }
    }
    if (x1 < 0) { x0 = 0; x1 = WACKI_SCREEN_W; y0 = 40; y1 = 300; }
    int x = (x0 + x1 - rw) / 2;
    int y = y1 + 14;
    if (y + rh > WACKI_SCREEN_H - 8) y = y0 - 14 - rh;    /* above */
    if (y < 8) y = WACKI_SCREEN_H - 8 - rh;
    if (x < 8) x = 8;
    if (x + rw > WACKI_SCREEN_W - 8) x = WACKI_SCREEN_W - 8 - rw;
    *ox = x;
    *oy = y;
}

static void place_row(void)
{
    place_box(row_width(), ROW_H, &s_row_x, &s_row_y);
}

static int item_at(int mx, int my)
{
    if (s_row_x < 0) return -2;
    if (mx >= s_row_x && mx < s_row_x + EAR_N && my >= s_row_y && my < s_row_y + EAR_N)
        return -1;
    int fy = s_row_y + (ROW_H - FLAG_H) / 2;
    for (int i = 0; i < s_nlangs; ++i) {
        int fx = s_row_x + EAR_N + GAP + i * (FLAG_W + GAP);
        if (mx >= fx - 3 && mx < fx + FLAG_W + 3 && my >= fy - 3 && my < fy + FLAG_H + 3)
            return i;
    }
    return -2;
}

static const char *lang_name(const char *c)
{
    static const char *const t[][2] = {
        {"pl","polski"}, {"en","English"}, {"gb","English"}, {"de","Deutsch"},
        {"fr","Fran\xC3\xA7""ais"}, {"es","Espa\xC3\xB1ol"}, {"it","Italiano"},
        {"cs","\xC4\x8C""e\xC5\xA1tina"}, {"cz","\xC4\x8C""e\xC5\xA1tina"},
        {"sk","Sloven\xC4\x8D""ina"}, {"uk","Ukrainska"}, {"ua","Ukrainska"},
        {"ru","Russkij"}, {"hu","Magyar"}, {"pt","Portugu\xC3\xAA""s"},
        {"nl","Nederlands"}, {"sv","Svenska"}, {"se","Svenska"} };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; ++i)
        if (!strcmp(t[i][0], c)) return t[i][1];
    return c;
}

/* ---- API -------------------------------------------------------------- */

void SubsIconsBegin(void)
{
    s_nlangs = SubtitlesListLangs(s_langs, MAX_LANGS);
    s_row_x = s_row_y = -1;
    s_hover = -2;
    LOG_INFO("napisy", "Solund: %d subtitle language(s)", s_nlangs);
}

void SubsIconsPaint(void)
{
    if (!g_back_shadow) return;
    if (s_row_x < 0) place_row();
    paint_ear(s_row_x, s_row_y, SubtitlesSdhActive(), s_hover == -1);
    int fy = s_row_y + (ROW_H - FLAG_H) / 2;
    for (int i = 0; i < s_nlangs; ++i) {
        int fx = s_row_x + EAR_N + GAP + i * (FLAG_W + GAP);
        paint_flag(fx, fy, s_langs[i], !strcmp(s_langs[i], g_subtitles_lang), s_hover == i);
    }
}

static void flag_hint(int i, uint32_t ms)
{
    char msg[96];
    snprintf(msg, sizeof msg, "Napisy: %s%s", lang_name(s_langs[i]),
             strcmp(s_langs[i], g_subtitles_lang) ? "" : " \xE2\x9C\x93");
    /* (the check mark has no glyph: dropped by the font conversion) */
    char *ck = strstr(msg, " \xE2\x9C\x93");
    if (ck) strcpy(ck, " (wybrane)");
    SubtitlesHint(msg, ms);
}

int SubsIconsUpdate(void)
{
    if (s_row_x < 0) place_row();
    s_hover = item_at(g_mouse_x, g_mouse_y);
    if (s_hover == -2) return 0;
    if (g_menu_free_click) {
        g_menu_free_click = 0;
        if (s_hover == -1) {
            OptionsToggleSdh();                       /* + saved + hint */
        } else {
            SubtitlesSetLang(s_langs[s_hover]);
            flag_hint(s_hover, 2000);
            if (!SubtitlesLoaded())
                SubtitlesHint("Nie uda\xC5\x82o si\xC4\x99 wczyta\xC4\x87 pliku napis\xC3\xB3w", 2500);
        }
        return 1;
    }
    if (s_hover == -1) {
        SubtitlesHint(SubtitlesSdhActive()
            ? "Napisy dla nies\xC5\x82ysz\xC4\x85""cych: w\xC5\x82\xC4\x85""czone"
            : "Napisy dla nies\xC5\x82ysz\xC4\x85""cych: wy\xC5\x82\xC4\x85""czone", 150);
    } else {
        flag_hint(s_hover, 150);
    }
    return 1;
}

/* ---- the light bulb on the main Options screen: hints on/off --------- */

/* 'g' glass, 'f' filament, 'b' screw base, 'o' outline */
static const char *const s_bulb[EAR_N] = {
    "................................",
    "................................",
    "...........oooooooooo...........",
    "..........ooggggggggooo.........",
    "........ooogggggggggggoo........",
    "........oggggggggggggggoo.......",
    ".......oogggggggggggggggo.......",
    "......ooggggggggggggggggoo......",
    "......oggggggggggggggggggo......",
    "......oggggggggggggggggggo......",
    "......oggggggggggggggggggo......",
    "......oggggggggggggggggggo......",
    "......oggggggggggggggggggo......",
    "......oggggggggfffgggggggo......",
    "......ogggggggffffgggggggo......",
    "......ofggggggfggfgggggggo......",
    "......ooggggggfggfggggggoo......",
    ".......ofgggggfggfgggggfo.......",
    ".......oofgggggggggggggoo.......",
    "........oofggggggggggfoo........",
    ".........ooggggggggggoo.........",
    "..........oggggggggggo..........",
    "..........ooggggggggoo..........",
    "...........oggggggggo...........",
    "...........oggggggggo...........",
    "...........ogbbbbbbgo...........",
    "...........oggggggggo...........",
    "...........obbbbbbbbo...........",
    "...........oogggggggo...........",
    "...........obbbbbbbbo...........",
    "...........ooggggggoo...........",
    "............oooooooo............",
};

/* 'b' body, 'r' right button, 'l' seam, 'o' outline — the "sterowanie
 * współczesne" switch next to the bulb */
static const char *const s_mouse[EAR_N] = {
    "................................",
    "................................",
    "............oooooooo............",
    "..........ooobbllrrooo..........",
    ".........oobbbbllrrrroo.........",
    "........oobbbbbllrrrrroo........",
    "........obbbbbbllrrrrrro........",
    ".......oobbbbbbllrrrrrroo.......",
    ".......obbbbbbbllrrrrrrro.......",
    ".......obbbbbbbllrrrrrrro.......",
    ".......obbbbbbbllrrrrrrro.......",
    ".......obbbbbbbllrrrrrrro.......",
    ".......ollllllllllllllllo.......",
    ".......ollllllllllllllllo.......",
    ".......obbbbbbbbbbbbbbbbo.......",
    ".......obbbbbbbbbbbbbbbbo.......",
    ".......obbbbbbbbbbbbbbbbo.......",
    ".......obbbbbbbbbbbbbbbbo.......",
    ".......obbbbbbbbbbbbbbbbo.......",
    ".......obbbbbbbbbbbbbbbbo.......",
    ".......obbbbbbbbbbbbbbbbo.......",
    ".......obbbbbbbbbbbbbbbbo.......",
    ".......obbbbbbbbbbbbbbbbo.......",
    ".......obbbbbbbbbbbbbbbbo.......",
    ".......oobbbbbbbbbbbbbboo.......",
    "........obbbbbbbbbbbbbbo........",
    "........oobbbbbbbbbbbboo........",
    ".........oobbbbbbbbbboo.........",
    "..........ooobbbbbbooo..........",
    "............oooooooo............",
    "................................",
    "................................",
};

extern int g_hints_on;
static int s_bulb_x = -1, s_bulb_y = -1, s_bulb_hover, s_mouse_hover;
#define MOUSE_DX   (EAR_N + 2 * GAP)          /* mouse icon right of the bulb */

static void slash(int x, int y)
{
    uint8_t dark = pal_near(0, 0, 0), red = pal_near(230, 25, 25);
    for (int t = 2; t < EAR_N - 2; ++t)
        for (int k = -2; k <= 2; ++k)
            put(x + t + k, y + EAR_N - 1 - t, (k == -2 || k == 2) ? dark : red);
}

static void paint_mouse(int x, int y)
{
    int on = g_modern_controls;
    uint8_t dark = pal_near(0, 0, 0);
    uint8_t body = s_mouse_hover ? pal_near(235, 235, 235) : pal_near(190, 190, 195);
    uint8_t rbtn = on ? (s_mouse_hover ? pal_near(255, 230, 120) : pal_near(250, 200, 40))
                      : pal_near(140, 140, 140);
    uint8_t seam = pal_near(70, 70, 80);
    for (int j = 0; j < EAR_N; ++j)
        for (int i = 0; i < EAR_N; ++i) {
            char c = s_mouse[j][i];
            uint8_t v = c == 'b' ? body : c == 'r' ? rbtn : c == 'l' ? seam : dark;
            if (c != '.') put(x + i, y + j, v);
        }
    if (!on) slash(x, y);
}

void HintIconBegin(void)
{
    s_bulb_x = s_bulb_y = -1;
    s_bulb_hover = 0;
}

void HintIconPaint(void)
{
    if (!g_back_shadow) return;
    if (s_bulb_x < 0) place_box(MOUSE_DX + EAR_N, EAR_N, &s_bulb_x, &s_bulb_y);
    paint_mouse(s_bulb_x + MOUSE_DX, s_bulb_y);
    int on = g_hints_on, x = s_bulb_x, y = s_bulb_y;
    uint8_t dark  = pal_near(0, 0, 0);
    uint8_t glass = on ? pal_near(255, 225, 80) : pal_near(150, 150, 150);
    if (s_bulb_hover) glass = on ? pal_near(255, 245, 150) : pal_near(200, 200, 200);
    uint8_t fil   = on ? pal_near(200, 80, 20) : pal_near(80, 80, 80);
    uint8_t base  = pal_near(120, 120, 130);
    for (int j = 0; j < EAR_N; ++j)
        for (int i = 0; i < EAR_N; ++i) {
            char c = s_bulb[j][i];
            uint8_t v = c == 'g' ? glass : c == 'f' ? fil : c == 'b' ? base : c == 'o' ? dark : 0;
            if (c != '.') put(x + i, y + j, v);
        }
    if (on) {                          /* rays */
        uint8_t ray = pal_near(255, 240, 140);
        static const signed char r[][4] = { {3,6,5,7}, {29,6,27,7}, {2,13,4,13}, {30,13,28,13}, {16,0,16,1} };
        for (size_t k = 0; k < sizeof r / sizeof r[0]; ++k) {
            int ax = r[k][0], ay = r[k][1], bx = r[k][2], by = r[k][3];
            int n = abs(bx - ax) > abs(by - ay) ? abs(bx - ax) : abs(by - ay);
            for (int t = 0; t <= n; ++t)
                put(x + ax + (n ? (bx - ax) * t / n : 0), y + ay + (n ? (by - ay) * t / n : 0), ray);
        }
    } else {                           /* red slash, dark edged */
        slash(x, y);
    }
}

int HintIconUpdate(void)
{
    if (s_bulb_x < 0) place_box(MOUSE_DX + EAR_N, EAR_N, &s_bulb_x, &s_bulb_y);
    s_bulb_hover = g_mouse_x >= s_bulb_x && g_mouse_x < s_bulb_x + EAR_N &&
                   g_mouse_y >= s_bulb_y && g_mouse_y < s_bulb_y + EAR_N;
    s_mouse_hover = g_mouse_x >= s_bulb_x + MOUSE_DX && g_mouse_x < s_bulb_x + MOUSE_DX + EAR_N &&
                    g_mouse_y >= s_bulb_y && g_mouse_y < s_bulb_y + EAR_N;
    if (s_mouse_hover) {
        if (g_menu_free_click) { g_menu_free_click = 0; ModernControlsToggle(); return 1; }
        SubtitlesHint(g_modern_controls
            ? "Sterowanie wsp\xC3\xB3\xC5\x82""czesne: w\xC5\x82\xC4\x85""czone (w panoramie PPM = panel, Tab / k\xC3\xB3\xC5\x82""ko = posta\xC4\x87)"
            : "Sterowanie wsp\xC3\xB3\xC5\x82""czesne: wy\xC5\x82\xC4\x85""czone (klasyczne)", 150);
        return 1;
    }
    if (!s_bulb_hover) return 0;
    if (g_menu_free_click) {
        g_menu_free_click = 0;
        HintsToggle();                      /* + saved + hint */
        return 1;
    }
    SubtitlesHint(g_hints_on ? "Podpowiedzi: w\xC5\x82\xC4\x85""czone"
                             : "Podpowiedzi: wy\xC5\x82\xC4\x85""czone (domy\xC5\x9Blnie)", 150);
    return 1;
}
