# Resource manager

The resource manager owns the boot-time resource index and the lifetime of
uncompressed resource data. It replaces direct access to the resource structs
with an acquire/release API, so data can be loaded on demand, released when a
consumer is done with it, and optionally cached.

The code lives in `resmgr.h` / `resmgr.c`. Resource parsing and the typed
resource structs live in `resource.h` / `resource.c`.

## Boot and index

`resmgrInit(mapFileName, loadEagerly)` performs two steps:

1. Parse `RESOURCE.MAP`: the resource-file name, the entry count, and each
   entry's offset and length.
2. Scan the main resource file. For every entry, read the resource name and
   size, determine its type from the extension, and parse its header.

The resulting index (`TResIndexEntry`) is small and stays resident for the
whole run. It holds the resource name, type, file offset, advertised size, the
absolute offset of the compressed blob, a pointer to the parsed resource
struct, and the data-management fields (`dataResident`, `pinCount`, `lastUse`,
`dataBytes`).

Two boot regimes are supported through `loadEagerly`:

- **Lazy** (`loadEagerly = 0`): only headers are parsed at boot. Data is
  loaded on first acquisition. Both entry points use this regime.
- **Eager** (`loadEagerly = 1`): every resource's data is loaded during the
  boot scan. This keeps the pre-resource-manager behaviour available for
  diagnostics.

The typed resource arrays (`adsResources[]`, `bmpResources[]`, ...) are
populated during the scan, because `dump.c` and `graphics.c` still access
resources through them.

## Header/data split

Every resource parser has two stages:

- `parseXxxHeader()` reads the header fields and skips the compressed blob
  with `fseek()`. The absolute blob position is stored in `blobOffset`;
  `uncompressedData` remains `NULL`.
- `loadXxxData()` seeks to that blob and decompresses it. The decompressor
  streams directly from the `FILE*`, so the compressed blob is never resident
  as a whole.

`resParseHeader()`, `resLoadData()`, `resDataSizes()` and
`resReleaseUncompressed()` dispatch to the five resource types (ADS, BMP, PAL,
SCR, TTM) through a single index entry. The `find*Resource()` helpers are
thin wrappers over `resmgrFind()`.

## Acquire/release lifecycle

`resmgrAcquireNamed(name)` looks up an entry and makes its data resident.
`resmgrAcquireData(entry)` pins it: `pinCount` is incremented, and the data
cannot be freed or evicted while the count is non-zero.
`resmgrReleaseData(entry)` decrements the count; when it reaches zero the data
is either freed immediately or retained according to the policy below.

Acquisition always goes through `resmgrAcquireData()`, including when the
data is already resident, so every acquire pins and every release balances.
This matters under LRU retention: a consumer walking a resource must never
have it evicted underneath.

Consumer lifetimes:

- `grLoadScreen()` (SCR): streams the background directly into the surface.
  The fallback path acquires the data, copies it, and releases it.
- `grLoadBmp()` (BMP): acquires the data, converts it into per-slot sprite
  surfaces, then releases it. This covers `island.c` and `bench.c`, which
  only call these loaders.
- `ttmLoadTtm()` (TTM): ownership transfer. The slot's `data` pointer is the
  acquired resource data. `TTtmSlot` records the owning index entry
  (`resIdxEntry`) and, when a slot is re-loaded while an old generation could
  still be walked, the previous owner (`resIdxEntryOld`). The old generation
  is released by `ttmResetSlot()` rather than immediately.
- `adsPlay()` (ADS): acquires the script at scene start. The interpreter
  walks it for the whole scene; it is released after all TTM slots are reset,
  just before the `scene end` report.
- `dump.c`: acquires and releases each resource around its dump.
- `sound.c` WAVs: loaded lazily on first play and cached until `soundEnd()`.
  Missing files are remembered in `soundProbed[]` and never re-probed.

## Retention policies

`resmgrReleaseData()` marks the end of a consumer's use. What happens next is
selected with `resmgrSetRetentionPolicy(policy, maxRetainedBytes,
maxRetainedEntries)`:

- `RESMGR_RETAIN_OFF` (default, PC): free the data immediately. This is the
  plain lazy regime.
- `RESMGR_RETAIN_LRU`: keep released data resident as a cache, bounded by a
  byte budget and an entry cap (`0` = no entry cap). On the next acquire of a
  retained resource, the data is re-pinned without touching the file. When a
  release pushes the retained total over budget, the least-recently-used
  entries are evicted: their data is freed and the parsed header is kept, so
  the entry remains indexable and becomes a cold load next time.

Pinned entries are never eviction candidates. A resource whose data alone
exceeds the whole budget is freed immediately instead of being retained.

The PC command line enables LRU with `lru` (1 MiB default budget) or
`lru=<bytes>`. The ESP32 build leaves retention off.

`resmgrTrimCache()` enforces the budget and can also be called directly.
`resmgrCacheReport()` prints a one-line `hits= evictions= retained=`
summary after a run (memstats builds only).

## Streaming SCR decode

SCR backgrounds are decompressed straight out of the resource file into the
background surface's pixel buffer. The uncompressed SCR bitmap is already the
engine's 4-bit packed surface format, so the decode is a verbatim
decompression into place.

Streaming consumers bypass the acquire/release/retention machinery entirely:
they never make `uncompressedData` resident, so there is nothing to release
or retain. `resmgrOpenMainFile()` opens the resource file; the caller seeks to
`idxEntry->blobOffset`, calls `uncompressIntoBuf()`, and closes the handle.

This keeps the 112–153.6 KB SCR blob out of RAM at all times. As a
consequence, SCRs are never retained by the LRU cache.

## Resource facts used by the loaders

- SCR backgrounds are 640x480 or 640x350, stored as 4-bit packed pixels. The
  uncompressed size equals `(width + 1) / 2 * height` for every shipped SCR.
- BMP images are stored sequentially. One running pointer walks the
  uncompressed blob across all images; it is not reset between images.
- Sprite widths are always even; heights may be odd.
- ADS scripts are at most ~12 KB; TTM bytecode at most ~30 KB.
- Palette index 0 renders magenta `#a800a8`; index 5 renders black. See
  `GRAPHICS.md` for the transparency rules derived from that.

## Memory profile

Measured with the memstats build, lazy regime, deterministic story run:

| item | size |
|---|---:|
| boot (resource index + headers) | 59 327 B |
| compose canvas (640x480 4-bit) | 153 608 B |
| background surface (640x480 4-bit) | 153 608 B |
| scene floor (resident at every scene end) | 460 262 B |
| scene start checkpoint (floor + scene-invariant sprites) | 509 602 B |
| per-scene transient (end vs start delta) | −49 340 B |
| all-time peak (BMP-driven scene sets) | 1 117 256 B |

The floor is `boot + compose + background + scene-invariant sprites`; it
returns to exactly the same value after every scene, so there is no
accumulation across scene cycles.

On the ESP32 the same resource data produces the same per-resource sizes. The
device's bookkeeping structs are smaller (ILP32 vs LP64), so its resident
totals are about 13.7 KB lower.
