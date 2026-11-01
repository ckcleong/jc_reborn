/*
 *  This file is part of 'Johnny Reborn' (PC/SDL platform half).
 *  GPLv3, same terms as the rest.
 *
 *  Everything here is SDL-window-specific. The ESP32 build implements the
 *  same hook surface in esp32/main/platform_glue.c against esp_lcd. The
 *  engine half (graphics.c) contains no SDL code.
 *
 *  Contents:
 *    - the SDL window lifecycle (grWindowOpen / grWindowClose),
 *    - grRefreshDisplay / grToggleFullScreen (window-only concerns),
 *    - grTicks() — SDL_GetTicks shim,
 *    - grPushFrame() — maps the canonical panel into the SDL window,
 *    - grDumpWindow() — the JC_WINDUMP PPM diagnostic, called at the end
 *      of grPushFrame() so the dump shows the just-presented frame.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL2/SDL.h>

#include "mytypes.h"
#include "utils.h"
#include "graphics.h"

static SDL_Window *sdl_window;


static void grDumpWindow()
{
    static int winDumpSeq = 0;

    const char *dir = getenv("JC_WINDUMP");

    if (dir == NULL)
        return;

    SDL_Surface *win = SDL_GetWindowSurface(sdl_window);
    if (win == NULL)
        return;

    char path[512];
    snprintf(path, sizeof(path), "%s/win%05d-%dx%d-%dbpp.ppm",
             dir, ++winDumpSeq, win->w, win->h, win->format->BitsPerPixel);
    FILE *fp = fopen(path, "wb");
    if (fp == NULL)
        return;

    fprintf(fp, "P6\n%d %d\n255\n", win->w, win->h);

    const int bpp = win->format->BytesPerPixel;
    for (int y=0; y < win->h; y++) {
        const uint8 *row = (uint8*) win->pixels + y * win->pitch;
        for (int x=0; x < win->w; x++) {
            uint32 pix = 0;
            memcpy(&pix, row + (size_t) x * bpp, bpp);   // little-endian safe
            uint8 rgb[3];
            SDL_GetRGB(pix, win->format, &rgb[0], &rgb[1], &rgb[2]);
            fwrite(rgb, 1, 3, fp);
        }
    }

    fclose(fp);
}


void grPushFrame(const uint8 *rgb888, int w, int h)
{
    SDL_Surface *win = SDL_GetWindowSurface(sdl_window);
    if (win == NULL)
        return;

    int bpp = win->format->BytesPerPixel;

    // Re-map the palette into the window's pixel format when the palette
    // changed, or when the window surface was re-created with a different
    // pixel format (SDL can recreate it on fullscreen toggles and window
    // resizes); SDL's cached format pointer would then be stale.
    static Uint32 grWinFmt = SDL_PIXELFORMAT_UNKNOWN;
    if (grWinColorDirty || grWinFmt != win->format->format) {
        grWinFmt = win->format->format;
        grWinColorDirty = 0;
    }

    for (int sy=0; sy < h; sy++) {

        const uint8 *inRow = rgb888 + sy * w * 3;
        uint8 *out0 = (uint8*) win->pixels + (2*sy)     * win->pitch;
        uint8 *out1 = (uint8*) win->pixels + (2*sy + 1) * win->pitch;

        // Each panel pixel is replicated 2x horizontally and 2x
        // vertically into the window: window columns 4*b .. 4*b+3 for
        // panel column b.
        for (int b=0; b < w; b++) {

            uint32 color = SDL_MapRGB(win->format,
                                      inRow[3*b + 0], inRow[3*b + 1], inRow[3*b + 2]);

            uint8 *p0 = out0 + 2 * b * bpp;
            uint8 *p1 = out1 + 2 * b * bpp;

            memcpy(p0,           &color,  bpp);
            memcpy(p0 +   bpp,   &color,  bpp);
            memcpy(p1,           &color,  bpp);
            memcpy(p1 +   bpp,   &color,  bpp);
        }
    }

    SDL_UpdateWindowSurface(sdl_window);

    grDumpWindow();
}


// Engine service (see graphics.h): PC implementation.
uint32 grTicks()
{
    return SDL_GetTicks();
}


void grRefreshDisplay()
{
    SDL_UpdateWindowSurface(sdl_window);
}


void grToggleFullScreen()
{
    grWindowed = !grWindowed;

    if (grWindowed) {
        SDL_SetWindowFullscreen(sdl_window, 0);
        SDL_ShowCursor(SDL_ENABLE);
    }
    else {
        SDL_SetWindowFullscreen(sdl_window, SDL_WINDOW_FULLSCREEN);
        SDL_ShowCursor(SDL_DISABLE);
    }

    SDL_UpdateWindowSurface(sdl_window);
}


void grWindowOpen(void)
{
    SDL_Init(SDL_INIT_VIDEO);

    sdl_window = SDL_CreateWindow(
        "Johnny Reborn ...?",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        (grWindowed ? 0 : SDL_WINDOW_FULLSCREEN)
    );

    if (sdl_window == NULL)
        fatalError("Could not create window: %s", SDL_GetError());

    if (!grWindowed)
        SDL_ShowCursor(SDL_DISABLE);

    SDL_UpdateWindowSurface(sdl_window);
}


void grWindowClose(void)
{
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();
}
