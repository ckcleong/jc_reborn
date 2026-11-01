/*
 * Board pin map — generic ESP32-S3 + SPI 320x240 panel (ST7789/ILI9341
 * class) on an N16R8-class module (16 MB flash, 8 MB octal PSRAM).
 *
 * Override per board here — nothing else hardcodes pins.
 */
#pragma once

// SPI panel. IMPORTANT: on N16R8-class modules (octal PSRAM) GPIO33-37
// are wired to the PSRAM and are UNUSABLE — keep panel pins clear of
// them, of 19/20 (native USB), 43/44 (UART console), 0 (boot button),
// and of the button/audio assignments below. Current picks: 9-12/14/21.
#define BOARD_PANEL_SCLK        12
#define BOARD_PANEL_MOSI        11
#define BOARD_PANEL_CS          10
#define BOARD_PANEL_DC          9
#define BOARD_PANEL_RST         14
#define BOARD_PANEL_LED         21      // backlight / LED pin, GPIO_ON active
#define BOARD_PANEL_PCLK_HZ     (80 * 1000 * 1000)

// Panel geometry/polarity: RGB565 with the panel chip's default 16-bit
// colour order.
#define BOARD_PANEL_COLOR_ORDER 0       // 0 = RGB order default of panel chip
// Orientation (ST7789 MADCTL bits via esp_lcd_panel_mirror, applied
// after the swap_xy that turns the native 240x320 portrait into the
// 320x240 landscape frame). Calibrated with the JC_PANEL_CALIB dot
// sweep: y needs flipping in the swapped orientation, which maps to
// driver mirror_x.
#define BOARD_PANEL_MIRROR_X    1
#define BOARD_PANEL_MIRROR_Y    0

// Button and audio assignments
#define BOARD_BTN_GPIO          0       // input: pause/exit button
#define BOARD_I2S_SCLK          15      // audio out: MAX98357A I2S amp,
#define BOARD_I2S_DOUT          16      //   11025 Hz mono U8 engine stream
#define BOARD_I2S_LRCK          17      //   expanded to 16-bit stereo slots
