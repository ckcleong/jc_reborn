/*
 * Platform interface between the engine (repository root sources,
 * compiled as the jc_engine component) and the ESP32 IDF project.
 *
 * Provided here:
 *   - panel init + the push primitives (test pattern / replay) used by
 *     main.c,
 *   - the engine hook implementations that replace graphics_pc.c:
 *     grPushFrame (RGB888 panel buffer -> RGB565 SPI push),
 *     grTicks (esp_timer ms), grWindowOpen/grWindowClose (stage-less),
 *   - device events and sound implementations for the SDL files (GPIO
 *     and esp_driver_i2s), so the engine links end-to-end on device.
 */
#pragma once

#include <stdint.h>
#include "mytypes.h"    // uint8/uint32 engine typedefs

// --- panel (main.c / platform_glue.c) --------------------------------

// Initialise the SPI panel (esp_lcd, board.h pins). Fatal on failure.
void panel_init(void);

// Push RGB565 panel content to the full 320x240 area (blocking draw).
// 'pixels' must hold GR_FRAME_W*GR_FRAME_H uint16 (RGB565, same layout
// as frames.bin — see the substitution formula in platform_glue.c).
void present_panel_rgb565(const uint16_t *pixels);

// Push measurement: pushes 'frames' full panels and prints the
// achieved push fps.
void panel_fps_probe(int frames);

// Panel orientation/mirror (ST7789 MADCTL MX/ML via the esp_lcd
// driver). Calibration helper — final orientation lives in board.h.
void panel_set_mirror(int mx, int my);

// The full-frame RGB565 staging buffer (GR_FRAME_W x GR_FRAME_H,
// internal DMA-capable RAM) for the app to draw into, then push with
// present_panel_rgb565() — used by the setup screen.
uint16_t *panel_frame565(void);

// Push one RGB888 pattern/panel frame from the engine through the
// platform hook (used by the test pattern and by the engine itself
// through grPushFrame()).
void grPushFrame(const uint8 *rgb888, int width, int height);

// Headless story: end the run at an absolute grTicks() deadline — the
// device equivalent of the PC leg's `timeout 300` (the story loop never
// returns on its own). Fired once from the pacing shim; main.c
// implements the closing reports.
void jcStorySetDeadline(int64_t deadline_ms);

// Fired once when the deadline set via jcStorySetDeadline() passes
// (from the pacing shim, in the story's own task). main.c implements
// it: closing memory/cache reports + restart. Defined in main.c.
void jc_story_deadline(void);

// --- engine hooks (see graphics.h comments) --------------------------
uint32 grTicks(void);
