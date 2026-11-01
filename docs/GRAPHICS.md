# Graphics pipeline

The engine composes every frame in a 640x480 coordinate space in
4-bit packed palette-index surfaces, then decimates once to the 320x240 panel
at presentation time.

`graphics.c` is the platform-independent engine half. `graphics_pc.c` is the
SDL window half; the ESP32 half is `esp32/main/platform_glue.c`.

## Surfaces

A `TGrSurface` (see `graphics.h`) stores:

- `width`, `height` — geometry in 640-space units
- `pitch` — packed bytes per row, `(width + 1) / 2`
- `pixels` — 4-bit palette indices, two horizontally adjacent pixels per
  byte, high nibble = left pixel
- `clipX1`, `clipY1`, `clipX2`, `clipY2` — clip zone, exclusive right/bottom

Rows are byte-packed. Sprite widths are always even in the source data, so a
row never straddles bytes. Coordinates are 1:1 with TTM/ADS 640-space
coordinates; decimation happens only during presentation.

Storage stays at full resolution because odd blit offsets exist. A later blit
can reference a source column that point-sampling at load time would have
discarded.

The compose canvas, background surface, TTM layers and saved-zones layer are
all `TGrSurface`. Memory per 640x480 surface is 153.6 KB.

## Palette and transparency

`grLoadPalette()` builds derived tables from the current palette:

- `grAvgSrc[16][3]` — palette index to 8-bit RGB, used by the 2x2 box
  decimation.
- `grTransparent[16]` — an index is transparent when its palette entry
  renders magenta, RGB `(0xa8, 0, 0xa8)`.
- `grMagentaFill` — an index rendering magenta, used to fill fresh layers.
  If the palette has no magenta, index 0 is used.
- `grBlackFill` — an index rendering black, used for the initial content of
  the compose canvas and the empty background. If the palette has no black,
  index 0 is used.
- `grBlitByteMode[256]` — classifies a packed source byte for the blit fast
  path: 0 = both pixels opaque, 1 = both transparent, 2 = mixed.
- `grPairSum[256][3]` — per-channel sum of the two palette entries in a
  packed byte, used by the fused ESP32 presentation.

`grWinColorDirty` is set by `grLoadPalette()` and observed by the PC push when
it needs to re-map the palette into the window surface format.

## Drawing primitives

- Rect fills and blits honour the clip zone.
- Direct pixel writes (pixels, lines, circles) honour only surface bounds.
- `grBlit()` skips pixels whose palette entry is transparent, and supports an
  optional horizontal flip.
- `grBlit()` has a byte-wise fast path for unflipped, nibble-aligned blits on
  distinct surfaces. It copies, skips or read-modify-writes whole packed
  bytes, which is exact with respect to the per-pixel loop. Flip,
  cross-parity and aliased-surface cases use the per-pixel loop.
- `grCopyZoneToBg()` blits `width + 2` pixels. The extra two columns
  compensate for an off-by-two in `GJIVS6.TTM`'s `SAVE_IMAGE` coordinates.
- `grSaveZone()` does not save per-zone state; `grRestoreZone()` frees the
  whole saved-zones layer. No TTM in the shipped corpus restores one zone
  while another is saved.
- `grFadeOut()` cycles through five fade types and presents each step.

## Presentation

`grPresentFrame()` decimates the compose canvas 2:1 into the panel and hands
it to the platform.

The decimation is a 2x2 **box average** of four adjacent compose pixels. A
point sample is phase-sensitive on the art's 1-pixel checker dithers: it
collapses them onto one palette colour instead of the blended colour the eye
perceives. The box average is phase-insensitive and reproduces the perceived
colour at panel resolution.

On PC, `grDecimateToPanel()` fills the canonical `grPanel` buffer (RGB888,
320x240, 230 400 B) and calls `grPushFrame()`. `grFramePresent()` then appends
the frame to `frames.bin` when `JC_FRAMEDUMP` is set.

`grRgb888To565()` is the single RGB888 to RGB565 conversion shared by the
frames.bin reference and the ESP32 SPI push:

```
((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
```

`frames.bin` is therefore the wire content by construction.

On ESP32, `grPushFrame565Buf()` returns the platform's RGB565 SPI staging
buffer. When it is non-NULL, `grPresentFrame()` decimates straight into that
buffer with `grPairSum` and calls `grPushFrame565Present()`. This skips the
intermediate RGB888 panel and the separate conversion pass. The first frame of
each boot runs a self-test that compares the fused path against the canonical
`grPanel` path and prints the number of mismatched pixels.

The PC `grPushFrame()` maps `grPanel` into the SDL window, replicating each
panel pixel 2x horizontally and vertically, updates the window surface, and
optionally dumps the window as a PPM (`JC_WINDUMP`). The ESP32
`grPushFrame()` line-converts RGB888 to RGB565 and performs the blocking SPI
draw.

## Initial state

The compose canvas and the empty background are filled with `grBlackFill`, so
they start as literal black. Fresh TTM layers are filled with `grMagentaFill`,
so every pixel starts transparent.

## Verification

The frame-equivalence harness and its environment variables are documented in
`TESTING.md`. In short: `JC_FRAMEDUMP` writes the presented panel as RGB565
`frames.bin`, `JC_WINDUMP` writes the SDL window as PPM, `JC_FADETEST` forces
all fade types, and `JC_BENCHFIX` makes benchmark phase boundaries
index-aligned.
