# Memory instrumentation

The `-DMEMTRACK` build measures how much memory the engine keeps resident and
how that splits between data that is always present and transient, per-scene
data. This is used to keep the engine within the ESP32's memory budget.

## What is measured

Every allocation that goes through `safe_malloc()` is tracked. `safe_malloc()`
stores the block size in a 16-byte header in front of each block, so
`safe_free()` can subtract it again. The counters therefore report *true
resident bytes*, not a cumulative allocation counter.

Buffers allocated by SDL itself are not visible to this machinery. The only
such buffers in the engine are the WAVs loaded with `SDL_LoadWAV()`; those are
registered manually through `memTrackExternalAdd()` in `sound.c`.

Three kinds of reports are printed (all prefixed with `[MEM]`):

- **Resources**: one line per resource when its data is made resident — at
  boot in the eager regime, or on first acquisition in the lazy regime. Each
  line shows the compressed and uncompressed sizes and the running
  resident/peak totals.
- **Scenes**: every call to `adsPlay()` starts a checkpoint. At scene
  start/end, the currently resident amount, the delta since the checkpoint and
  the all-time peak are shown. The delta tells how much *transient* memory a
  scene needs on top of what is always resident.
- **Exit**: after `storyPlay()` returns, a run summary with the number of
  allocations, frees, registered SDL buffers, resident bytes at exit and the
  overall peak is printed.

## How to build and run

From the project root:

```
    make memtrack          # clean rebuild with -DMEMTRACK
```

Equivalently, `make MEMTRACK=1` after a `make clean`. A normal `make` build is
unaffected; the instrumentation compiles to no-ops.

At runtime, instrumentation output is enabled only when the `memstats`
argument is present:

```
    ./jc_reborn memstats                     # full screensaver run
    ./jc_reborn window memstats              # windowed run
    ./jc_reborn memstats dump                # just load + dump resources
    SDL_VIDEODRIVER=dummy ./jc_reborn window nosound memstats   # headless
```

Run it redirected or through `tee` to keep a log:

```
    ./jc_reborn memstats 2>&1 | tee run.log
    grep '\[MEM\]' run.log
```

## Example output

```
[MEM] res JOFFICE.SCR    compressed=5015      uncompressed=112000    resident=189967 peak=189967
...
[MEM] boot: resident=59327  deltaVsCheckpoint=0  peak=59327
[MEM] scene start: resident=509602  deltaVsCheckpoint=0  peak=509602
[MEM] grLoadScreen(INTRO.SCR): 640x480 (bg surface needs 153600 bytes @4bpp/2px-per-byte)
[MEM] scene end: resident=460262  deltaVsCheckpoint=-49340  peak=1117256
[MEM] wav sound0.wav: length=10768 (SDL-allocated buffer)
```

With lazy loading, boot stays at ~59 KB (headers only). During playback the
resident floor is ~460 KB, with an all-time transient peak around 1.1 MB. See
`RESMGR.md` for the resource-manager side of these numbers.

## Known limitations

- SDL-internal allocations other than the registered WAVs (for example the
  window surface internals, or the temporary overlay used by `grFadeOut()`)
  are not counted, so real process memory is a little higher than the
  counters.
- `safe_free()` must be used everywhere the engine frees a `safe_malloc()`
  buffer. Every `free()` call in the engine follows this rule; keep it that
  way.
- The instrumentation is compiled in only with `-DMEMTRACK`; a normal `make`
  build compiles the calls down to no-ops. The `memstats` argument is then
  accepted and ignored.
