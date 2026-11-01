# ESP32-S3 build

This directory is the ESP-IDF v6.1 project for the shared engine sources at
the repository root. The PC SDL build remains the reference implementation.
See `docs/ESP32.md` for the architecture and `docs/TESTING.md` for the shared
test harness.

```
root/                     shared engine sources
  esp32/
    CMakeLists.txt        IDF project
    partitions.csv        NVS + 3 MB app + 8 MB read-only FATFS
    sdkconfig.defaults    PSRAM, memory routing, FATFS, FreeRTOS tick rate
    components/jc_engine/ engine sources as an IDF component
    main/
      main.c              app entry point
      platform_glue.c     panel, presentation hooks, events pacing, audio
      board.h             pin map
      platform.h          panel/platform interface
      setup.c / setup.h   clock-setup portal
      qr.c / qr.h         QR encoder for the setup screen
      replay_gen.py       generate replay_frames.h from a PC frames.bin
```

## Board

- Module: ESP32-S3 **N16R8** (16 MB flash, 8 MB octal PSRAM).
- Panel: SPI 320x240 RGB565 (ST7789-class). Pins are in `main/board.h`.
- Audio: optional MAX98357A I2S mono amplifier.
- Button: GPIO 0, used to re-enter setup mode.

On N16R8 modules GPIO33–37 are connected to the octal PSRAM and are unusable.
Keep panel pins clear of those, of GPIO19/20 (native USB), GPIO43/44 (UART
console) and GPIO0 (boot button). The default map uses GPIO 9–12, 14 and 21
for the panel.

## Build and flash

Install ESP-IDF v6.1 and activate its environment. With the EIM-style
installer the activation script is generated per IDF version:

```sh
. ~/.espressif/tools/activate_idf_v6.1.sh
```

With the classic installer layout:

```sh
. ~/.espressif/v6.1/esp-idf/export.sh
```

Then:

```sh
cd esp32
idf.py set-target esp32s3
idf.py menuconfig                # optional; sdkconfig.defaults is preset
idf.py build flash monitor
```

`idf.py flash` does not write the resource partition. Build and flash that
image separately.

## Resource image

The engine expects `RESOURCE.MAP`, `RESOURCE.001`, the WAV set and a seed
story-state file in a FAT partition mounted at `/resources`. Build the image
from the PC resource directory:

```sh
mkdir -p /tmp/fatfs_in
cp out/RESOURCE.001 out/RESOURCE.MAP out/sound*.wav /tmp/fatfs_in/
printf 'currentDay=5\ndate=292\n' > /tmp/fatfs_in/.jc_reborn

python "$IDF_PATH/components/fatfs/fatfsgen.py" \
    /tmp/fatfs_in --partition_size 0x800000 --long_name_support \
    --output_file /tmp/resources_ro.img

python -m esptool --chip esp32s3 -p /dev/ttyACM0 --baud 921600 \
    write-flash 0x310000 /tmp/resources_ro.img
```

The partition is mounted read-only (plain FAT, no wear levelling). It supplies
resources only; `.jc_reborn` seeds the story-day state on first boot, after
which the engine keeps that state in NVS. Story progression therefore survives
reboots without a writable resource partition.

## Console

The default console is UART0 through the board's USB-UART bridge. On the
reference board this enumerates as `/dev/ttyACM0` (CH9102, USB ID
`1a86:55d3`), not the native `303a:1001` USB port. UART0 keeps panic output
across resets; the native USB-Serial/JTAG console does not.

## Optional frame replay

`main/replay_gen.py` converts a PC-captured `frames.bin` into an embedded
header. When `main/replay_frames.h` exists, the build defines
`JC_REPLAY_FRAMES` and the device can replay the captured frames cyclically.

```sh
python3 main/replay_gen.py ../_baseline/<suite>/story/frames.bin 8
idf.py build flash monitor
idf.py partition-table-flash     # if the partition layout changed
```

An 8-frame embed is about 1.2 MB. The 3 MB app partition holds roughly 15
frames.

## Audio wiring

The MAX98357A is optional: without it the story runs silently (the I2S clocks
into nothing).

| MAX98357A | ESP32-S3 | note |
|---|---|---|
| Vin | 5V (VUSB) | 2.5–5.5 V; 3V3 also works at lower volume |
| GND | GND | common ground with the panel and ESP32 |
| BCLK | GPIO 15 | bit clock |
| LRC | GPIO 17 | word select |
| DIN | GPIO 16 | I2S data in |
| GAIN | unconnected | 9 dB default (100 kΩ→GND 15 dB, GND 12 dB, 100 kΩ→Vin 3 dB, Vin 6 dB) |
| SD | unconnected | amp enabled; output = (L+R)/2 |
| speaker | +/− pads | 4–8 Ω, up to 3 W at 5 V |

Format: I2S Philips, 11025 Hz, 16-bit stereo. Both slots carry the mono
unsigned 8-bit engine stream. The boot log shows:

```
audio ready (MAX98357A, 11025 Hz mono U8 -> 16-bit stereo)
```

## Setup portal

The engine needs a wall clock. Without an RTC, the device sets it through a
self-hosted portal:

1. On first boot (empty NVS) or when the boot button is held for ~5 seconds,
   the device enters setup mode. The panel shows a QR code and the device
   hosts the open access point `JohnnyCastaway-Setup` with a wildcard DNS
   server, so a phone that joins the network gets the portal at
   `http://192.168.4.1/`.
2. The page prefills the phone's clock. **Save & restart** posts the time
   (epoch plus timezone offset) and the **Sound enabled** checkbox. The
   device applies the time, stores it in NVS and restarts into the story.
3. The clock is snapshotted to NVS every ~10 minutes, so a power cut costs at
   most ~10 minutes plus the power-off gap.

Browser notes:

- iPhone: the captive-portal window opens automatically and the flow works.
- Android/Chrome may upgrade the typed address to `https://` and fail with
  `ERR_TOO_MANY_RETRIES`, because nothing listens on port 443 and a bare IP on
  a private AP cannot have a valid certificate. Use Samsung Internet with the
  explicit `http://192.168.4.1/`, or tap the system's "Sign in to network"
  notification.

The QR encoder is `main/qr.c` (byte mode, versions 1–4, ECC level L, ISO/IEC
18004 masks). The portal is `main/setup.c`; the `dns_server` component is the
Espressif captive-portal example.

## Diagnostics

- The boot log prints internal/PSRAM free and largest-block sizes.
- At story start the log shows the NVS `currentDay`/`date`, the effective
  clock and the sound preference. NVS story-state updates and each ADS scene
  (name, tag, flags) are logged as they happen.
- A colour-bar test pattern and a full-panel push-fps probe run at boot.
- A heap trajectory (internal, PSRAM, story task stack high-water mark) is
  printed every 5 seconds.
- `JC_BENCH_AT_BOOT` runs the compose benchmark instead of the story.
- `JC_STORY_DEADLINE_S` ends the story after N seconds, prints the closing
  `[MEM]`/cache reports and restarts.
- `JC_PANEL_CALIB` cycles solid colours with normal and byte-swapped pixels to
  identify panel channel order and orientation.

See `docs/ESP32.md` for memory routing, presentation and audio details.
