# ESP32-S3 port

The ESP32-S3 build runs the same engine sources as the PC build. The PC build
remains the reference implementation; the device build supplies the platform
half and the application entry point.

## Target hardware

- ESP32-S3 with 16 MB flash and 8 MB octal PSRAM (N16R8-class module).
- 320x240 RGB565 SPI panel (ST7789-class).
- Optional MAX98357A I2S mono amplifier.
- Boot button on GPIO 0 for re-entering setup mode.

Pin assignments are in `esp32/main/board.h`.

## Project layout

```
root/                     shared engine sources
  esp32/
    CMakeLists.txt        IDF project
    partitions.csv        NVS + 3 MB app + 8 MB read-only FATFS
    sdkconfig.defaults    PSRAM, memory routing, FATFS, FreeRTOS tick rate
    components/jc_engine/ shared engine sources as an IDF component
    main/
      main.c              application entry point
      platform_glue.c     panel, presentation hooks, events pacing, audio
      board.h             pin map
      platform.h          panel/platform interface
      setup.c / setup.h   clock-setup portal
      qr.c / qr.h         QR encoder for the setup screen
      replay_gen.py       generate replay_frames.h from a PC frames.bin
```

The `jc_engine` component compiles `graphics.c`, `events.c`'s engine-side
peers, and the rest of the engine. It excludes the PC-only files:

- `events.c` — SDL event loop and pacing
- `sound.c` — SDL audio and WAV loading
- `graphics_pc.c` — SDL window and PC push
- `jc_reborn.c` — PC command-line entry point

`esp32/main/platform_glue.c` provides the same hook surface as those files.

## Platform split

`graphics.h` declares the hooks; the platform half defines them:

| hook | PC (`graphics_pc.c`) | ESP32 (`platform_glue.c`) |
|---|---|---|
| `grWindowOpen` / `grWindowClose` | create/destroy the SDL window | no-op; `app_main` initialises the panel |
| `grPushFrame` | map the canonical panel into the SDL window | convert to RGB565 and push over SPI |
| `grPushFrame565Buf` / `grPushFrame565Present` | NULL/no-op | fused RGB565 staging buffer and SPI draw |
| `grTicks` | `SDL_GetTicks()` | `esp_timer` milliseconds |
| `grRefreshDisplay` / `grToggleFullScreen` | window operations | not used |

The engine half contains no SDL code. The ESP32 push uses the same
`grRgb888To565()` conversion as the PC `frames.bin` dump, so both paths
produce the same wire content.

## Presentation

`grPresentFrame()` decimates the 640x480 4-bit compose canvas 2:1 with a 2x2
box average. On PC it fills `grPanel` (RGB888, 320x240) and calls
`grPushFrame()`. On ESP32 the fused path decimates directly into the platform's
RGB565 staging buffer and calls `grPushFrame565Present()`.

`present_panel_rgb565()` byte-swaps the staging buffer in place and performs
one blocking `esp_lcd_panel_draw_bitmap()` for the full panel. The panel
expects big-endian pixel words on the SPI wire; `frames.bin` remains the
canonical little-endian capture.

The panel is initialised with `esp_lcd` on SPI2 at 80 MHz, 16 bits per pixel,
with the ST7789 driver. Orientation is `swap_xy` plus the mirror settings from
`board.h`.

## Filesystem and environment

The engine opens resources by bare name (`RESOURCE.MAP`, `RESOURCE.001`,
`soundN.wav`). The IDF VFS has no `chdir()`, so `safe_fopen()` prefixes bare
paths with `/resources` on `__xtensa__`. Absolute paths pass through
unchanged.

`app_main` mounts the `resources` FATFS partition read-only at `/resources`:

- `RESOURCE.MAP`, `RESOURCE.001` and the WAV set are packed into a FAT image
  and flashed separately from the application.
- The image also ships `.jc_reborn` with `currentDay=5` and `date=292`.
  On ESP32 this file is only a first-boot seed: `config.c` stores the story
  day state in NVS and reads it from there on subsequent runs.
- `$HOME` is set to `/resources` so `config.c` can find that seed file.
- If no valid clock is stored, `JC_TESTTIME=102014` pins the calendar to the
  same reference moment the PC test runner uses.

The partition layout is:

| name | type | offset | size |
|---|---|---:|---:|
| `nvs` | data/nvs | 0x9000 | 0x4000 |
| `phy_init` | data/phy | 0xd000 | 0x1000 |
| `otadata` | data/ota | 0xe000 | 0x2000 |
| `app` | app/factory | 0x10000 | 3 MB |
| `resources` | data/fat | 0x310000 | 8 MB |

The resource partition is plain FAT mounted read-only, not wear-levelled:
the engine only reads resources from it. Story day state is persisted in NVS
instead, so no writable filesystem is needed and the resource image stays
durable.

## Memory routing

The engine's large surfaces live in PSRAM:

- `grPanel` (230 400 B) is marked `EXT_RAM_BSS_ATTR` under `__XTENSA__` and
  placed in `.ext_ram.bss` by `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`.
- The two 153 600 B test-pattern staging buffers are marked the same way.
- Heap allocations are routed by `CONFIG_SPIRAM_USE_MALLOC`. The
  always-internal threshold is 256 bytes: the engine's resident set is mostly
  small allocations, and routing those into internal SRAM fragments it into
  crumbs that starve FreeRTOS queues and VFS buffers. Everything above ~1 KB
  goes to PSRAM.
- Internal SRAM is reserved for stacks, SPI/I2S DMA buffers and file buffers.
  `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` keeps 32 KB available.

The full-panel RGB565 SPI staging buffer is 153 600 B in internal DMA-capable
RAM. The application runs the story in a 28 KB task; `app_main`'s default stack
is too small for the engine's decompression tables.

## Pacing

The engine expresses delays in 20 ms units (`eventsWaitTick(delay)` multiplies
by 20). The PC implementation waits until `SDL_GetTicks()` reaches the target,
with the anchor taken when the wait ends. The ESP32 stand-in reproduces that
semantics with `grTicks()` and `vTaskDelay()`, so the time spent composing and
presenting a frame falls inside the next frame's delay budget.

`CONFIG_FREERTOS_HZ=1000` gives the pacing loop millisecond resolution.

## Audio

`platform_glue.c` implements the same single-voice model as the PC `sound.c`:

- WAVs are loaded lazily on first play from `/resources/soundN.wav` and cached
  until `soundEnd()`.
- The shipped files are 11025 Hz mono unsigned 8-bit. They are expanded to
  16-bit stereo I2S slots; the MAX98357A's `(L+R)/2` mixdown reproduces the
  mono sample.
- A retrigger of the sample that is still sounding is dropped.
- A different sample preempts, with the previous sample's 48-byte tail mixed
  over the new sample's head to avoid a click.
- A 256-byte render chunk and a ~23 ms I2S DMA queue provide the pull pacing.

The "Sound enabled" preference from the setup portal can disable audio.

## Clock and setup portal

The engine reads the wall clock for island day/night, holiday dates and
story-day progression. The device has no RTC, so the clock is set once through
a self-hosted portal and persisted in NVS.

- On first boot (no stored clock) or when the boot button is held for ~5
  seconds, the device enters setup mode.
- The panel shows a QR code encoding the WIFI-join URI for the open access
  point `JohnnyCastaway-Setup`.
- A wildcard DNS server makes the phone's captive-portal check open
  `http://192.168.4.1/`.
- The page prefills the phone's clock and posts it with the timezone offset
  and the sound preference.
- `POST /set` applies the time with `settimeofday()`, stores it in NVS, and
  restarts into the story.
- During playback the clock is snapshotted to NVS every ~10 minutes, so a
  power cut costs at most ~10 minutes plus the power-off gap.

Story-day state (`currentDay`, `date`) is stored in NVS as well, so story
progression survives reboots independently of the clock snapshot. See
`STORY.md` for the day cycle, scene selection and persistence rules.

`main/qr.c` is a minimal byte-mode QR encoder (versions 1–4, ECC level L,
ISO/IEC 18004 mask selection). `main/setup.c` implements the softAP, DNS,
HTTP server and setup screen. The `dns_server` component is the Espressif
captive-portal example.

## Application flow

`app_main` performs:

1. NVS init and a heap/PSRAM availability log.
2. Panel init, colour-bar test pattern, full-panel push-fps probe.
3. Setup mode if the clock is missing or requested; otherwise restore the
   clock from NVS.
4. Mount `/resources`, enable memstats, start the boot-button task, optionally
   arm the story deadline, and create the story task.
5. The story task runs the same sequence as the PC `window memstats nosound`
   leg: `resmgrInit` (lazy), boot checkpoint, `graphicsInit`, `soundInit`,
   `storyPlay`, closing reports.

If the resource mount fails, the device falls back to the replay loop when
`replay_frames.h` is embedded, otherwise it idles with a console message.

## Diagnostics

- A heap trajectory is printed every 5 seconds: internal and PSRAM free/largest
  blocks and the story task stack high-water mark.
- At story start the log shows the NVS `currentDay`/`date`, the effective
  clock and the sound preference. NVS story-state updates and each ADS scene
  (name, tag, flags) are logged as they happen.
- The boot push-fps probe measures the full-panel blocking SPI push rate.
- `JC_BENCH_AT_BOOT` runs the compose benchmark instead of the story.
- `JC_STORY_DEADLINE_S` ends the story after N seconds, prints the closing
  memory and cache reports, and restarts.
- `JC_PANEL_CALIB` cycles solid colours with normal and byte-swapped pixels to
  identify the panel's channel order.
- `JC_REPLAY_FRAMES` replays captured PC frames from `replay_frames.h`.

See `docs/TESTING.md` for the shared harness and environment variables, and
`esp32/README.md` for build, flash and wiring instructions.

## Constraints

- Full-resolution 640x480 presentation over SPI is not viable at 80 MHz; the
  panel runs at 320x240 RGB565.
- Panel colour response and dither appearance are properties of the LCD and
  are not part of pixel parity. The data path is checked against the PC
  reference by replaying `frames.bin`.
- The IDF version is pinned by the installed toolchain. `esp_lcd` and
  `esp_driver_i2s` APIs differ from IDF v4/v5 examples; this project targets
  IDF v6.1.
- Internal SRAM is the scarcest resource. Frame-sized buffers must stay in
  PSRAM, and engine work must not run on the `app_main` stack.
