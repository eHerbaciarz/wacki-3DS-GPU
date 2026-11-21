/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/text/hints.c — optional in-game hints (podpowiedzi).
 *
 * Off by default; switched on by the light bulb on the main Options screen
 * (menu/subs_icons.c), saved in wacki.cfg as hints=0|1. Asked for with H on
 * the PC keyboard or a pad combo (see docs/podpowiedzi.md).
 *
 * File: podpowiedzi/<lang>.txt (falls back to pl) next to the game data,
 * like the subtitles. UTF-8, '#' = comment:
 *
 *   [etap 1]
 *   = Kiosk                      a topic (shown in the hint)
 *   @ kiosk, ulica               when the room's name or background .pic
 *                                contains one of these (any case)
 *   @ *                          ... or: the stage's general topic
 *   1<TAB>first, gentle nudge
 *   2<TAB>more concrete
 *   3<TAB>the solution
 *
 * Each press shows the next level of the topic for the room you are in —
 * but only 3 minutes (hint_wait) after the previous one. Before that it
 * shows the current level again with "kolejna za m:ss". The first level
 * comes at once. Progress is kept per topic for the session.
 */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/api.h"

#include <SDL.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOPICS   160
#define MAX_LEVELS   6
#define MAX_KEYS     8
#define SHOW_MS      10000

int g_hints_on   = 0;           /* wacki.cfg hints=        */
int g_hint_wait  = 180;         /* wacki.cfg hint_wait= (s) */

extern char g_subtitles_lang[16];
extern char g_data_root[260];
extern const DemoScene *g_current_scene;

typedef struct {
    int         etap;
    const char *title;
    const char *keys[MAX_KEYS];
    int         nkeys;
    int         general;        /* "@ *" */
    const char *level[MAX_LEVELS];
    int         nlevels;
    int         shown;          /* levels revealed this session */
    uint32_t    last_ms;
} Topic;

static Topic  s_topic[MAX_TOPICS];
static int    s_ntopics;
static char  *s_text;           /* the file, cut into strings in place */
static int    s_loaded_once;

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') ++s;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

void HintsUnload(void)
{
    free(s_text);
    s_text = NULL;
    s_ntopics = 0;
}

/* Parse a whole file (UTF-8, modified in place, kept). Returns topics. */
int HintsLoadBuffer(const char *utf8, size_t len)
{
    HintsUnload();
    s_text = (char *)malloc(len + 1);
    if (!s_text) return 0;
    memcpy(s_text, utf8, len);
    s_text[len] = 0;

    int etap = 0;
    Topic *t = NULL;
    char *p = s_text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *line = trim(p);
        p = nl ? nl + 1 : NULL;

        if (!*line || *line == '#') continue;
        if (*line == '[') {
            int n = 0;
            etap = (sscanf(line, "[etap %d]", &n) == 1 || sscanf(line, "[etap%d]", &n) == 1) ? n : 0;
            t = NULL;
            continue;
        }
        if (*line == '=') {
            if (s_ntopics >= MAX_TOPICS || etap <= 0) { t = NULL; continue; }
            t = &s_topic[s_ntopics++];
            memset(t, 0, sizeof *t);
            t->etap  = etap;
            t->title = trim(line + 1);
            continue;
        }
        if (!t) continue;
        if (*line == '@') {
            char *k = line + 1;
            while (k && *k) {
                char *comma = strchr(k, ',');
                if (comma) *comma = 0;
                char *key = trim(k);
                if (!strcmp(key, "*")) t->general = 1;
                else if (*key && t->nkeys < MAX_KEYS) {
                    for (char *c = key; *c; ++c) *c = (char)tolower((unsigned char)*c);
                    t->keys[t->nkeys++] = key;
                }
                k = comma ? comma + 1 : NULL;
            }
            continue;
        }
        if (line[0] >= '1' && line[0] <= '9') {
            char *txt = line + 1;
            while (*txt >= '0' && *txt <= '9') ++txt;
            if ((*txt == '\t' || *txt == ' ' || *txt == '.' || *txt == ')') && t->nlevels < MAX_LEVELS)
                t->level[t->nlevels++] = trim(txt + 1);
        }
    }
    /* topics without any level are useless */
    int w = 0;
    for (int i = 0; i < s_ntopics; ++i)
        if (s_topic[i].nlevels) s_topic[w++] = s_topic[i];
    s_ntopics = w;
    return s_ntopics;
}

static int try_load(const char *lang)
{
    const char *roots[3] = { g_data_root[0] ? g_data_root : NULL, ".", "data" };
    char path[400];
    for (int r = 0; r < 3; ++r) {
        if (!roots[r]) continue;
        snprintf(path, sizeof path, "%s/podpowiedzi/%s.txt", roots[r], lang);
        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(fp); continue; }
        char *buf = (char *)malloc((size_t)sz);
        if (!buf) { fclose(fp); return 0; }
        size_t got = fread(buf, 1, (size_t)sz, fp);
        fclose(fp);
        int n = HintsLoadBuffer(buf, got);
        free(buf);
        LOG_INFO("hints", "%s: %d topics", path, n);
        return n > 0;
    }
    return 0;
}

void HintsInit(void)
{
    char lang[16];
    snprintf(lang, sizeof lang, "%s", g_subtitles_lang[0] ? g_subtitles_lang : "pl");
    for (char *c = lang; *c; ++c)
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') *c = '_';
    s_loaded_once = 1;
    if (try_load(lang)) return;
    if (strcmp(lang, "pl") && try_load("pl")) return;
    HintsUnload();
    LOG_INFO("hints", "no podpowiedzi/%s.txt", lang);
}

int HintsCount(void) { return s_ntopics; }

static int contains(const char *hay, const char *key)
{
    if (!hay || !*hay) return 0;
    size_t kl = strlen(key);
    for (const char *h = hay; *h; ++h) {
        size_t i = 0;
        while (i < kl && h[i] && tolower((unsigned char)h[i]) == key[i]) ++i;
        if (i == kl) return 1;
    }
    return 0;
}

/* The topic for this stage + room (names may be NULL). */
int HintsFindTopic(int etap, const char *room, const char *pic)
{
    int general = -1, first = -1;
    for (int i = 0; i < s_ntopics; ++i) {
        Topic *t = &s_topic[i];
        if (t->etap != etap) continue;
        if (first < 0) first = i;
        if (t->general && general < 0) general = i;
        for (int k = 0; k < t->nkeys; ++k)
            if (contains(room, t->keys[k]) || contains(pic, t->keys[k])) return i;
    }
    return general >= 0 ? general : first;
}

/* Build the text for one press; advances the topic when allowed.
 * Exposed for tests. */
void HintsPress(int topic, uint32_t now, char *out, size_t cap)
{
    if (topic < 0 || topic >= s_ntopics) {
        snprintf(out, cap, "Brak podpowiedzi dla tego miejsca.");
        return;
    }
    Topic *t = &s_topic[topic];
    uint32_t wait = (uint32_t)(g_hint_wait > 0 ? g_hint_wait : 0) * 1000u;
    if (t->shown == 0 || (t->shown < t->nlevels && now - t->last_ms >= wait)) {
        t->shown++;
        t->last_ms = now;
    }
    int lv = t->shown - 1;
    char tail[64] = "";
    if (t->shown >= t->nlevels) {
        if (t->nlevels > 1) snprintf(tail, sizeof tail, "|(to ju\xC5\xBC rozwi\xC4\x85zanie)");
    } else {
        uint32_t left = (now - t->last_ms >= wait) ? 0 : (wait - (now - t->last_ms) + 999) / 1000;
        if (left) snprintf(tail, sizeof tail, "|(kolejna za %u:%02u)", left / 60, left % 60);
    }
    snprintf(out, cap, "Podpowied\xC5\xBA %d/%d (%s): %s%s",
             lv + 1, t->nlevels, t->title, t->level[lv], tail);
}

void HintsReset(void)
{
    for (int i = 0; i < s_ntopics; ++i) { s_topic[i].shown = 0; s_topic[i].last_ms = 0; }
}

/* The key / pad combo. */
void HintsRequest(void)
{
    if (!g_hints_on) {
        SubtitlesHint("Podpowiedzi s\xC4\x85 wy\xC5\x82\xC4\x85""czone \xE2\x80\x93 "
                      "w\xC5\x82\xC4\x85""cz je \xC5\xBC""ar\xC3\xB3wk\xC4\x85 w Opcjach", 3000);
        return;
    }
    if (!s_loaded_once) HintsInit();
    if (!s_ntopics) {
        SubtitlesHint("Brak pliku podpowiedzi (podpowiedzi/pl.txt)", 3000);
        return;
    }
    const char *room = g_current_scene ? g_current_scene->name   : NULL;
    const char *pic  = g_current_scene ? g_current_scene->bg_pic : NULL;
    int topic = HintsFindTopic((int)g_cur_etap, room, pic);
    LOG_INFO("hints", "etap %u, komnata '%s', tlo '%s' -> topic %d", (unsigned)g_cur_etap,
             room ? room : "", pic ? pic : "", topic);
    char msg[512];
    HintsPress(topic, SDL_GetTicks(), msg, sizeof msg);
    SubtitlesHint(msg, SHOW_MS);
}

/* Light bulb in Options. */
void HintsToggle(void)
{
    extern void ConfigSave(void);
    g_hints_on = !g_hints_on;
    if (g_hints_on && !s_loaded_once) HintsInit();
    SubtitlesHint(g_hints_on ? "Podpowiedzi: w\xC5\x82\xC4\x85""czone (klawisz H / kombinacja na padzie)"
                             : "Podpowiedzi: wy\xC5\x82\xC4\x85""czone", 2500);
    ConfigSave();
}
