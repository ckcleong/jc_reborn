/*
 * ESP32-S3 platform glue:
 *   - SPI panel bring-up + full-frame RGB565 push (esp_lcd, blocking),
 *   - grPushFrame(): converts the engine's canonical RGB888 panel
 *     buffer (grPanel) line-by-line to RGB565 and pushes it over SPI;
 *     the shift formula is byte-identical to grDumpFrameRef()
 *     (graphics.c), which produces frames.bin, so the two conversion
 *     sites must stay in sync.
 *   - grTicks(): esp_timer, ms — the device side of the grTicks shim
 *     (graphics.h documents the contract; ads.c pacing is the only
 *     consumer).
 *   - grWindowOpen/grWindowClose: no-ops (the panel is initialised by
 *     the app before the engine stage starts; there is no window on
 *     device).
 *   - eventsInit/eventsWaitTick + sound*: device implementations of the
 *     SDL interfaces (GPIO events and esp_driver_i2s). eventsWaitTick
 *     mirrors the PC pacing semantics (delay*20 ms real time, 20 ms
 *     engine-delay unit per events.c), so the device story paces like
 *     the PC reference.
 */

#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_attr.h>   // EXT_RAM_BSS_ATTR (frame-sized staging in PSRAM)
#include <driver/spi_master.h>
#include <driver/gpio.h>

#include "mytypes.h"
#include "utils.h"      // debugMsg, safe_malloc, memTrackExternal*
#include "graphics.h"
#include "events.h"
#include "sound.h"
#include "board.h"
#include "setup.h"      // setup_snapshot_time (clock snapshot)
#include "platform.h"

static const char *TAG = "gr";

// --- panel state ------------------------------------------------------

static esp_lcd_panel_handle_t s_panel = NULL;

// Full-frame RGB565 staging buffer. Lives in internal (DMA-capable) RAM
// on purpose: SPI DMA cannot read from flash-mapped memory, and PSRAM
// DMA needs cache-managed caps. At 320*240*2 = 153 600 B this is the
// one frame kept in internal RAM; verify placement at bring-up.
static uint16_t *s_frame565 = NULL;

void panel_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num   = BOARD_PANEL_SCLK,
        .mosi_io_num   = BOARD_PANEL_MOSI,
        .miso_io_num   = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = GR_FRAME_W * GR_FRAME_H * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = BOARD_PANEL_DC,
        .cs_gpio_num       = BOARD_PANEL_CS,
        .pclk_hz           = BOARD_PANEL_PCLK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        // 1 (not 0!): the value is passed through as the SPI device's
        // transaction queue length (esp_lcd_panel_io_spi.c passes it to
        // xQueueCreate) and a 0-length queue asserts at boot. With 1,
        // esp_lcd blocks on the transaction anyway — same deterministic
        // push timing, one frame in flight.
        .trans_queue_depth = 1,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                             &io_config, &io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_PANEL_RST,
        // IDF v6: LCD_RGB_ELEMENT_ORDER_{RGB,BGR} from esp_lcd_types.h.
        .rgb_ele_order  = (BOARD_PANEL_COLOR_ORDER == 0)
                            ? LCD_RGB_ELEMENT_ORDER_RGB : LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    // 320(w)x240(h) from the panel's 240x320 native frame
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    // Calibrated orientation (board.h; see the dot-sweep note there)
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel,
                                         BOARD_PANEL_MIRROR_X,
                                         BOARD_PANEL_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // Backlight on
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << BOARD_PANEL_LED,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led);
    gpio_set_level(BOARD_PANEL_LED, 1);

    s_frame565 = heap_caps_malloc(GR_FRAME_W * GR_FRAME_H * 2,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (s_frame565 == NULL)
        ESP_LOGE(TAG, "panel staging buffer alloc failed (internal SRAM!)");

    ESP_LOGI(TAG, "panel ready (%d Hz SPI)", BOARD_PANEL_PCLK_HZ);
}

void present_panel_rgb565(const uint16_t *pixels)
{
    // The ST7789 SPI wire wants each 16-bit pixel's HIGH byte first
    // (big-endian pixel words); our buffers hold little-endian uint16,
    // so the DMA would stream the low byte first. Byte-swap into the
    // DMA staging buffer — in place when the source is the staging
    // buffer (the engine grPushFrame path, no extra pass). frames.bin
    // stays the canonical little-endian RGB565 capture; the byte order
    // here is SPI transport, not pixel semantics.
    if (pixels != s_frame565) {
        for (int i = 0; i < GR_FRAME_W * GR_FRAME_H; i++)
            s_frame565[i] = (uint16_t) __builtin_bswap16(pixels[i]);
    } else {
        for (int i = 0; i < GR_FRAME_W * GR_FRAME_H; i++)
            s_frame565[i] = (uint16_t) __builtin_bswap16(s_frame565[i]);
    }

    // One full-panel blocking draw == one present transaction (push and
    // flush united, the contract documented on grPushFrame()).
    esp_lcd_panel_handle_t panel = s_panel;
    esp_lcd_panel_draw_bitmap(panel, 0, 0, GR_FRAME_W, GR_FRAME_H,
                              (void *) s_frame565);
}

void panel_fps_probe(int frames)
{
    // PSRAM staging (CPU-only scratch; see main.c pattern_show).
    static EXT_RAM_BSS_ATTR uint16_t pattern[GR_FRAME_W * GR_FRAME_H];

    // fill the staging pattern with something crunchy to look at
    for (int y = 0; y < GR_FRAME_H; y++)
        for (int x = 0; x < GR_FRAME_W; x++) {
            int c = ((x / 40) % 5) * 6 + 16 + (y % 16) * 8;   // gradient bands
            pattern[y * GR_FRAME_W + x] =
                (uint16_t)(c << 11 | c << 5 | c);
        }

    int64_t t0 = esp_timer_get_time();

    for (int f = 0; f < frames; f++) {
        // animate a marching band so the eye confirms live pushes
        static uint8 offset = 0;
        memcpy(s_frame565, pattern, sizeof(pattern));
        for (int y = 0; y < GR_FRAME_H; y++)
            s_frame565[y * GR_FRAME_W + offset] = 0xF800;  // red field cursor
        offset = (uint8)((offset + 7) % GR_FRAME_W);

        present_panel_rgb565(s_frame565);
    }

    int64_t us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "push fps: %.1f (%d frames, %lld us each)",
             frames * 1000000.0 / (double) us, frames,
             (long long) us / frames);
}

void panel_set_mirror(int mx, int my)
{
    // ST7789 MADCTL MX/ML bits, via the esp_lcd driver. Only meaningful
    // after panel_init; no-op before.
    if (s_panel != NULL)
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, mx, my));
}

uint16_t *panel_frame565(void)
{
    return s_frame565;
}

// --- engine hooks (graphics.h contracts) ------------------------------

void grPushFrame(const uint8 *rgb888, int width, int height)
{
    if (s_frame565 == NULL)
        return;

    // Line-convert RGB888 -> RGB565 with the frames.bin formula, then
    // the push == the present (that identity is the seam's contract).
    for (int y = 0; y < height; y++) {
        const uint8 *row = rgb888 + (size_t) y * width * 3;
        uint16_t *out = s_frame565 + (size_t) y * GR_FRAME_W;
        for (int x = 0; x < width; x++)
            out[x] = grRgb888To565(row + x * 3);
    }

    present_panel_rgb565(s_frame565);
}

// Fused-present seam (see graphics.h): the engine's present decimates
// straight into the staging buffer and hands it back for the SPI draw.
const uint16_t *grPushFrame565Buf(void)
{
    return s_frame565;
}

void grPushFrame565Present(void)
{
    present_panel_rgb565(s_frame565);
}

uint32 grTicks(void)
{
    return (uint32) (esp_timer_get_time() / 1000LL);
}

void grWindowOpen(void)
{
    // Panel lifecycle belongs to app_main (panel_init); the engine has
    // no window concept on device.
}

void grWindowClose(void)
{
}

// --- events (device implementation of the SDL events interface) -------

int evHotKeysEnabled = 0;

static uint32 s_evLastTicks = 0;
static uint32 s_heap_log_count = 0;

// Headless-story deadline: jc_story_deadline_ms is an absolute grTicks()
// deadline set by main.c (0 = unlimited). When the pacing shim sees it
// pass, it fires jc_story_deadline() exactly once — main.c implements it
// (final memory reports + restart). The PC story leg is ended the same
// way (timeout 300); this is the device equivalent, surfaced from the
// story's own task so the closing reports run in a quiescent engine
// state.
static int64_t jc_story_deadline_ms = 0;

void eventsInit(void)
{
    s_evLastTicks = grTicks();
}

void jcStorySetDeadline(int64_t deadline_ms)
{
    jc_story_deadline_ms = deadline_ms;
}

void eventsWaitTick(uint16 delay)
{
    static int64_t s_next_heap_log = 0;

    uint32 waitMs = (uint32) delay * 20;   // engine delay unit, as on PC

    // Real-time pacing with the PC's exact semantics (events.c
    // eventsWaitTick): the anchor is taken when the wait ends, so the
    // frame's blit+present work falls inside the next frame's delay
    // budget and the frame-to-frame period is delay*20 regardless of
    // how long the work took (as long as it fits).
    while ((uint32) (grTicks() - s_evLastTicks) < waitMs)
        vTaskDelay(pdMS_TO_TICKS(2));

    s_evLastTicks = grTicks();

    // Device heap trajectory alongside the engine's [MEM] lines
    // (memtrack counters cover the engine's own heap only; the device
    // additionally reports the FreeRTOS/internal and PSRAM pools, plus
    // the story task's stack high-water mark).
    if (grTicks() >= (uint32) s_next_heap_log) {
        s_next_heap_log = (int64_t) grTicks() + 5000;

        // Snapshot the clock to NVS every ~10 minutes (120 of the 5 s
        // heap-log ticks), so a power cut costs at most ~10 minutes of
        // clock (plus the power-off gap itself).
        if (++s_heap_log_count % 120 == 0)
            setup_snapshot_time();

        ESP_LOGI(TAG, "heap: internal free %zu (largest %zu), psram free %zu "
                      "(largest %zu), story stack hwm %u",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned) uxTaskGetStackHighWaterMark(NULL));
    }

    if (jc_story_deadline_ms && (int64_t) grTicks() >= jc_story_deadline_ms) {
        jc_story_deadline_ms = 0;       // fire once
        jc_story_deadline();
    }
}

// --- sound: MAX98357A I2S amp (esp_driver_i2s) ------------------------
//
// The device implementation of sound.c, with the same single-voice
// semantics:
//   - the shipped sound0..24.wav set plays verbatim: the device stream
//     is the same byte sequence SDL rendered. The WAVs' native format
//     (11025 Hz mono U8) is expanded to 16-bit stereo slots for the
//     MAX98357A (it needs >=16-bit slots; both slots carry the same
//     sample, so its (L+R)/2 mono mixdown reproduces the sample),
//   - a retrigger of the sample that is still sounding is dropped
//     (the island corpora fire ambience clips at animation cadence;
//     restart-replace would stutter),
//   - a different sample preempts with the 48-byte fade tail mixed
//     over the new head (click-free switch).
// SDL pulls the stream from soundCallback(); the I2S TX is push-based,
// so a small audio task plays the pull role: it renders the same state
// machine (current + tail + U8 silence) into a short chunk and blocks
// in i2s_channel_write() — the DMA queue is the pacing clock. The
// chunk (and the DMA depth) is ~23 ms, bounding the preempt latency
// (SDL: up to 93 ms at its 1024-sample buffer).

#include <driver/i2s_std.h>

#define SND_NUM_SAMPLES 25      // sound0..sound24.wav (PC sound.c)
#define SND_CHUNK       256     // U8 bytes per pass (~23 ms at 11025 Hz)
#define SND_TAIL_FADE   48      // ~4.4 ms fade tail (PC sound.c)

struct SndSample {
    uint32 length;
    uint8  *data;
};

int soundDisabled = 1;   // cleared by a successful soundInit() (PC semantics)

static struct SndSample sndSamples[SND_NUM_SAMPLES];
static uint8    sndProbed[SND_NUM_SAMPLES];   // missing-file memory (lazy load)

static uint8    *sndCurPtr;
static uint32   sndCurRemaining;
static int       sndCurIdx = -1;              // which sample sndCurPtr is in
static uint8    *sndTailPtr;
static uint32   sndTailRemaining;

static i2s_chan_handle_t sndTx = NULL;
static TaskHandle_t      sndTaskHandle = NULL;
static SemaphoreHandle_t sndLock = NULL;      // guards the stream state

// The pull role: render one U8 chunk from the same state machine as
// the PC soundCallback() (memcpy current, fold the preempted tail
// over the head, pad U8 silence), expand to 16-bit stereo and push.
static void sndTask(void *arg)
{
    static uint8  u8[SND_CHUNK];
    static int16_t stereo[SND_CHUNK * 2];     // interleaved L,R frames

    for (;;) {
        uint32 n;

        xSemaphoreTake(sndLock, portMAX_DELAY);
        n = (sndCurRemaining > SND_CHUNK) ? SND_CHUNK : sndCurRemaining;

        if (n > 0) {
            memcpy(u8, sndCurPtr, n);
            sndCurPtr       += n;
            sndCurRemaining -= n;
        }

        if (sndTailRemaining > 0) {
            uint32 m = (sndTailRemaining < n) ? sndTailRemaining : n;

            for (uint32 i=0; i < m; i++) {
                int out = u8[i] + (int) sndTailPtr[i] - 127;
                u8[i] = out > 255 ? 255 : (out < 0 ? 0 : (uint8) out);
            }

            sndTailPtr       += m;
            sndTailRemaining -= m;
        }

        if (n < SND_CHUNK)
            memset(u8 + n, 128, SND_CHUNK - (int) n);   // U8 silence
        xSemaphoreGive(sndLock);

        for (int i=0; i < SND_CHUNK; i++) {
            int16_t s = (int16_t) ((u8[i] - 128) << 8);
            stereo[2*i]     = s;
            stereo[2*i + 1] = s;
        }

        size_t written;
        i2s_channel_write(sndTx, stereo, sizeof(stereo), &written,
                          portMAX_DELAY);
    }
}

// Minimal RIFF/WAV reader for the shipped set: validates the container
// and returns the verbatim bytes of the data chunk (the PC plays the
// stream verbatim; every shipped file is native 11025 Hz mono U8).
static uint8 sndLoadWav(int nb)
{
    char path[32];
    FILE *f;
    uint8 hdr[12];

    snprintf(path, sizeof(path), "/resources/sound%d.wav", nb);
    f = fopen(path, "rb");
    if (f == NULL) {
        debugMsg("sound: no %s (probed)", path);
        return 0;
    }

    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0
        || memcmp(hdr + 8, "WAVE", 4) != 0) {
        debugMsg("sound: %s is not a WAV", path);
        fclose(f);
        return 0;
    }

    for (;;) {
        uint8 chunk[8];
        uint32 size;

        if (fread(chunk, 1, 8, f) != 8)
            break;
        size = (uint32) chunk[4] | ((uint32) chunk[5] << 8)
             | ((uint32) chunk[6] << 16) | ((uint32) chunk[7] << 24);

        if (memcmp(chunk, "data", 4) == 0) {
            uint8 *data = safe_malloc(size);

            if (fread(data, 1, size, f) != size) {
                debugMsg("sound: %s: short data chunk", path);
                safe_free(data);
                fclose(f);
                return 0;
            }

            sndSamples[nb].data   = data;
            sndSamples[nb].length = size;
            memTrackLogf("wav sound%d.wav: length=%u (device-owned buffer)",
                         nb, (unsigned) size);
            memTrackExternalAdd(size);
            fclose(f);
            return 1;
        }

        fseek(f, (long) ((size + 1) & ~1u), SEEK_CUR);   // skip, word-padded
    }

    debugMsg("sound: %s: no data chunk", path);
    fclose(f);
    return 0;
}

void soundInit(void)
{
    // The setup portal's "Sound enabled" checkbox (NVS, default on).
    if (!setup_sound_enabled()) {
        ESP_LOGI(TAG, "sound disabled by the setup preference");
        soundDisabled = 1;
        return;
    }

    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                           I2S_ROLE_MASTER);
    chanCfg.dma_frame_num = 64;         // 4x64 frames = ~23 ms queue
    chanCfg.dma_desc_num  = 4;

    if (i2s_new_channel(&chanCfg, &sndTx, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "i2s_new_channel failed - audio disabled");
        soundDisabled = 1;
        return;
    }

    i2s_std_config_t stdCfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(11025),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_SCLK,
            .ws   = BOARD_I2S_LRCK,
            .dout = BOARD_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { 0, 0, 0 },
        },
    };

    if (i2s_channel_init_std_mode(sndTx, &stdCfg) != ESP_OK
        || i2s_channel_enable(sndTx) != ESP_OK) {
        ESP_LOGW(TAG, "i2s init failed - audio disabled");
        soundDisabled = 1;
        return;
    }

    sndLock = xSemaphoreCreateMutex();

    if (xTaskCreate(sndTask, "snd", 4096, NULL, 6, &sndTaskHandle) != pdPASS) {
        ESP_LOGW(TAG, "snd task failed - audio disabled");
        soundDisabled = 1;
        return;
    }

    soundDisabled = 0;
    ESP_LOGI(TAG, "audio ready (MAX98357A, 11025 Hz mono U8 -> 16-bit stereo)");
}

void soundEnd(void)
{
    if (soundDisabled)
        return;

    // Stop the pull task before tearing down the channel and the sample
    // buffers: it may be blocked in i2s_channel_write() and would touch
    // sndCurPtr/sndTailPtr (freed below) on its next pass. Taking the
    // lock first guarantees it is not mid-render.
    if (sndTaskHandle != NULL) {
        xSemaphoreTake(sndLock, portMAX_DELAY);
        vTaskDelete(sndTaskHandle);
        sndTaskHandle = NULL;
        xSemaphoreGive(sndLock);
    }

    i2s_channel_disable(sndTx);

    sndCurPtr       = NULL;
    sndCurRemaining = 0;
    sndCurIdx       = -1;
    sndTailPtr      = NULL;
    sndTailRemaining = 0;

    for (int i=0; i < SND_NUM_SAMPLES; i++)
        if (sndSamples[i].data != NULL) {
            memTrackExternalRemove(sndSamples[i].length);
            safe_free(sndSamples[i].data);
            sndSamples[i].data   = NULL;
            sndSamples[i].length = 0;
        }
}

void soundPlay(int nb)
{
    if (soundDisabled)
        return;

    if (nb < 0 || SND_NUM_SAMPLES <= nb) {
        debugMsg("soundPlay(): wrong sound sample index #%d", nb);
        return;
    }

    // Lazy loading of the WAV file, the first time this sample is used
    // (missing files are probed once, like the PC's soundProbed[]).
    if (sndSamples[nb].data == NULL && !sndProbed[nb]) {
        sndProbed[nb] = sndLoadWav(nb);
        if (sndSamples[nb].data == NULL)
            return;
    }

    if (sndSamples[nb].length == 0)
        return;

    xSemaphoreTake(sndLock, portMAX_DELAY);

    // Single-voice semantics (PC sound.c): a retrigger of the sample
    // that is still sounding is dropped; a different sample preempts
    // with the 48-byte fade tail kept for mixing over the new head.
    // Island ambience clips (surf 6, sand 10) re-fire at animation
    // cadence; restart-replace would stutter through them.
    if (sndCurIdx == nb && sndCurRemaining > 0) {
        xSemaphoreGive(sndLock);
        return;
    }

    if (sndCurRemaining > SND_TAIL_FADE) {
        sndTailPtr       = sndCurPtr + sndCurRemaining - SND_TAIL_FADE;
        sndTailRemaining = SND_TAIL_FADE;
    } else if (sndCurRemaining > 0) {
        sndTailPtr       = sndCurPtr;
        sndTailRemaining = sndCurRemaining;
    } else {
        sndTailRemaining = 0;
    }

    sndCurPtr       = sndSamples[nb].data;
    sndCurRemaining = sndSamples[nb].length;
    sndCurIdx       = nb;

    xSemaphoreGive(sndLock);
}
