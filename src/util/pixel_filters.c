/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 * Filters from ReClaw's optional/PixelFilters.cpp — Copyright (C) 2026
 * Herbaciarz, GPL-3.0 — ported to C. The game's frame is opaque, so the
 * alpha-weighted mixing of the original is a plain average here.
 *
 * src/util/pixel_filters.c — pixel-art 2× upscalers on 0x00RRGGBB pixels:
 *   PIXF_SCALE2X — EPX / Scale2x: sharp; straight and 45° edges smoothed
 *   PIXF_SAI2X   — 2xSaI (Kreed): smooth, blends edges
 *   PIXF_XBR2X   — xBR (Hyllian, level 1): edges of any slope
 * out = (2w)×(2h) pixels. Used by the 3DS bottom-screen zoom
 * (video_3ds_c3d.c): 2× = one pass, 4× = two. */

#include "wacki.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct { const uint32_t *px; int w, h; } Src;

static inline uint32_t at(const Src *s, int x, int y)
{
    x = x < 0 ? 0 : (x >= s->w ? s->w - 1 : x);
    y = y < 0 ? 0 : (y >= s->h ? s->h - 1 : y);
    return s->px[(size_t)y * s->w + x];
}

#define R_(c) ((int)(((c) >> 16) & 0xFF))
#define G_(c) ((int)(((c) >> 8) & 0xFF))
#define B_(c) ((int)((c) & 0xFF))

static inline uint32_t mix2(uint32_t a, uint32_t b)
{
    if (a == b) return a;
    /* per-channel average without overflow */
    return ((a & 0xFEFEFEu) >> 1) + ((b & 0xFEFEFEu) >> 1) + (a & b & 0x010101u);
}

static inline uint32_t mix4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    unsigned r = (unsigned)(R_(a) + R_(b) + R_(c) + R_(d)) / 4;
    unsigned g = (unsigned)(G_(a) + G_(b) + G_(c) + G_(d)) / 4;
    unsigned bl = (unsigned)(B_(a) + B_(b) + B_(c) + B_(d)) / 4;
    return (r << 16) | (g << 8) | bl;
}

static inline void put(uint32_t *out, int w2, int x, int y,
                       uint32_t e0, uint32_t e1, uint32_t e2, uint32_t e3)
{
    uint32_t *row = out + (size_t)(2 * y) * w2 + 2 * x;
    row[0] = e0; row[1] = e1;
    row[w2] = e2; row[w2 + 1] = e3;
}

static void do_scale2x(const Src *s, uint32_t *out, int y0, int y1)
{
    const int w2 = s->w * 2;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < s->w; ++x) {
            uint32_t b = at(s, x, y - 1), d = at(s, x - 1, y), e = at(s, x, y);
            uint32_t f = at(s, x + 1, y), h = at(s, x, y + 1);
            if (b != h && d != f)
                put(out, w2, x, y, d == b ? d : e, b == f ? f : e,
                                   d == h ? d : e, h == f ? f : e);
            else
                put(out, w2, x, y, e, e, e, e);
        }
}

static int result1(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    int x = 0, y = 0, r = 0;
    if (a == c) ++x; else if (b == c) ++y;
    if (a == d) ++x; else if (b == d) ++y;
    if (x <= 1) r += 1;
    if (y <= 1) r -= 1;
    return r;
}

static int result2(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    int x = 0, y = 0, r = 0;
    if (a == c) ++x; else if (b == c) ++y;
    if (a == d) ++x; else if (b == d) ++y;
    if (x <= 1) r -= 1;
    if (y <= 1) r += 1;
    return r;
}

static void do_sai2x(const Src *s, uint32_t *out, int y0, int y1)
{
    const int w2 = s->w * 2;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < s->w; ++x) {
            /*  I E F J
             *  G A B K
             *  H C D L
             *  M N O P */
            uint32_t I = at(s, x-1, y-1), E = at(s, x, y-1), F = at(s, x+1, y-1), J = at(s, x+2, y-1);
            uint32_t G = at(s, x-1, y),   A = at(s, x, y),   B = at(s, x+1, y),   K = at(s, x+2, y);
            uint32_t H = at(s, x-1, y+1), C = at(s, x, y+1), D = at(s, x+1, y+1), L = at(s, x+2, y+1);
            uint32_t M = at(s, x-1, y+2), N = at(s, x, y+2), O = at(s, x+1, y+2);
            uint32_t right, below, diag;
            if (A == D && B != C) {
                right = ((A == E && B == L) || (A == C && A == F && B != E && B == J)) ? A : mix2(A, B);
                below = ((A == G && C == O) || (A == B && A == H && G != C && C == M)) ? A : mix2(A, C);
                diag = A;
            } else if (B == C && A != D) {
                right = ((B == F && A == H) || (B == E && B == D && A != F && A == I)) ? B : mix2(A, B);
                below = ((C == H && A == F) || (C == G && C == D && A != H && A == I)) ? C : mix2(A, C);
                diag = B;
            } else if (A == D && B == C) {
                if (A == B) {
                    right = below = diag = A;
                } else {
                    below = mix2(A, C);
                    right = mix2(A, B);
                    int r = 0;
                    r += result1(A, B, G, E);
                    r += result2(B, A, K, F);
                    r += result2(B, A, H, N);
                    r += result1(A, B, L, O);
                    diag = r > 0 ? A : (r < 0 ? B : mix4(A, B, C, D));
                }
            } else {
                diag = mix4(A, B, C, D);
                if (A == C && A == F && B != E && B == J) right = A;
                else if (B == E && B == D && A != F && A == I) right = B;
                else right = mix2(A, B);
                if (A == B && A == H && G != C && C == M) below = A;
                else if (C == G && C == D && A != H && A == I) below = C;
                else below = mix2(A, C);
            }
            put(out, w2, x, y, A, right, below, diag);
        }
}

/* xBR's colour distance in YUV: 48·|ΔY| + 7·|ΔU| + 6·|ΔV|. Y/U/V are
 * computed once per source pixel (linear in RGB, so |Y(a)-Y(b)| is the
 * original per-pair formula), packed in one int: Y 10 bits | U 11 | V 11. */
static int32_t *s_yuv;
static size_t   s_yuv_cap;

static inline int32_t yuv_of(uint32_t c)
{
    int r = R_(c), g = G_(c), b = B_(c);
    int y = (r * 299 + g * 587 + b * 114) / 1000;          /* 0..255  */
    int u = (-r * 169 - g * 331 + b * 500) / 1000 + 128;   /* 0..255  */
    int v = (r * 500 - g * 419 - b * 81) / 1000 + 128;
    return (y << 20) | (u << 10) | v;
}

static inline int ydist(int32_t a, int32_t b)
{
    if (a == b) return 0;
    int dy = ((a >> 20) & 0x3FF) - ((b >> 20) & 0x3FF);
    int du = ((a >> 10) & 0x3FF) - ((b >> 10) & 0x3FF);
    int dv = (a & 0x3FF) - (b & 0x3FF);
    return 48 * (dy < 0 ? -dy : dy) + 7 * (du < 0 ? -du : du) + 6 * (dv < 0 ? -dv : dv);
}

/* YUV of every source pixel, once per frame (shared by the row bands). */
static int xbr_prepare(const Src *s)
{
    size_t n = (size_t)s->w * s->h;
    if (s_yuv_cap < n) {
        int32_t *ny = (int32_t *)realloc(s_yuv, n * sizeof *ny);
        if (!ny) return 0;
        s_yuv = ny; s_yuv_cap = n;
    }
    for (size_t i = 0; i < n; ++i) s_yuv[i] = yuv_of(s->px[i]);
    return 1;
}

static void do_xbr2x(const Src *s, uint32_t *out, int y0, int y1)
{
    const int w = s->w, h = s->h, w2 = w * 2;
    if (!s_yuv || s_yuv_cap < (size_t)w * h) { do_scale2x(s, out, y0, y1); return; }

    /* Neighbour offsets for the bottom-right corner; the other three
     * corners are the same turned by 90° ((dx,dy) → (dy,-dx)). Order:
     * H F I C G F4 H5 I4 I5 D B */
    static const int base[11][2] = {
        {0,1},{1,0},{1,1},{1,-1},{-1,1},{2,0},{0,2},{2,1},{1,2},{-1,0},{0,-1}
    };
    int off[4][11][2];
    for (int k = 0; k < 4; ++k)
        for (int m = 0; m < 11; ++m) {
            int dx = base[m][0], dy = base[m][1];
            for (int t = 0; t < k; ++t) { int nx = dy, ny = -dx; dx = nx; dy = ny; }
            off[k][m][0] = dx; off[k][m][1] = dy;
        }

    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t ie = (size_t)y * w + x;
            const uint32_t E = s->px[ie];
            uint32_t corner[4] = { E, E, E, E };   /* bottom-right, top-right, top-left, bottom-left */
            int inner = x >= 2 && y >= 2 && x < w - 2 && y < h - 2;
            for (int k = 0; k < 4; ++k) {
                int (*o)[2] = off[k];
                size_t idx[11];
                for (int m = 0; m < 11; ++m) {
                    int xx = x + o[m][0], yy = y + o[m][1];
                    if (!inner) {
                        xx = xx < 0 ? 0 : (xx >= w ? w - 1 : xx);
                        yy = yy < 0 ? 0 : (yy >= h ? h - 1 : yy);
                    }
                    idx[m] = (size_t)yy * w + xx;
                }
                uint32_t H = s->px[idx[0]], F = s->px[idx[1]];
                if (E == H || E == F) continue;
                const int32_t *Y = s_yuv;
                int32_t yE = Y[ie], yH = Y[idx[0]], yF = Y[idx[1]], yI = Y[idx[2]];
                int e = ydist(yE, Y[idx[3]]) + ydist(yE, Y[idx[4]]) + ydist(yI, Y[idx[5]])
                      + ydist(yI, Y[idx[6]]) + 4 * ydist(yH, yF);
                int i = ydist(yH, Y[idx[9]]) + ydist(yH, Y[idx[8]]) + ydist(yF, Y[idx[7]])
                      + ydist(yF, Y[idx[10]]) + 4 * ydist(yE, yI);
                if (e < i) {
                    uint32_t px = ydist(yE, yF) <= ydist(yE, yH) ? F : H;
                    corner[k] = mix2(E, px);
                }
            }
            put(out, w2, x, y, corner[2], corner[1], corner[3], corner[0]);
        }
}

/* Per-frame setup (xBR: the YUV table). Call before the row bands. */
void PixelUpscale2xPrepare(int kind, const uint32_t *src, int w, int h)
{
    Src s = { src, w, h };
    if (kind == PIXF_XBR2X && !xbr_prepare(&s)) { free(s_yuv); s_yuv = NULL; s_yuv_cap = 0; }
}

/* Source rows y0..y1-1 → output rows 2*y0..2*y1-1. Bands can run in
 * parallel threads after PixelUpscale2xPrepare(). */
void PixelUpscale2xRows(int kind, const uint32_t *src, int w, int h,
                        uint32_t *out, int y0, int y1)
{
    Src s = { src, w, h };
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    switch (kind) {
    case PIXF_SAI2X: do_sai2x(&s, out, y0, y1);   break;
    case PIXF_XBR2X: do_xbr2x(&s, out, y0, y1);   break;
    default:         do_scale2x(&s, out, y0, y1); break;
    }
}

void PixelUpscale2x(int kind, const uint32_t *src, int w, int h, uint32_t *out)
{
    PixelUpscale2xPrepare(kind, src, w, h);
    PixelUpscale2xRows(kind, src, w, h, out, 0, h);
}
