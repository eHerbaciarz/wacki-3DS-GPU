/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/text/subtitles.c — napisy (subtitles) for dialogue, sounds and films,
 * including captions for the deaf and hard of hearing, in any language.
 *
 * The shipped game's dialogue is VOICE ONLY: Gadki.scr names a WAV per
 * line ([sampl]) and has no text. This module adds the text from a plain
 * file the player (or tools/napisy/wacki_napisy.py, which transcribes the
 * WAVs with Whisper) provides:
 *
 *     <data root>/napisy/<lang>.txt      (also ./napisy/<lang>.txt)
 *
 * UTF-8, one entry per line, fields separated by TAB:
 *
 *     @jezyk=pl                 settings ("@key=value")
 *     @mowcy=1                  show the speaker's name before the line
 *     @dzwieki=1                show sound captions ([dzwieki] section)
 *     # comment
 *     [dialogi]
 *     fjgut04a.wav <TAB> Fjej <TAB> Text of the line|second line
 *     [dzwieki]
 *     wybuch.wav <TAB> [wybuch]
 *     [film intro.avi]
 *     00:01.200 <TAB> 00:03.500 <TAB> Text shown in the film
 *
 * "|" forces a line break; long text is wrapped to the screen anyway.
 * Entries with empty text are ignored, so a skeleton file is harmless.
 * Polish letters use the game font's own glyphs; other accented Latin
 * letters fall back to the plain letter (the font has no glyphs for them).
 *
 * The language comes from wacki.cfg (subtitles_lang=pl, default "pl");
 * speaker names / sound captions can be forced there too
 * (subtitles_speakers / subtitles_sounds = 0 or 1, -1 = as the file says).
 * Everything is shown only while the Solund menu's "napisy" option
 * (g_subtitles_on) is on.
 *
 * DRAWING: nothing is painted into the engine's own back buffer (the
 * engine does not redraw every pixel every frame, so text would leave
 * trails). SubtitlesComposeFrame copies the finished frame, draws the
 * text on the copy (game font, dark outline + bright fill picked from the
 * current palette by brightness, so it reads on every scene) and hands
 * the copy to the platform. Dialogue and film text sit at the bottom of
 * the picture (above the HUD panel in gameplay), sound captions at the
 * top. No file / option off → the frame passes through untouched. */

#include "wacki.h"
#include "wacki/log.h"
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUB_MAX_LINES         3     /* lines per caption on screen        */
#define SUB_TEXT_MAX        512     /* bytes of one caption (font bytes)  */
#define SUB_MARGIN_X         16     /* keep text this far from the sides  */
#define SUB_GAP              4      /* px between the text and an edge    */
#define SUB_HUD_TOP        400      /* gameplay picture is 640×400        */
#define SUB_SOUND_MS      2500      /* how long a sound caption stays     */
#define SUB_READ_MS_BASE   900      /* text-only line: reading time       */
#define SUB_READ_MS_CHAR    60
#define SUB_READ_MS_MAX   9000

/* ---- loaded file --------------------------------------------------- */

typedef struct { char *wav; char *speaker; char *text; } SubDialog;
typedef struct { char *wav; char *text; } SubSound;
typedef struct { char *film; uint32_t t0, t1; char *text; } SubCue;

static char      *s_pool;            /* all strings, font bytes          */
static size_t     s_pool_len, s_pool_cap;
static SubDialog *s_dlg;   static int s_dlg_n,   s_dlg_cap;
static SubSound  *s_snd;   static int s_snd_n,   s_snd_cap;
static SubCue    *s_cue;   static int s_cue_n,   s_cue_cap;
static int        s_file_speakers = 0, s_file_sounds = 1;
static int        s_loaded = 0;

/* wacki.cfg (config.c) */
char g_subtitles_lang[16]    = "pl";
int  g_subtitles_speakers    = -1;   /* -1 = file decides */
int  g_subtitles_sounds      = -1;

extern uint8_t     g_subtitles_on;   /* Solund "napisy" (dialog.c) */
extern int         g_cutscene_playing;
extern FontHandle *g_default_font;
extern char        g_data_root[260];

/* ---- UTF-8 → game font bytes --------------------------------------- *
 * Futura.30 carries Polish letters at its own slots (the same table as
 * balloon.c's TextTranslationLutInit, keyed by Unicode here). */
static const struct { uint16_t cp; uint8_t slot; } k_pl_glyphs[] = {
    {0x0104,0xC2},{0x0106,0xCA},{0x0118,0xCB},{0x0141,0xCE},{0x0143,0xCF},
    {0x00D3,0xD3},{0x015A,0xD4},{0x017B,0xDB},{0x0179,0xDA},
    {0x0105,0xE2},{0x0107,0xEA},{0x0119,0xEB},{0x0142,0xEE},{0x0144,0xEF},
    {0x00F3,0xF3},{0x015B,0xF4},{0x017A,0xFA},{0x017C,0xFB},
};

/* Accented Latin letters the font lacks → plain letter (U+00C0..U+017F). */
static char latin_fallback(uint32_t cp)
{
    static const char l1[] =   /* U+00C0..U+00FF */
        "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs"
        "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
    static const char ea[] =   /* U+0100..U+017F */
        "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg"
        "GgGgHhHhIiIiIiIi" "IiJjJjKkkLlLlLlL"
        "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs"
        "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
    if (cp >= 0xC0 && cp <= 0xFF)   return l1[cp - 0xC0];
    if (cp >= 0x100 && cp <= 0x17F) return ea[cp - 0x100];
    return 0;
}

/* Decode one UTF-8 sequence; returns bytes used (≥1). Bad bytes → U+FFFD. */
static int utf8_next(const unsigned char *s, size_t n, uint32_t *cp)
{
    unsigned c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 0;
    if (len == 0 || (size_t)len > n) { *cp = 0xFFFD; return 1; }
    uint32_t v = c & (0xFF >> (len + 1));
    for (int i = 1; i < len; ++i) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return len;
}

/* Append the font-byte form of UTF-8 text [s, s+n) to `out` (cap bytes,
 * always NUL-terminated). Returns bytes written. */
size_t SubtitlesUtf8ToFont(const char *s, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    if (cap == 0) return 0;
    while (n > 0 && o + 4 < cap) {
        uint32_t cp;
        int used = utf8_next(p, n, &cp);
        p += used; n -= (size_t)used;
        const char *rep = NULL;
        char one[2] = {0, 0};
        if (cp == '\t' || cp == '\r' || cp == '\n') cp = ' ';
        if (cp >= 0x20 && cp < 0x7F) { one[0] = (char)cp; rep = one; }
        else if (cp == 0x00A0) { rep = " "; }
        else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x00B4) rep = "'";
        else if (cp == 0x201C || cp == 0x201D || cp == 0x201E ||
                 cp == 0x00AB || cp == 0x00BB) rep = "\"";
        else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) rep = "-";
        else if (cp == 0x2026) rep = "...";
        else if (cp == 0x00DF) rep = "ss";
        else if (cp == 0xFEFF) rep = "";          /* BOM */
        else {
            for (size_t i = 0; i < sizeof k_pl_glyphs / sizeof k_pl_glyphs[0]; ++i)
                if (k_pl_glyphs[i].cp == cp) { one[0] = (char)k_pl_glyphs[i].slot; rep = one; break; }
            if (!rep) {
                char f = latin_fallback(cp);
                one[0] = f ? f : '?';
                rep = one;
            }
        }
        for (; *rep && o + 1 < cap; ++rep) out[o++] = *rep;
    }
    out[o] = 0;
    return o;
}

/* ---- pool / arrays ---------------------------------------------------- */

static char *pool_add(const char *utf8, size_t n, int to_font)
{
    size_t need = n * 3 + 1;                 /* "…" → "..." worst case */
    if (s_pool_len + need > s_pool_cap) {
        /* Strings are stored as offsets until load ends (see fixup). */
        size_t cap = s_pool_cap ? s_pool_cap * 2 : 16384;
        while (cap < s_pool_len + need) cap *= 2;
        char *np = (char *)realloc(s_pool, cap);
        if (!np) return NULL;
        s_pool = np; s_pool_cap = cap;
    }
    char *dst = s_pool + s_pool_len;
    size_t w;
    if (to_font) {
        w = SubtitlesUtf8ToFont(utf8, n, dst, need);
    } else {                                  /* ASCII key: lower-case */
        w = 0;
        for (size_t i = 0; i < n; ++i) {
            char c = utf8[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            dst[w++] = c;
        }
        dst[w] = 0;
    }
    s_pool_len += w + 1;
    return (char *)(uintptr_t)(dst - s_pool + 1);   /* offset+1, 0 = none */
}

/* Make room for one more element (on failure n stays >= cap). */
#define GROW(arr, n, cap) do {                                       \
    if ((n) >= (cap)) {                                              \
        int   nc_ = (cap) ? (cap) * 2 : 64;                          \
        void *na_ = realloc((arr), sizeof *(arr) * (size_t)nc_);     \
        if (na_) { (arr) = na_; (cap) = nc_; }                       \
    }                                                                \
} while (0)

static char *fix(char *off) { return off ? s_pool + ((uintptr_t)off - 1) : NULL; }

/* "12.5", "01:02.300", "1:02:03.5" → ms; returns 0 on success. */
static int parse_time(const char *s, size_t n, uint32_t *out)
{
    double part = 0.0, total = 0.0;
    int    seen = 0;
    double frac = 0.0, scale = 0.1;
    int    in_frac = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == ' ') continue;
        if (c >= '0' && c <= '9') {
            if (in_frac) { frac += (c - '0') * scale; scale /= 10.0; }
            else part = part * 10.0 + (c - '0');
            seen = 1;
        } else if (c == ':') {
            total = (total + part) * 60.0; part = 0.0; in_frac = 0;
        } else if (c == '.' || c == ',') {
            in_frac = 1;
        } else {
            return -1;
        }
    }
    if (!seen) return -1;
    *out = (uint32_t)((total + part + frac) * 1000.0 + 0.5);
    return 0;
}

/* Split a line on TABs into up to `max` fields (trimmed). */
static int split_tabs(const char *s, size_t n, const char **f, size_t *fl, int max)
{
    int k = 0;
    size_t i = 0;
    while (k < max) {
        size_t a = i;
        while (i < n && s[i] != '\t') ++i;
        size_t b = i;
        while (a < b && (s[a] == ' ')) ++a;
        while (b > a && (s[b-1] == ' ')) --b;
        if (k == max - 1) {                  /* last field keeps the rest */
            b = n;
            while (b > a && (s[b-1] == ' ' || s[b-1] == '\t')) --b;
        }
        f[k] = s + a; fl[k] = b - a; ++k;
        if (i >= n || k == max) break;
        ++i;
    }
    return k;
}

static void clear_captions(void);

void SubtitlesUnload(void)
{
    clear_captions();                        /* they point into s_pool */
    free(s_pool); s_pool = NULL; s_pool_len = s_pool_cap = 0;
    free(s_dlg); s_dlg = NULL; s_dlg_n = s_dlg_cap = 0;
    free(s_snd); s_snd = NULL; s_snd_n = s_snd_cap = 0;
    free(s_cue); s_cue = NULL; s_cue_n = s_cue_cap = 0;
    s_loaded = 0;
}

/* Parse a whole file (UTF-8 bytes). Returns entries loaded. Exposed for
 * tests (tests/test_subtitles.c). */
int SubtitlesLoadBuffer(const char *buf, size_t len)
{
    enum { SEC_NONE, SEC_DLG, SEC_SND, SEC_FILM } sec = SEC_NONE;
    char *film = NULL;
    int   lineno = 0, bad = 0;

    SubtitlesUnload();
    s_file_speakers = 0;
    s_file_sounds   = 1;

    size_t i = 0;
    while (i < len) {
        size_t a = i;
        while (i < len && buf[i] != '\n') ++i;
        size_t b = i;
        if (i < len) ++i;
        ++lineno;
        if (b > a && buf[b-1] == '\r') --b;
        if (lineno == 1 && b - a >= 3 && (unsigned char)buf[a] == 0xEF &&
            (unsigned char)buf[a+1] == 0xBB && (unsigned char)buf[a+2] == 0xBF) a += 3;
        while (a < b && buf[a] == ' ') ++a;
        if (a >= b || buf[a] == '#') continue;

        const char *s = buf + a;
        size_t      n = b - a;

        if (s[0] == '@') {
            if (n > 7 && memcmp(s, "@mowcy=", 7) == 0)   s_file_speakers = s[7] == '1';
            if (n > 9 && memcmp(s, "@dzwieki=", 9) == 0) s_file_sounds   = s[9] == '1';
            continue;
        }
        if (s[0] == '[') {
            if (n >= 9 && memcmp(s, "[dialogi]", 9) == 0)       sec = SEC_DLG;
            else if (n >= 9 && memcmp(s, "[dzwieki]", 9) == 0)  sec = SEC_SND;
            else if (n > 7 && memcmp(s, "[film ", 6) == 0 && s[n-1] == ']') {
                sec = SEC_FILM;
                const char *nm = s + 6; size_t nl = n - 7;
                while (nl && *nm == ' ') { ++nm; --nl; }
                while (nl && nm[nl-1] == ' ') --nl;
                film = pool_add(nm, nl, 0);
            } else {
                sec = SEC_NONE;
                ++bad;
            }
            continue;
        }

        const char *f[3]; size_t fl[3];
        if (sec == SEC_DLG) {
            int k = split_tabs(s, n, f, fl, 3);
            if (k < 2) { ++bad; continue; }
            const char *text = k == 3 ? f[2] : f[1];
            size_t      tl   = k == 3 ? fl[2] : fl[1];
            if (k == 2) { fl[1] = 0; }       /* "wav<TAB>text": no speaker */
            if (tl == 0 || fl[0] == 0) continue;
            GROW(s_dlg, s_dlg_n, s_dlg_cap);
            if (s_dlg_n >= s_dlg_cap) break;
            s_dlg[s_dlg_n].wav     = pool_add(f[0], fl[0], 0);
            s_dlg[s_dlg_n].speaker = (k == 3 && fl[1]) ? pool_add(f[1], fl[1], 1) : NULL;
            s_dlg[s_dlg_n].text    = pool_add(text, tl, 1);
            ++s_dlg_n;
        } else if (sec == SEC_SND) {
            int k = split_tabs(s, n, f, fl, 2);
            if (k < 2) { ++bad; continue; }
            if (fl[1] == 0 || fl[0] == 0) continue;
            GROW(s_snd, s_snd_n, s_snd_cap);
            if (s_snd_n >= s_snd_cap) break;
            s_snd[s_snd_n].wav  = pool_add(f[0], fl[0], 0);
            s_snd[s_snd_n].text = pool_add(f[1], fl[1], 1);
            ++s_snd_n;
        } else if (sec == SEC_FILM && film) {
            int k = split_tabs(s, n, f, fl, 3);
            uint32_t t0, t1;
            if (k < 3 || parse_time(f[0], fl[0], &t0) || parse_time(f[1], fl[1], &t1) ||
                t1 <= t0) { ++bad; continue; }
            if (fl[2] == 0) continue;
            GROW(s_cue, s_cue_n, s_cue_cap);
            if (s_cue_n >= s_cue_cap) break;
            s_cue[s_cue_n].film = film;
            s_cue[s_cue_n].t0 = t0; s_cue[s_cue_n].t1 = t1;
            s_cue[s_cue_n].text = pool_add(f[2], fl[2], 1);
            ++s_cue_n;
        } else {
            ++bad;
        }
    }
    /* The pool may have moved while growing: offsets → pointers now. */
    for (int k = 0; k < s_dlg_n; ++k) {
        s_dlg[k].wav = fix(s_dlg[k].wav); s_dlg[k].speaker = fix(s_dlg[k].speaker);
        s_dlg[k].text = fix(s_dlg[k].text);
    }
    for (int k = 0; k < s_snd_n; ++k) { s_snd[k].wav = fix(s_snd[k].wav); s_snd[k].text = fix(s_snd[k].text); }
    for (int k = 0; k < s_cue_n; ++k) { s_cue[k].film = fix(s_cue[k].film); s_cue[k].text = fix(s_cue[k].text); }

    if (bad) LOG_WARN("napisy", "%d line(s) not understood (TABs between fields?)", bad);
    s_loaded = (s_dlg_n + s_snd_n + s_cue_n) > 0;
    return s_dlg_n + s_snd_n + s_cue_n;
}

void SubtitlesInit(void)
{
    char path[400];
    const char *roots[3] = { g_data_root[0] ? g_data_root : NULL, ".", "data" };
    char lang[16];
    strncpy(lang, g_subtitles_lang[0] ? g_subtitles_lang : "pl", sizeof lang - 1);
    lang[sizeof lang - 1] = 0;
    for (char *c = lang; *c; ++c)            /* a file name, nothing else */
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) *c = '_';

    for (int r = 0; r < 3; ++r) {
        if (!roots[r]) continue;
        snprintf(path, sizeof path, "%s/napisy/%s.txt", roots[r], lang);
        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0 || sz > 8 * 1024 * 1024) { fclose(fp); continue; }
        char *buf = (char *)malloc((size_t)sz);
        if (!buf) { fclose(fp); return; }
        size_t got = fread(buf, 1, (size_t)sz, fp);
        fclose(fp);
        int n = SubtitlesLoadBuffer(buf, got);
        free(buf);
        LOG_INFO("napisy", "%s: %d dialogue, %d sound, %d film entries", path,
                 s_dlg_n, s_snd_n, s_cue_n);
        (void)n;
        return;
    }
    LOG_INFO("napisy", "no napisy/%s.txt — subtitles off", lang);
}

/* ---- active captions -------------------------------------------------- */

enum { CAP_DLG = 0, CAP_SND, CAP_FILM, CAP_COUNT };

typedef struct {
    const char *text;           /* font bytes (pool) or NULL         */
    const char *speaker;
    char        shown[SUB_TEXT_MAX]; /* what the image was built from */
    uint8_t    *img;            /* 0 = none, 1 = outline, 2 = text    */
    int         img_w, img_h;
    uint32_t    until_ms;       /* sound captions: hide after         */
} Caption;

static Caption     s_cap[CAP_COUNT];
static const char *s_film;      /* lower-case basename while a film plays */
static uint32_t    s_film_start;

static void clear_captions(void)
{
    for (int k = 0; k < CAP_COUNT; ++k) {
        free(s_cap[k].img);
        memset(&s_cap[k], 0, sizeof s_cap[k]);
    }
    s_film = NULL;
}

static int speakers_on(void)
{
    return g_subtitles_speakers >= 0 ? g_subtitles_speakers : s_file_speakers;
}
static int sounds_on(void)
{
    return g_subtitles_sounds >= 0 ? g_subtitles_sounds : s_file_sounds;
}

static int key_eq(const char *key, const char *name)
{
    for (; *key && *name; ++key, ++name) {
        char c = *name;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (*key != c) return 0;
    }
    return *key == 0 && *name == 0;
}

static const char *basename_of(const char *p)
{
    const char *b = p;
    for (; *p; ++p) if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    return b;
}

/* Length in characters (font bytes) → reading time for a text-only line. */
static uint32_t reading_ms(const char *t)
{
    uint32_t ms = SUB_READ_MS_BASE + (uint32_t)strlen(t) * SUB_READ_MS_CHAR;
    return ms > SUB_READ_MS_MAX ? SUB_READ_MS_MAX : ms;
}

uint32_t SubtitlesDialogLine(const char *wav, char speaker_tag)
{
    (void)speaker_tag;
    s_cap[CAP_DLG].text = NULL;
    if (!s_loaded || !wav || !*wav) return 0;
    const char *b = basename_of(wav);
    for (int i = 0; i < s_dlg_n; ++i) {
        if (key_eq(s_dlg[i].wav, b)) {
            s_cap[CAP_DLG].text    = s_dlg[i].text;
            s_cap[CAP_DLG].speaker = s_dlg[i].speaker;
            return g_subtitles_on ? reading_ms(s_dlg[i].text) : 0;
        }
    }
    LOG_TRACE("napisy", "no text for %s", b);
    return 0;
}

void SubtitlesDialogEnd(void)
{
    s_cap[CAP_DLG].text = NULL;
}

void SubtitlesSound(const char *wav)
{
    if (!s_loaded || !s_snd_n || !wav || !sounds_on()) return;
    const char *b = basename_of(wav);
    for (int i = 0; i < s_snd_n; ++i) {
        if (key_eq(s_snd[i].wav, b)) {
            s_cap[CAP_SND].text     = s_snd[i].text;
            s_cap[CAP_SND].speaker  = NULL;
            s_cap[CAP_SND].until_ms = SDL_GetTicks() + SUB_SOUND_MS;
            return;
        }
    }
}

void SubtitlesFilmBegin(const char *path)
{
    s_film = NULL;
    s_cap[CAP_FILM].text = NULL;
    if (!s_loaded || !s_cue_n || !path) return;
    const char *b = basename_of(path);
    for (int i = 0; i < s_cue_n; ++i)
        if (key_eq(s_cue[i].film, b)) { s_film = s_cue[i].film; break; }
    s_film_start = SDL_GetTicks();
}

void SubtitlesFilmEnd(void)
{
    s_film = NULL;
    s_cap[CAP_FILM].text = NULL;
}

/* ---- drawing ---------------------------------------------------------- */

extern int MeasureTextLine(FontHandle *f, const uint8_t *text);
extern int FontCellHeight(FontHandle *f);

/* Word-wrap `t` into at most SUB_MAX_LINES lines no wider than max_w.
 * Writes NUL-separated lines into `out`, returns the line count. */
static int wrap_text(FontHandle *font, const char *t, int max_w,
                     char *out, size_t cap, char **lines)
{
    int count = 0;
    size_t o = 0;
    char word[SUB_TEXT_MAX], cur[SUB_TEXT_MAX];
    size_t cl = 0;
    cur[0] = 0;

    const char *p = t;
    for (;;) {
        /* next word (or forced break / end) */
        while (*p == ' ') ++p;
        int forced = 0, end = 0;
        size_t wl = 0;
        while (*p && *p != ' ' && *p != '|' && wl + 1 < sizeof word) word[wl++] = *p++;
        word[wl] = 0;
        if (*p == '|') { forced = 1; ++p; }
        if (!*p && !forced) end = 1;

        if (wl) {
            char trial[SUB_TEXT_MAX * 2 + 2];
            int  n = snprintf(trial, sizeof trial, "%s%s%s", cur, cl ? " " : "", word);
            if (n < 0) n = 0;
            if (cl && MeasureTextLine(font, (const uint8_t *)trial) > max_w) {
                /* flush cur, start a new line with the word */
                if (count == SUB_MAX_LINES) break;
                if (o + cl + 1 > cap) break;
                lines[count++] = out + o;
                memcpy(out + o, cur, cl + 1); o += cl + 1;
                strncpy(cur, word, sizeof cur - 1); cur[sizeof cur - 1] = 0;
                cl = strlen(cur);
            } else {
                strncpy(cur, trial, sizeof cur - 1); cur[sizeof cur - 1] = 0;
                cl = strlen(cur);
            }
        }
        if ((forced || end) && cl) {
            if (count == SUB_MAX_LINES || o + cl + 1 > cap) break;
            lines[count++] = out + o;
            memcpy(out + o, cur, cl + 1); o += cl + 1;
            cur[0] = 0; cl = 0;
        }
        if (end) break;
    }
    if (cl && count < SUB_MAX_LINES && o + cl + 1 <= cap) {
        lines[count++] = out + o;
        memcpy(out + o, cur, cl + 1);
    }
    return count;
}

/* Build the caption's 0/1/2 image (outline/text) for its current text. */
static void build_image(Caption *c, int frame_w)
{
    char full[SUB_TEXT_MAX];
    if (c->speaker && speakers_on())
        snprintf(full, sizeof full, "%s: %s", c->speaker, c->text);
    else
        snprintf(full, sizeof full, "%s", c->text);
    if (c->img && strcmp(full, c->shown) == 0) return;

    free(c->img); c->img = NULL;
    strncpy(c->shown, full, sizeof c->shown - 1);
    c->shown[sizeof c->shown - 1] = 0;

    FontHandle *font = g_default_font;
    if (!font) return;
    int cell = FontCellHeight(font);
    if (cell <= 0) return;

    char  buf[SUB_TEXT_MAX * 2];
    char *lines[SUB_MAX_LINES];
    int   n = wrap_text(font, full, frame_w - 2 * SUB_MARGIN_X, buf, sizeof buf, lines);
    if (n == 0) return;

    int w = frame_w, h = n * cell + 2;
    uint8_t *mask = (uint8_t *)calloc((size_t)w * h, 1);
    uint8_t *img  = (uint8_t *)calloc((size_t)w * h, 1);
    if (!mask || !img) { free(mask); free(img); return; }

    for (int i = 0; i < n; ++i) {
        int lw = MeasureTextLine(font, (const uint8_t *)lines[i]);
        int x  = (w - lw) / 2;
        if (x < 1) x = 1;
        TextRenderTarget td = {
            .stride     = (uint16_t)w,
            .x          = (uint16_t)x,
            .color_base = 1,
            .pixels     = mask + (size_t)(1 + i * cell) * w,
            .font       = font,
        };
        RenderTextLineToBuffer(&td, (const uint8_t *)lines[i]);
    }
    /* text = 2; every empty pixel touching text = 1 (outline). */
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (mask[(size_t)y * w + x]) { img[(size_t)y * w + x] = 2; continue; }
            int edge = 0;
            for (int dy = -1; dy <= 1 && !edge; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int yy = y + dy, xx = x + dx;
                    if (yy < 0 || yy >= h || xx < 0 || xx >= w) continue;
                    if (mask[(size_t)yy * w + xx]) { edge = 1; break; }
                }
            if (edge) img[(size_t)y * w + x] = 1;
        }
    free(mask);
    c->img = img; c->img_w = w; c->img_h = h;
}

static void pick_colours(const uint8_t *pal, uint8_t *dark, uint8_t *bright)
{
    int lo = 1 << 30, hi = -1;
    *dark = 0; *bright = 255;
    for (int i = 0; i < 256; ++i) {
        const uint8_t *e = pal + i * 3;
        /* luma ×1000; ties prefer the first (stable across frames) */
        int y = e[0] * 299 + e[1] * 587 + e[2] * 114;
        if (y < lo) { lo = y; *dark = (uint8_t)i; }
        if (y > hi) { hi = y; *bright = (uint8_t)i; }
    }
}

static void blit_caption(uint8_t *frame, int w, int h, const Caption *c,
                         int top, uint8_t dark, uint8_t bright)
{
    if (!c->img) return;
    int y0 = top;
    if (y0 < 0) y0 = 0;
    for (int y = 0; y < c->img_h && y0 + y < h; ++y) {
        const uint8_t *src = c->img + (size_t)y * c->img_w;
        uint8_t       *dst = frame + (size_t)(y0 + y) * w;
        int cw = c->img_w < w ? c->img_w : w;
        for (int x = 0; x < cw; ++x) {
            if (src[x] == 2)      dst[x] = bright;
            else if (src[x] == 1) dst[x] = dark;
        }
    }
}

static uint8_t *s_out;
static size_t   s_out_sz;

const uint8_t *SubtitlesComposeFrame(const uint8_t *shadow, const uint8_t *pal,
                                     int w, int h)
{
    if (!s_loaded || !g_subtitles_on || !shadow || !pal || w <= 0 || h <= 0)
        return shadow;

    /* film cue for "now" */
    s_cap[CAP_FILM].text = NULL;
    if (s_film && g_cutscene_playing) {
        uint32_t t = SDL_GetTicks() - s_film_start;
        for (int i = 0; i < s_cue_n; ++i)
            if (s_cue[i].film == s_film && t >= s_cue[i].t0 && t < s_cue[i].t1) {
                s_cap[CAP_FILM].text = s_cue[i].text;
                s_cap[CAP_FILM].speaker = NULL;
                break;
            }
    }
    if (s_cap[CAP_SND].text && (int32_t)(SDL_GetTicks() - s_cap[CAP_SND].until_ms) >= 0)
        s_cap[CAP_SND].text = NULL;

    int any = 0;
    for (int k = 0; k < CAP_COUNT; ++k) if (s_cap[k].text) any = 1;
    if (!any) return shadow;

    size_t need = (size_t)w * h;
    if (s_out_sz < need) {
        uint8_t *nb = (uint8_t *)realloc(s_out, need);
        if (!nb) return shadow;
        s_out = nb; s_out_sz = need;
    }
    memcpy(s_out, shadow, need);

    uint8_t dark, bright;
    pick_colours(pal, &dark, &bright);
    int bottom = (!g_cutscene_playing && h > SUB_HUD_TOP) ? SUB_HUD_TOP : h;

    /* bottom: film text wins over dialogue during a film */
    Caption *low = s_cap[CAP_FILM].text ? &s_cap[CAP_FILM]
                 : s_cap[CAP_DLG].text  ? &s_cap[CAP_DLG] : NULL;
    if (low) {
        build_image(low, w);
        if (low->img) blit_caption(s_out, w, h, low, bottom - SUB_GAP - low->img_h, dark, bright);
    }
    if (s_cap[CAP_SND].text) {
        build_image(&s_cap[CAP_SND], w);
        blit_caption(s_out, w, h, &s_cap[CAP_SND], SUB_GAP, dark, bright);
    }
    return s_out;
}
