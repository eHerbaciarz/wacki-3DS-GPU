/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła / szczuru
 *
 * src/platform/vita/gamepad_vita.c — Vita controls.
 *
 * Cross (SDL A)  → LMB
 * Circle (SDL B) → RMB
 * Triangle       → aspect
 * Square         → touch mode cycle
 * START          → pause
 * L / R          → quickload / quicksave
 */

#include "wacki.h"
#include "wacki/log.h"
#include "wacki/platform/input.h"
#include "sdl_internal.h"
#include <SDL.h>

static SDL_GameController *s_pad = NULL;

#define PAD_ANALOG_MAX_PX   9
#define PAD_ANALOG_DEADZONE 8000

void platform_pad_open(void)
{
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        LOG_INFO("platform", "SDL_INIT_GAMECONTROLLER: %s", SDL_GetError());
        return;
    }
    {
        int i;
        for (i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) {
                s_pad = SDL_GameControllerOpen(i);
                if (s_pad) {
                    LOG_INFO("platform", "controller: %s",
                             SDL_GameControllerName(s_pad));
                    return;
                }
            }
        }
    }
}

/* SELECT is a modifier: SELECT + square = hint (text/hints.c); SELECT on
 * its own = colour filter, on release when no combo was used. */
static int s_select_combo = 0;

int platform_pad_handle_event(const SDL_Event *ev)
{
    int select_held = s_pad &&
        SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_BACK);
    switch (ev->type) {
    case SDL_CONTROLLERBUTTONUP:
        if (ev->cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
            if (!s_select_combo) ColorFilterCycle();
            return 1;
        }
        return 0;
    case SDL_CONTROLLERBUTTONDOWN:
        if (ev->cbutton.button == SDL_CONTROLLER_BUTTON_X && select_held) {
            HintsRequest();
            s_select_combo = 1;
            return 1;
        }
        switch (ev->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A:             g_lmb_clicked        = 1; break;
        case SDL_CONTROLLER_BUTTON_B:             g_rmb_clicked        = 1; break;
        case SDL_CONTROLLER_BUTTON_Y:             platform_video_toggle_aspect_mode(); break;
        case SDL_CONTROLLER_BUTTON_X:             platform_touch_cycle_mode();         break;
        case SDL_CONTROLLER_BUTTON_START:         g_pause_menu_request = 1; break;
        /* SELECT = colour filter on release (above) / modifier */
        case SDL_CONTROLLER_BUTTON_BACK:          s_select_combo = 0;               break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  g_quickload_request  = 1; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: g_quicksave_request  = 1; break;
        default: return 0;
        }
        return 1;

    case SDL_CONTROLLERDEVICEADDED:
        if (!s_pad && SDL_IsGameController(ev->cdevice.which))
            s_pad = SDL_GameControllerOpen(ev->cdevice.which);
        return 1;

    case SDL_CONTROLLERDEVICEREMOVED:
        if (s_pad && ev->cdevice.which ==
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pad))) {
            SDL_GameControllerClose(s_pad);
            s_pad = NULL;
        }
        return 1;

    default:
        return 0;
    }
}

void platform_pad_read_motion(int *dx, int *dy, float *ax, float *ay)
{
    if (!s_pad) return;
    *dx += SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
         - SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    *dy += SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)
         - SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_UP);

    {
        int sx = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTX);
        int sy = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (sx > PAD_ANALOG_DEADZONE || sx < -PAD_ANALOG_DEADZONE)
            *ax = (float)sx / 32767.0f * (float)PAD_ANALOG_MAX_PX;
        if (sy > PAD_ANALOG_DEADZONE || sy < -PAD_ANALOG_DEADZONE)
            *ay = (float)sy / 32767.0f * (float)PAD_ANALOG_MAX_PX;
    }
    plat_pad_read_extra(ax, ay);
}

int plat_pad_menu_nav(int *up, int *down, int *confirm)
{
    *up = *down = *confirm = 0;
    if (!s_pad) return 0;
    SDL_GameControllerUpdate();
    {
        int sy = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY);
        int u = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_UP)
             || sy < -PAD_ANALOG_DEADZONE;
        int d = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)
             || sy >  PAD_ANALOG_DEADZONE;
        int c = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_A);
        static int pu = 0, pd = 0, pc = 0;
        if (u && !pu) *up = 1;
        if (d && !pd) *down = 1;
        if (c && !pc) *confirm = 1;
        pu = u; pd = d; pc = c;
    }
    return 1;
}

void plat_input_flush(void)
{
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    g_lmb_clicked = 0;
    g_rmb_clicked = 0;
}
