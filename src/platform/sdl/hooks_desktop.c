/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/platform/sdl/hooks_desktop.c — desktop platform hooks provider.
 *
 * Each target links exactly one hooks provider for the small platform-variant
 * behaviors the sdl/ core calls through (firmware volume, hardware-button
 * keysyms, display prefs, the PS2 analog/USB-mouse pad extras, keyboard
 * presence). The handhelds + PS2 provide their own; this is the desktop set —
 * mostly no-ops, plus the first-run display-mode picker and "yes, there's a
 * keyboard". That single-provider-per-target rule is what keeps the sdl/ core
 * files free of any WACKI_MIYOO / WACKI_PORTMASTER / WACKI_HANDHELD / WACKI_PS2
 * #ifdef. */

#include "wacki.h"                   /* g_fullscreen / g_scale_factor globals */
#include "wacki/platform/system.h"   /* plat_restore_system_volume */
#include "wacki/platform/input.h"    /* plat_handle_platform_key, plat_pad_read_extra */
#include "wacki/platform/video.h"    /* plat_apply_video_prefs */

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void plat_restore_system_volume(void)            { }
int  plat_handle_platform_key(int sym)           { (void)sym; return 0; }
void plat_pad_read_extra(float *ax, float *ay)   { (void)ax; (void)ay; }

/* The desktop has a real keyboard, so gameplay keybindings beyond ESC are
 * safe (see plat_input_has_keyboard / play_loop.c). */
int  plat_input_has_keyboard(void)               { return 1; }

/* Desktop display prefs: on the first launch (no wacki.cfg yet, no display
 * mode forced on the CLI/env) ask once which mode to use and persist it.
 * Runs before the window is created (a standalone NULL-parent message box is
 * fine), and sets g_fullscreen / g_scale_factor that plat_video_init reads. */
/* A build without data/WACKI.EXE baked in (src/platform/sdl/
 * embedded_wacki_pe_stub.c, e.g. a cross-built .exe that can't ship the
 * game's own file) reads WACKI.EXE from the player's game folder at start,
 * like the console ports do. */
extern const int g_wacki_pe_slice_count;
extern char      g_data_root[260];

static int try_exe_in(const char *dir)
{
    extern int PeLoaderInit(const char *path);
    static const char *const names[] = { "WACKI.EXE", "wacki.exe", "Wacki.exe",
                                         "../WACKI.EXE", "../wacki.exe",
                                         "data/WACKI.EXE", "data/wacki.exe" };
    char path[600];
    for (size_t i = 0; i < SDL_arraysize(names); ++i) {
        size_t n = strlen(dir);
        snprintf(path, sizeof path, "%s%s%s", dir,
                 (n && (dir[n-1] == '/' || dir[n-1] == '\\')) ? "" : "/", names[i]);
        if (PeLoaderInit(path)) return 1;
    }
    return 0;
}

static void load_wacki_exe_dynamic(void)
{
    if (g_wacki_pe_slice_count > 0) return;          /* embedded: done */
    if (g_data_root[0] && try_exe_in(g_data_root)) return;
    char *base = SDL_GetBasePath();
    int ok = base && try_exe_in(base);
    SDL_free(base);
    if (ok || try_exe_in(".")) return;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Wacki",
        "Nie znalaz\xC5\x82""em pliku WACKI.EXE z gry.\n"
        "Po\xC5\x82\xC3\xB3\xC5\xBC go obok programu albo w folderze z danymi gry.",
        NULL);
    exit(1);
}

void plat_apply_video_prefs(void)
{
    load_wacki_exe_dynamic();
    extern int  g_config_first_run;
    extern void ConfigSave(void);
    if (!(g_config_first_run && g_fullscreen == 0 && g_scale_factor == 0))
        return;

    /* Raw UTF-8 literals — the source file is UTF-8, SDL message boxes take
     * UTF-8, and every toolchain we build with uses a UTF-8 execution
     * charset. */
    const SDL_MessageBoxButtonData btns[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Pełny ekran" },
        { 0,                                       1, "Okno 2×" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Okno 1×" },
    };
    const SDL_MessageBoxData mbd = {
        SDL_MESSAGEBOX_INFORMATION, NULL,
        "Wacki — tryb wyświetlania",
        "Jak chcesz grać?\n\n"
        "Możesz to później zmienić:\n"
        "  • F11 — przełącz pełny ekran\n"
        "  • rozciągnij okno za róg, aby zmienić zoom",
        (int)SDL_arraysize(btns), btns, NULL
    };
    int choice = -1;
    if (SDL_ShowMessageBox(&mbd, &choice) == 0) {
        switch (choice) {
        case 0: g_fullscreen = 1;                     break;
        case 1: g_fullscreen = 0; g_scale_factor = 2; break;
        case 2: g_fullscreen = 0; g_scale_factor = 1; break;
        default: break;   /* closed without picking → defaults */
        }
    }
    ConfigSave();
}
