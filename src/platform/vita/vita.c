/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła / szczuru
 *
 * src/platform/vita/vita.c — PlayStation Vita platform hooks. */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/system.h"
#include "wacki/platform/input.h"
#include "wacki/platform/video.h"
#include <SDL.h>
#include <psp2/power.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

extern int  g_fullscreen;
extern char g_data_root[260];
extern const int g_wacki_pe_slice_count;

/* Full clocks (ReClaw's vita_speed.h): apps start at 333 MHz CPU; the
 * game gets 444 MHz CPU, 222 bus, 222 GPU, 166 GPU crossbar. Runs before
 * main() (so before SDL_Init, as the note asks). Also: the rear touch pad
 * is touched by accident while holding the Vita — SDL ignores it. */
__attribute__((constructor)) static void vita_full_speed(void)
{
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);
}

static int load_wacki_exe_dynamic(void)
{
    extern int PeLoaderInit(const char *path);
    char path[300];
    if (g_data_root[0]) {
        snprintf(path, sizeof path, "%s/WACKI.EXE", g_data_root);
        if (PeLoaderInit(path)) return 1;
        snprintf(path, sizeof path, "%s/wacki.exe", g_data_root);
        if (PeLoaderInit(path)) return 1;
    }
    {
        static const char *const fb[] = {
            "ux0:/data/wacki/WACKI.EXE",
            "ux0:/data/wacki/wacki.exe",
            "ux0:/data/wacki/data/WACKI.EXE",
            "ux0:/data/wacki/data/wacki.exe",
            "ux0:/app/WACK10001/WACKI.EXE",
        };
        size_t i;
        for (i = 0; i < sizeof fb / sizeof fb[0]; ++i)
            if (PeLoaderInit(fb[i])) return 1;
    }
    return 0;
}

void plat_apply_video_prefs(void)
{
    g_fullscreen = 1;

    /* Szybkość: nie czekaj na VSync; wymuś prostszy present */
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    /* Próbkowanie ustawia video_vita.c per tekstura (wygładzanie wg
     * pierwszego przycisku Grafiki); domyślnie najbliższy sąsiad. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    /* reszta jak było: PE load… */
    if (g_wacki_pe_slice_count > 0) {
        LOG_INFO("wacki", "WACKI.EXE embedded (%d slices)", g_wacki_pe_slice_count);
        return;
    }
    if (load_wacki_exe_dynamic()) {
        LOG_INFO("wacki", "WACKI.EXE loaded from memory card");
        return;
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Wacki",
        "Nie znalazlem WACKI.EXE.\n"
        "Skopiuj go do ux0:/data/wacki/data/", NULL);
    exit(1);
}

void plat_restore_system_volume(void) {}
int  plat_handle_platform_key(int sym) { (void)sym; return 0; }
void plat_pad_read_extra(float *ax, float *ay) { (void)ax; (void)ay; }
int  plat_input_has_keyboard(void) { return 0; }
