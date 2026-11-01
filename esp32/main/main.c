/*
 * This file is part of 'Johnny Reborn' (ESP32-S3 port).
 * GPLv3 — same terms as the rest of the project.
 *
 * App entry point: panel bring-up, resource filesystem, and the
 * headless story.
 *
 * Sequence:
 *   1. init the esp_lcd panel (SPI + ST7789-class init sequence),
 *   2. push a colour-bars test pattern and measure push fps as a live
 *      panel sanity check,
 *   3. mount the resource FATFS partition ('resources', partitions.csv)
 *      and set up the PC runner's environment: bare resource names
 *      resolve onto the mount (utils.c safe_fopen device prefix — the
 *      IDF VFS has no chdir), $HOME is pinned for the config, and
 *      JC_TESTTIME supplies the calendar when no clock is set,
 *   4. run the jc_reborn.c argPlayAll chain headless in a task. The
 *      story plays forever; JC_STORY_DEADLINE_S ends a run for
 *      diagnostic captures, printing the closing [MEM]/cache reports
 *      before restarting.
 *
 * The platform glue in platform_glue.c (grPushFrame/grTicks plus the
 * events/sound stand-ins) is the engine's device half.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>     // setenv
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_timer.h>
#include <esp_log.h>
#include <esp_system.h> // esp_restart
#include <esp_heap_caps.h>
#include <esp_attr.h>   // EXT_RAM_BSS_ATTR (frame-sized staging in PSRAM)
#include <nvs_flash.h>

#include "setup.h"      // clock setup portal
#include <esp_vfs_fat.h>

#include "mytypes.h"
#include "config.h"     // cfgFileRead, TConfig
#include "graphics.h"   // GR_FRAME_W/GR_FRAME_H, graphicsInit/End
#include "utils.h"      // memTracking, memTrackCheckpoint/RunReport
#include "resmgr.h"     // resmgrInit, resmgrCacheReport
#include "story.h"      // storyPlay
#include "ads.h"        // adsPlayBench (JC_BENCH_AT_BOOT)
#include "sound.h"      // soundInit/soundEnd (ESP32 stand-ins, glue)
#include "board.h"
#include "platform.h"

static const char *TAG = "jc_reborn";

// Startup diagnostics: the persisted story state, the effective clock and
// the audio preference.
static void log_startup_state(void)
{
    struct TConfig cfg;
    cfgFileRead(&cfg);

    ESP_LOGI(TAG, "story state (NVS): currentDay=%d date=%d",
             cfg.currentDay, cfg.date);

    if (setup_clock_valid()) {
        time_t now = time(NULL);
        struct tm *tmNow = localtime(&now);
        char clock[32];

        if (tmNow != NULL
            && strftime(clock, sizeof(clock), "%Y-%m-%d %H:%M:%S", tmNow) > 0)
            ESP_LOGI(TAG, "clock: %s (epoch %lld)", clock, (long long) now);
        else
            ESP_LOGI(TAG, "clock: epoch %lld", (long long) now);
    }
    else {
        ESP_LOGI(TAG, "clock: baseline pin %s %02d:00 (day-of-year %d)",
                 getMonthAndDay(), getHour(), getDayOfYear());
    }

    ESP_LOGI(TAG, "sound: %s", setup_sound_enabled() ? "enabled" : "disabled");
}

// Optional pipeline measurement: run the `bench` compose-fps test
// (1/4/8 ttm layers, 3 s wall-clock windows) before the story, then
// restart. Set to 1 to capture the numbers; 0 for the production boot.
#define JC_BENCH_AT_BOOT 0

// Headless-story runtime: seconds until the pacing shim ends the run
// (prints the closing reports and restarts) — the device equivalent of
// the PC leg's `timeout 300`. 0 = play the story forever (the product
// behaviour: the device continuously displays).
#define JC_STORY_DEADLINE_S 0

#if defined(JC_REPLAY_FRAMES)
#include "replay_frames.h"
#endif

#if defined(JC_REPLAY_FRAMES)
// Cyclic replay of the PC-captured frames (one full-panel RGB565 frame
// per entry, exactly the frames.bin layout). What the panel shows here
// must be what frames.bin holds — the same RGB888→RGB565 conversion the
// PC reference performed (see platform_glue.c / grDumpFrameRef()).
static void replay_show(int loops)
{
    for (int f = 0; loops == 0 || f < loops; f++)
        for (int n = 0; n < JC_REPLAY_NFRAMES; n++) {
            present_panel_rgb565(jc_replay_frames[n]);
            vTaskDelay(pdMS_TO_TICKS(125));     // ~8 fps reference pace
        }
}
#endif

// Colour-mapping diagnostic (JC_PANEL_CALIB build): full-screen solid
// colours — red, green, blue, white with normal byte order (dots 1-4),
// then the same four with the two bytes of every 16-bit pixel swapped
// (dots 5-8). The user reports the colours seen per dot count; from
// that the exact panel channel mapping (byte order + RGB/BGR bit) is
// derived and fixed in board.h / the push path.
#if defined(JC_PANEL_CALIB)
static void calib_show(void)
{
    static EXT_RAM_BSS_ATTR uint16_t buffer[GR_FRAME_W * GR_FRAME_H];

    static const uint16_t colours[4] = {
        0xF800, 0x07E0, 0x001F, 0xFFFF,     // RGB565 red, green, blue, white
    };
    static const char *names[4] = { "RED", "GREEN", "BLUE", "WHITE" };

    for (int phase = 1;; phase = phase % 8 + 1) {
        int swapped = (phase > 4);
        uint16_t c = colours[(phase - 1) % 4];
        if (swapped)
            c = (uint16_t) ((c >> 8) | (c << 8));

        for (int i = 0; i < GR_FRAME_W * GR_FRAME_H; i++)
            buffer[i] = c;

        // phase counter: white dots with a black outline, top-left
        for (int i = 0; i < phase; i++) {
            int x0 = 10 + i * 14, y0 = 8;
            for (int dy = -1; dy < 7; dy++)
                for (int dx = -1; dx < 7; dx++)
                    buffer[(y0 + dy) * GR_FRAME_W + (x0 + dx)] = 0x0000;
            for (int dy = 0; dy < 6; dy++)
                for (int dx = 0; dx < 6; dx++)
                    buffer[(y0 + dy) * GR_FRAME_W + (x0 + dx)] = 0xFFFF;
        }

        ESP_LOGI(TAG, "colour calib phase %d/8: %s bytes, sent %s",
                 phase, swapped ? "SWAPPED" : "normal", names[(phase - 1) % 4]);
        present_panel_rgb565(buffer);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}
#endif  // JC_PANEL_CALIB

// Colour-bars test pattern: red / green / blue / grey ramps + a black
// half-frame — legible at once on any panel, exposes colour-order and
// mirror/scaling errors.
static void pattern_show(void)
{
    // PSRAM staging (CPU-only scratch; the DMA'd buffer is s_frame565
    // in platform_glue.c) — 153 600 B does not fit in internal SRAM.
    static EXT_RAM_BSS_ATTR uint16_t buffer[GR_FRAME_W * GR_FRAME_H];

    for (int y = 0; y < GR_FRAME_H; y++)
        for (int x = 0; x < GR_FRAME_W; x++) {
            uint16 shade = (uint16) (31L * (GR_FRAME_H - 1 - y) / (GR_FRAME_H - 1));
            uint16 band  = x / (GR_FRAME_W / 5);
            uint16 r = 0, g = 0, b = 0;

            if (band == 0) r = shade;
            else if (band == 1) g = shade;
            else if (band == 2) b = shade;
            else if (band == 3) { r = shade; g = shade; b = shade; }
            else if ((y / 8) & 1) { r = 31; g = 31; b = 31; }   // checker band

            buffer[y * GR_FRAME_W + x] = (uint16) (r << 11 | g << 5 | b);
        }

    present_panel_rgb565(buffer);
}

// --- Resource filesystem + headless story -----------------------------

// Mount the 'resources' FATFS partition (flash 0x310000; image built
// with the IDF fatfsgen from out/: RESOURCE.001 + RESOURCE.MAP + WAVs
// + the seed story state .jc_reborn) READ-ONLY. The resource set is
// read-only data, so a plain FAT ro mount (no wear levelling, no
// writes) is the durable design.
//   - the engine opens the resource files by bare name ("RESOURCE.MAP",
//     resmgr.c) — the PC runner chdirs into out/; on device (IDF VFS
//     has no chdir) utils.c safe_fopen() prefixes them with the mount
//     point,
//   - config.c seeds its NVS-backed story state from the .jc_reborn
//     file in the image (currentDay=5, date=292) on first boot, then
//     reads and writes NVS; $HOME is pinned to the mount to find it,
//   - JC_TESTTIME=102014 pins the calendar exactly as on PC (utils.c).
static int story_mount_resources(void)
{
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 4,
        .allocation_unit_size   = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_ro("/resources", "resources",
                                                  &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FATFS mount failed (%s) — is the resources image "
                      "flashed at 0x310000? (esp32/README.md)", esp_err_to_name(err));
        return -1;
    }

    setenv("HOME", "/resources", 1);
    // The story's date source: the set clock when available, else the
    // deterministic baseline pin (Oct 20, 14:00).
    if (!setup_clock_valid())
        setenv("JC_TESTTIME", "102014", 1);
    ESP_LOGI(TAG, "resources mounted (ro), story date from %s "
                  "(day 5, date 292, deadline %d s)",
             setup_clock_valid() ? "the set clock" : "the pinned JC_TESTTIME",
             JC_STORY_DEADLINE_S);
    return 0;
}

// Deadline fire (platform glue, fired once from the pacing shim): the
// device equivalent of the PC leg's `timeout 300` — print the closing
// reports the PC stdout.log ends with, then restart for a fresh run
// (fresh pinned story state, like the runner's per-leg HOME).
void jc_story_deadline(void)
{
    ESP_LOGI(TAG, "story deadline (%d s) reached — closing reports",
             JC_STORY_DEADLINE_S);
    memTrackRunReport();
    resmgrCacheReport();
    soundEnd();
    graphicsEnd();
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300));     // UART drain
    esp_restart();
}

// Hold the boot button (GPIO 0, active low) ~5 s to re-enter the
// clock-setup portal on the next boot.
static void button_task(void *arg)
{
    (void) arg;
    for (;;) {
        if (setup_button_held()) {
            ESP_LOGW(TAG, "boot button held — requesting setup mode");
            setup_request();
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// The jc_reborn.c argPlayAll chain, executed verbatim: resmgrInit ->
// boot checkpoint -> graphicsInit/soundInit -> storyPlay -> closing
// reports. storyPlay() never returns on its own (infinite while(1),
// ended by `timeout` on PC and by the pacing-shim deadline on device);
// the tail calls exist for symmetry.
//
// arg != 0 runs the `bench` compose-fps measurement instead of the
// story (then restarts for a clean boot). This task exists because
// engine decompression puts 8 KB of LZW tables on the stack — do not
// run engine work on the app_main task (default stack ~3.5 KB).
static void story_task(void *arg)
{
    int bench = arg != NULL;

    // Lazy acquisition (loadEagerly = 0), as on the PC. Retention is
    // deliberately left at RESMGR_RETAIN_OFF: LRU retention is a PC
    // tuning knob (jc_reborn.c 'lru[=bytes]') and is not enabled here.
    resmgrInit("RESOURCE.MAP", 0);
    memTrackCheckpoint("boot");     // peak during resource loading

    log_startup_state();

    graphicsInit();

    if (bench) {
        // Compose fps at 1/4/8 ttm layers (3 s wall-clock windows, PC
        // `bench` mode). Restarting afterwards gives the story a clean
        // engine state.
        ESP_LOGI(TAG, "bench: compose fps at 1/4/8 layers follows");
        adsPlayBench();
        esp_restart();
    }

    soundInit();

    storyPlay();

    memTrackRunReport();            // peak during a whole story
    resmgrCacheReport();            // cache hit/eviction figures
    soundEnd();
    graphicsEnd();

    vTaskDelay(portMAX_DELAY);
}

void app_main(void)
{
    ESP_LOGI(TAG, "jc_reborn filesystem + headless story");

    // NVS (WiFi calibration + the clock snapshot) + setup mode.
    esp_err_t nvsErr = nvs_flash_init();
    if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES
        || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvsErr = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvsErr);

    // Memory-routing sanity printout: the engine's big surfaces must
    // land in PSRAM (see docs/ESP32.md).
    ESP_LOGI(TAG, "internal free %zu (largest %zu), psram free %zu (largest %zu)",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    panel_init();

    // 2. panel sanity: test pattern + push fps measurement (full-panel
    //    blocking pushes at 80 MHz SPI; PC compose fps is measured
    //    separately by `bench`).
    pattern_show();
    vTaskDelay(pdMS_TO_TICKS(2000));
    panel_fps_probe(60);

    // Setup mode: no valid clock (first boot / after a power cut with
    // an empty NVS) or the boot button held ~5 s starts the QR portal.
    // Own task: WiFi init + httpd don't fit the app_main stack.
    if (setup_needed()) {
        if (xTaskCreate(setup_run, "setup", 16 * 1024, NULL, 5, NULL)
                != pdPASS) {
            ESP_LOGE(TAG, "cannot create the setup task");
            esp_restart();
        }
        vTaskDelay(portMAX_DELAY);      // the setup task owns the device
    }
    setup_restore_time();

    // 3. resources + environment, then the headless story.
    if (story_mount_resources() == 0) {
        memTracking = 1;            // 'memstats' leg equivalent

        // Hold the boot button ~5 s to re-enter setup mode.
        if (xTaskCreate(button_task, "btn", 2048, NULL, 3, NULL) != pdPASS)
            ESP_LOGW(TAG, "cannot create the button task");

#if JC_STORY_DEADLINE_S > 0
        // Diagnostic deadline (see JC_STORY_DEADLINE_S); 0 = unlimited.
        jcStorySetDeadline((int64_t) grTicks() + JC_STORY_DEADLINE_S * 1000LL);
#endif

        // Own task: the engine story needs far more stack than the
        // app_main default, and app_main must stay responsive.
        void *taskArg = NULL;
#if JC_BENCH_AT_BOOT
        taskArg = (void *) 1;      // bench instead of story, then restart
#endif
            BaseType_t rc = xTaskCreate(story_task, "story", 28 * 1024,
                                        taskArg, 5, NULL);
            if (rc != pdPASS) {
                // The stack is 28 KB rather than 32: the story's
                // measured peak is ~21.7 KB, and with the setup
                // feature's statics the internal heap's largest block
                // is 31.7 KB, so a 32 KB stack does not fit.
                ESP_LOGE(TAG, "cannot create the story task (rc=%d): internal "
                              "free %zu (largest %zu), psram free %zu "
                              "(largest %zu)",
                         rc,
                         heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                         heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
                esp_restart();
            }
        return;     // the story runs; the deadline hook ends/restarts it
    }

    // Mount failed — fall back to the panel diagnostics so the device
    // stays inspectable (panel replay instead of a dead console).
#if defined(JC_REPLAY_FRAMES)
    ESP_LOGI(TAG, "falling back to the replay loop");
    replay_show(0);   // endless
#else
    ESP_LOGI(TAG, "no replay_frames.h — see main/replay_gen.py; idling");
    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
#endif
}
