# Testing and diagnostics

The PC build is the reference implementation. Device changes are checked
against captured PC frame streams, and composition changes are checked against
a previous PC build.

## Baseline suites

`tools/run_baseline.sh <target-dir> [binary]` runs the standard set of legs
and records the results under `<target-dir>/<leg>/`:

| leg | command | end |
|---|---|---|
| story | `window memstats nosound` | timeout |
| bench | `bench nosound` with `JC_BENCHFIX=1` | natural |
| islandads | `island ads JOHNNY.ADS 2 nosound` | natural |
| fadetest | `ttm GJNAT3.TTM nosound` with `JC_FADETEST=1` | natural |
| mjfish | `ttm MJFISH.TTM nosound` | natural |
| gjvis5 | `ttm GJVIS5.TTM nosound` | natural |
| gjvis6 | `ttm GJVIS6.TTM nosound` | natural |

Each leg directory contains `frames.bin` (RGB565 320x240, 153 600 bytes per
frame), `stdout.log`, `stderr.log`, and the run writes a `SUMMARY.md` manifest
with frame counts, md5s and `[MEM]` highlights. The binary must be a
`-DMEMTRACK` build for the memstats legs.

`tools/compare_baseline.sh <ref-dir> <test-dir>` compares two suites:

- The six naturally-ending legs must be byte-identical over the whole file.
- The story leg is cut by a wall-clock timeout while frame delivery is paced
  in real time, so its frame count jitters. It gates on byte identity over the
  common prefix, plus a sanity band on the count delta. Content is
  deterministic because the runner pins the random seed and the clock.

Suites must run on an idle machine: a concurrent build can starve the story
leg and skew its timeout cut.

## Determinism pins

The runner sets these for every suite:

- `SDL_VIDEODRIVER=dummy` — headless.
- `JC_FRAMEDUMP=<leg-dir>` — writes frames and pins `srand(42)`, which makes
  the island's random `OCEAN` background choice reproducible.
- `JC_TESTTIME=102014` — pins the wall clock to October 20, 14:00. The story
  derives the island day/night from `hour % 8`, holiday dates from
  month/day, and story-day progression from day-of-year.
- `HOME=<target-dir>/home` with a fresh `.jc_reborn` containing
  `currentDay=5` and `date=292`. `storyUpdateCurrentDay()` reads and writes
  this file; `storyCurrentDay` drives the scene pool and the raft build
  progress. Pinning it keeps the story content independent of the invoking
  user's home directory. `date=292` equals the pinned day-of-year, so the run
  records no day bump.

The default story timeout is 300 seconds. Override `JC_TESTTIME` by exporting
it before invoking the runner if a different clock class is needed.

## Environment variables

| variable | effect |
|---|---|
| `JC_FRAMEDUMP=<dir>` | append every presented panel frame to `<dir>/frames.bin` as RGB565 320x240; also pins `srand(42)` |
| `JC_WINDUMP=<dir>` | dump the SDL window surface after every display update as `<dir>/winNNNNN-<W>x<H>-<bpp>.ppm` |
| `JC_FADETEST` | force `grFadeOut()` every 100 display updates, cycling all five fade types |
| `JC_BENCHFIX` | make each benchmark phase run a fixed 500 frames instead of 3 seconds |
| `JC_TESTTIME=MMDDHH` | pin the engine's wall-clock helpers to a fixed moment |
| `JC_SEGVDUMP` | install a SIGSEGV backtrace handler on Linux (`-rdynamic` is in the link flags) |
| `JC_SNDTRACE=<path>` | write the rendered 11025 Hz U8 mono audio stream to `<path>` and log `SND`, `SNDDROP`, `SCENE` and `WALK` marks to stderr |

## Memory checks

The story leg's `[MEM]` lines are part of the gate. In a deterministic PC run
the expected values are:

| checkpoint | resident |
|---|---:|
| boot | 59 327 |
| scene start | 509 602 |
| scene end floor | 460 262 |
| all-time peak | 1 117 256 |

The scene-end floor must return to the same value after every scene
(`deltaVsCheckpoint=0`), which proves there is no per-scene leak. On ESP32 the
same run is expected to be about 13.7 KB lower because its bookkeeping
structs are smaller.

## Device diagnostics

The ESP32 project has additional build-time diagnostics:

| switch | effect |
|---|---|
| `JC_BENCH_AT_BOOT` (main.c) | run the compose benchmark before the story, then restart |
| `JC_STORY_DEADLINE_S` (main.c) | end the story after N seconds, print the closing reports and restart; `0` plays forever |
| `JC_PANEL_CALIB` (build define) | cycle solid colours with normal and byte-swapped pixels to identify panel channel order |
| `JC_REPLAY_FRAMES` (set by main/CMakeLists.txt when `replay_frames.h` exists) | replay captured PC frames cyclically instead of running the story |

The device prints a heap trajectory every 5 seconds and a panel push-fps
measurement at boot.
