/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/config.c — wacki.cfg: persisted player display/input preferences.
 *
 * Dodane klucze:
 *   aspect_mode=wide|wide-bars|4:3 — panorama (przycięta / z paskami) albo 4:3
 *   touch_mode=absolute|relative|off — tryb ekranu dotykowego
 *   subtitles_lang=pl            — plik napisy/<lang>.txt (src/text/subtitles.c)
 *   subtitles_speakers=-1|0|1    — imiona mówców (-1 = jak w pliku)
 *   subtitles_sounds=-1|0|1      — opisy dźwięków dla niesłyszących
 *   color_filter=-1..3           — filtr kolorów (util/color_filter.c)
 *   zoom_filter=-1..4            — filtr zoomu dolnego ekranu 3DS
 *   smoothing=-1|0|1             — PC: wygładzanie obrazu (F3)
 *   wide_top=-1|0|1              — 3DS: górny ekran 800 px (SELECT+START)
 *
 * g_aspect_mode i g_touch_mode są zdefiniowane tutaj (nie w video_sdl.c /
 * platform_sdl.c) aby były dostępne dla WSZYSTKICH targetów przez linkowanie
 * config.c — zapobiega undefined reference na PS2 i innych platformach. */

#include "wacki.h"
#include "wacki/log.h"
#include <stdio.h>
#include <string.h>

#define WACKI_CFG_PATH "wacki.cfg"
#define CFG_SCALE_MAX  8

extern int  g_scale_factor;
extern int  g_fullscreen;
extern char g_subtitles_lang[16];   /* text/subtitles.c */
extern int  g_subtitles_speakers;
extern int  g_subtitles_sounds;
extern int  g_hints_on, g_hint_wait;
extern int  g_modern_controls;
int         g_cfg_diag = 0;          /* wacki.cfg diagnostyka= (Shift+F10) */
int         g_pixel_filter = 0;      /* F6 pixel-art filter (SDL backends) */
int         g_zoom_filter = -1;     /* 3DS bottom-screen zoom; -1 = default */
int         g_smoothing   = -1;     /* PC smoothing: -1 = --scale-mode / default */
int         g_wide_top    = -1;     /* 3DS 800-px top screen: -1 = default (on) */

/* Zdefiniowane tutaj — dostępne dla wszystkich targetów. */
char g_aspect_mode[16] = "wide";   /* wide | wide-bars | 4:3 */
char g_touch_mode[16]  = "absolute";

int g_config_first_run = 0;

void ConfigSave(void);

static void normalize_aspect(const char *raw)
{
    if (!raw) return;
    if (strncmp(raw,"4:3",3)==0 || strncmp(raw,"4x3",3)==0 || strncmp(raw,"43",2)==0)
        strncpy(g_aspect_mode, "4:3", 15);
    else if (strncmp(raw,"wide-bars",9)==0 || strncmp(raw,"bars",4)==0)
        strncpy(g_aspect_mode, "wide-bars", 15);
    else if (strncmp(raw,"stretch",7)==0 || strncmp(raw,"wide",4)==0 ||
             strncmp(raw,"16:9",4)==0)
        strncpy(g_aspect_mode, "wide", 15);   /* old "stretch" → panorama */
    g_aspect_mode[15] = '\0';
}

static void normalize_touch(const char *raw)
{
    if (!raw) return;
    if      (strncmp(raw,"absolute",8)==0) strncpy(g_touch_mode,"absolute",15);
    else if (strncmp(raw,"relative",8)==0) strncpy(g_touch_mode,"relative",15);
    else if (strncmp(raw,"off",3)==0)      strncpy(g_touch_mode,"off",15);
    g_touch_mode[15] = '\0';
}

void ConfigLoad(void)
{
    FILE *fp = fopen(WACKI_CFG_PATH, "r");
    if (!fp) {
        g_config_first_run = 1;
        ConfigSave();
        return;
    }
    char line[128], sv[16];
    int v;
    while (fgets(line, sizeof line, fp)) {
        if      (sscanf(line,"fullscreen=%d",&v)==1) g_fullscreen = v?1:0;
        else if (sscanf(line,"scale=%d",&v)==1 && v>=1 && v<=CFG_SCALE_MAX)
            g_scale_factor = v;
        else if (sscanf(line,"aspect_mode=%15s",sv)==1) normalize_aspect(sv);
        else if (sscanf(line,"touch_mode=%15s",sv)==1)  normalize_touch(sv);
        else if (sscanf(line,"subtitles_lang=%15s",sv)==1) {
            strncpy(g_subtitles_lang, sv, 15);
            g_subtitles_lang[15] = '\0';
        }
        else if (sscanf(line,"subtitles_speakers=%d",&v)==1)
            g_subtitles_speakers = v < 0 ? -1 : (v ? 1 : 0);
        else if (sscanf(line,"subtitles_sounds=%d",&v)==1)
            g_subtitles_sounds = v < 0 ? -1 : (v ? 1 : 0);
        else if (sscanf(line,"diagnostyka=%d",&v)==1)
            { g_cfg_diag = v ? 1 : 0; if (g_cfg_diag) g_log_diag = 1; }
        else if (sscanf(line,"modern_controls=%d",&v)==1)
            g_modern_controls = v ? 1 : 0;
        else if (sscanf(line,"pixel_filter=%d",&v)==1)
            g_pixel_filter = (v >= 0 && v < 4) ? v : 0;
        else if (sscanf(line,"hint_wait=%d",&v)==1)
            g_hint_wait = v < 0 ? 0 : v;
        else if (sscanf(line,"hints=%d",&v)==1)
            g_hints_on = v ? 1 : 0;
        else if (sscanf(line,"color_filter=%d",&v)==1)
            g_color_filter = (v >= 0 && v < COLOR_FILTER_COUNT) ? v : -1;
        else if (sscanf(line,"zoom_filter=%d",&v)==1)
            g_zoom_filter = (v >= 0 && v < 5) ? v : -1;
        else if (sscanf(line,"smoothing=%d",&v)==1)
            g_smoothing = v < 0 ? -1 : (v ? 1 : 0);
        else if (sscanf(line,"wide_top=%d",&v)==1)
            g_wide_top = v < 0 ? -1 : (v ? 1 : 0);
    }
    fclose(fp);
    LOG_INFO("config","loaded: fullscreen=%d scale=%d aspect=%s touch=%s",
             g_fullscreen,g_scale_factor,g_aspect_mode,g_touch_mode);
}

void ConfigSave(void)
{
    FILE *fp = fopen(WACKI_CFG_PATH, "w");
    if (!fp) { LOG_INFO("config","cannot write %s",WACKI_CFG_PATH); return; }
    fprintf(fp,"# Wacki display/input preferences\n");
    fprintf(fp,"fullscreen=%d\n", g_fullscreen?1:0);
    fprintf(fp,"scale=%d\n",      g_scale_factor>0?g_scale_factor:1);
    fprintf(fp,"# aspect_mode: wide (panorama, przycieta) | wide-bars (panorama z paskami) | 4:3\n");
    fprintf(fp,"aspect_mode=%s\n", g_aspect_mode[0]?g_aspect_mode:"wide");
    fprintf(fp,"# touch_mode: absolute | relative | off\n");
    fprintf(fp,"touch_mode=%s\n",  g_touch_mode[0]?g_touch_mode:"absolute");
    fprintf(fp,"# napisy: plik napisy/<subtitles_lang>.txt obok danych gry\n");
    fprintf(fp,"subtitles_lang=%s\n", g_subtitles_lang[0]?g_subtitles_lang:"pl");
    fprintf(fp,"# imiona mowcow / opisy dzwiekow: 1 tak, 0 nie, -1 jak w pliku napisow\n");
    fprintf(fp,"subtitles_speakers=%d\n", g_subtitles_speakers);
    fprintf(fp,"subtitles_sounds=%d\n",   g_subtitles_sounds);
    fprintf(fp,"# diagnostyka: 1 = dziennik kazdego klikniecia i kroku skryptu (zapis do wacki.log: F10, awaria, wyjscie)\n");
    fprintf(fp,"diagnostyka=%d\n", g_cfg_diag);
    fprintf(fp,"# sterowanie wspolczesne (ikona myszy w Opcjach): PPM = panel w panoramie, Tab/kolko = postac\n");
    fprintf(fp,"modern_controls=%d\n", g_modern_controls);
    fprintf(fp,"# filtr pikseli (F6, PC/Switch): 0 brak, 1 Scale2x, 2 2xSaI, 3 xBR\n");
    fprintf(fp,"pixel_filter=%d\n", g_pixel_filter);
    fprintf(fp,"# podpowiedzi (zarowka w Opcjach): 1 wlaczone, 0 wylaczone; plik podpowiedzi/<jezyk>.txt\n");
    fprintf(fp,"hints=%d\n", g_hints_on);
    fprintf(fp,"# ile sekund czekac na kolejny stopien podpowiedzi\n");
    fprintf(fp,"hint_wait=%d\n", g_hint_wait);
    fprintf(fp,"# kolory: 0 oryginalne, 1 zywe, 2 bardzo zywe, 3 jasniejsze (-1 domyslne)\n");
    fprintf(fp,"color_filter=%d\n", g_color_filter);
    fprintf(fp,"# 3DS zoom na dolnym ekranie: 0 ostry, 1 gladki, 2 Scale2x, 3 2xSaI, 4 xBR\n");
    fprintf(fp,"zoom_filter=%d\n", g_zoom_filter);
    fprintf(fp,"# PC: wygladzanie obrazu (F3): 1 tak, 0 nie, -1 domyslne\n");
    fprintf(fp,"smoothing=%d\n", g_smoothing);
    fprintf(fp,"# 3DS: gorny ekran 800 px (ostrzejszy, bez 3D): 1 tak, 0 nie\n");
    fprintf(fp,"wide_top=%d\n", g_wide_top);
    fclose(fp);
    LOG_INFO("config","saved: fullscreen=%d scale=%d aspect=%s touch=%s",
             g_fullscreen,g_scale_factor?g_scale_factor:1,g_aspect_mode,g_touch_mode);
}
