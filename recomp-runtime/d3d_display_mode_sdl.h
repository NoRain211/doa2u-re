// SPDX-License-Identifier: GPL-3.0-or-later
/* The SDL_GPU presenter's display modes (PC Settings' Display Mode and F11),
   included inside its anonymous namespace after fitWindow. */
#ifndef DOA2U_RECOMP_D3D_DISPLAY_MODE_SDL_H
#define DOA2U_RECOMP_D3D_DISPLAY_MODE_SDL_H

/* PC Settings' requests: their count << 2 | the mode, 0 before the first. */
std::atomic<unsigned> mode_request{0u};

/* 0 a window, 1 borderless over its display, 2 exclusive at the display's own
   mode. Exclusive falls back to borderless when SDL refuses it, as the
   Direct3D 11 presenter's does. A hidden (headless) window stays as made. */
void setWindowMode(RecompD3dPresenter *presenter, unsigned mode)
{
    SDL_Window *window = presenter->window;
    if (window == nullptr || (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) != 0) return;
    if (mode == 2u) {
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display);
        SDL_DisplayMode own{};
        if (desktop == nullptr || !SDL_GetClosestFullscreenDisplayMode(display, desktop->w, desktop->h,
                desktop->refresh_rate, true, &own) ||
            !SDL_SetWindowFullscreenMode(window, &own) || !SDL_SetWindowFullscreen(window, true)) {
            std::fprintf(stderr, "recomp d3d presenter: exclusive fullscreen unavailable; borderless: %s\n",
                SDL_GetError());
            mode = 1u;
        }
    }
    if (mode != 2u) {
        SDL_SetWindowFullscreenMode(window, nullptr);
        if (!SDL_SetWindowFullscreen(window, mode != 0u))
            std::fprintf(stderr, "recomp d3d presenter: fullscreen %u: %s\n", mode, SDL_GetError());
    }
    presenter->display_mode = mode;
    if (mode != 0u) presenter->preferred_fullscreen = mode;
    else fitWindow(presenter);
}

/* Applies PC Settings' display mode between frames, as applyQuality does. */
void applyDisplayMode(RecompD3dPresenter *presenter)
{
    const unsigned request = mode_request;
    if (request == presenter->display_mode_applied) return;
    presenter->display_mode_applied = request;
    setWindowMode(presenter, request & 3u);
}

#endif
