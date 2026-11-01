/*
 *  This file is part of 'Johnny Reborn'
 *
 *  An open-source engine for the classic
 *  'Johnny Castaway' screensaver by Sierra.
 *
 *  Copyright (C) 2019 Jeremie GUILLAUME
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __XTENSA__   // ESP32 IDF build (see esp32/sdkconfig.defaults)
#include <esp_attr.h>   // EXT_RAM_BSS_ATTR (external-RAM BSS section)
#endif

#include "mytypes.h"
#include "utils.h"
#include "graphics.h"
#include "resource.h"
#include "resmgr.h"
#include "uncompress.h"
#include "events.h"

static uint8 ttmPalette[16][4];

// Derived tables, rebuilt by grLoadPalette():
// - grAvgSrc[i][3]:   palette index -> 8-bit R,G,B (the 6-bit VGA entries
//                     scaled <<2); the 2x2 box decimation averages these.
// - grTransparent[i]: "this palette entry renders magenta" — the indexed
//   equivalent of the SDL colour key on RGB (0xa8, 0, 0xa8)
// - grMagentaFill:    an index rendering magenta, for layer fills (0xFF if
//                     the palette has none)
// - grBlackFill:      an index rendering literal black, for the initial
//                     content of the compose canvas and of the empty
//                     background
static uint8  grAvgSrc[16][3];
static uint8  grTransparent[16];
// Byte-mode LUT for the aligned grBlit fast path, rebuilt by
// grLoadPalette() from grTransparent[]: for a packed byte holding the
// pixel pair (high nibble, low nibble) — 0 = both pixels opaque (copy
// the byte verbatim), 1 = both transparent (skip the byte), 2 = mixed
// (one read-modify-write, preserving the transparent side's nibble).
static uint8  grBlitByteMode[256];
static uint8  grMagentaFill = 0;
static uint8  grBlackFill = 0;
// Palette-reload flag consumed by the PC push (graphics_pc.c); exposed
// as a global because palette loads are an engine event and the
// platform push only observes it (see graphics.h).
int grWinColorDirty = 1;

// The composition canvas: 640x480 in the engine's 4-bit packed format
// (two horizontally adjacent palette indices per byte, high nibble =
// left pixel). Everything composes here. grPresentFrame() is the only
// code that converts palette->RGB, and it decimates 2:1 on the way out.
static struct TGrSurface *grComposeSfc = NULL;

static struct TGrSurface *grSavedZonesLayer = NULL;

struct TGrSurface *grBackgroundSfc = NULL;

int grDx = 0;
int grDy = 0;
int grWindowed = 0;
int grUpdateDelay = 0;


//--------------------------------------------------------------------
//   Optional coordinate-parity audit.
//
//   Build with -DGR_ODDAUDIT to record, per coordinate-taking primitive
//   call, whether any parameter sits on an odd 640-space coordinate or
//   has an odd size. Counters per (primitive, parity-bitmask) aggregate
//   a whole run; an exit-time summary goes to stderr.
//--------------------------------------------------------------------
#ifdef GR_ODDAUDIT
static long grAudBlit[16];      // bitmask: 1=odd dx, 2=odd dy, 4=odd src rect, 8=flip
static long grAudBlitTotal = 0;
static long grAudFill[16];      // fill rects / draw-rect: 1=odd x, 2=odd y, 4=odd w, 8=odd h
static long grAudClip[16];      // clip zones: 1=odd x1, 2=odd y1, 4=odd x2, 8=odd y2
static long grAudPxOddX = 0;    // direct pixel writes onto odd x
static long grAudPxOddY = 0;    // direct pixel writes onto odd y
static long grAudPxTotal = 0;
static long grAudLineOdd = 0;   // DRAW_LINE with >=1 odd endpoint
static long grAudLineTotal = 0;
static long grAudCircOdd = 0;   // DRAW_CIRCLE at odd anchor
static long grAudCircTotal = 0;
static long grAudZone = 0;      // COPY_ZONE_TO_BG at odd x/y
static long grAudZoneTotal = 0;

static void grAuditSummary(void)
{
    long fillTotal = 0, clipTotal = 0;
    for (int i=0; i<16; i++) fillTotal += grAudFill[i];
    for (int i=0; i<16; i++) clipTotal += grAudClip[i];

    fprintf(stderr, "[ODDAUDIT] blit total=%ld odd(1:dx 2:dy 4:src-size 8:flip):"
                    " 1=%ld 2=%ld 3=%ld 4=%ld 5=%ld 6=%ld 7=%ld 8=%ld"
                    " 9=%ld 10=%ld 11=%ld 12=%ld 13=%ld 14=%ld 15=%ld\n",
            grAudBlitTotal, grAudBlit[1], grAudBlit[2], grAudBlit[3],
            grAudBlit[4], grAudBlit[5], grAudBlit[6], grAudBlit[7],
            grAudBlit[8], grAudBlit[9], grAudBlit[10], grAudBlit[11],
            grAudBlit[12], grAudBlit[13], grAudBlit[14], grAudBlit[15]);
    fprintf(stderr, "[ODDAUDIT] even-blit counts: mask0=%ld (only odd height) mask4=%ld mask12=%ld\n",
            grAudBlit[0], grAudBlit[4], grAudBlit[12]);
    fprintf(stderr, "[ODDAUDIT] fill total=%ld (1=x 2=y 4=w 8=h):"
                    " 1=%ld 2=%ld 3=%ld 4=%ld 5=%ld 6=%ld 7=%ld"
                    " 8=%ld 9=%ld 10=%ld 11=%ld 12=%ld 13=%ld 14=%ld 15=%ld\n",
            fillTotal, grAudFill[1], grAudFill[2], grAudFill[3], grAudFill[4],
            grAudFill[5], grAudFill[6], grAudFill[7], grAudFill[8],
            grAudFill[9], grAudFill[10], grAudFill[11], grAudFill[12],
            grAudFill[13], grAudFill[14], grAudFill[15]);
    fprintf(stderr, "[ODDAUDIT] clip total=%ld (1=x1 2=y1 4=x2 8=y2):"
                    " 1=%ld 2=%ld 3=%ld 4=%ld 5=%ld 6=%ld 7=%ld"
                    " 8=%ld 9=%ld 10=%ld 11=%ld 12=%ld 13=%ld 14=%ld 15=%ld\n",
            clipTotal, grAudClip[1], grAudClip[2], grAudClip[3], grAudClip[4],
            grAudClip[5], grAudClip[6], grAudClip[7], grAudClip[8],
            grAudClip[9], grAudClip[10], grAudClip[11], grAudClip[12],
            grAudClip[13], grAudClip[14], grAudClip[15]);
    fprintf(stderr, "[ODDAUDIT] pixels total=%ld oddX=%ld oddY=%ld\n",
            grAudPxTotal, grAudPxOddX, grAudPxOddY);
    fprintf(stderr, "[ODDAUDIT] lines total=%ld odd-endpoint=%ld"
                    " circles total=%ld odd-anchor=%ld"
                    " zone-copies total=%ld odd=%ld\n",
            grAudLineTotal, grAudLineOdd, grAudCircTotal, grAudCircOdd,
            grAudZoneTotal, grAudZone);
}

static void grAuditInit(void)
{
    atexit(grAuditSummary);
} 
#endif


//--------------------------------------------------------------------
//   Verification harness (see docs/TESTING.md)
//
//   JC_FRAMEDUMP=<dir> : after every display update, append the composed
//     frame to <dir>/frames.bin as raw RGB565 (320x240x2 bytes), using the
//     same 2x2 box decimation as grPresentFrame(). Byte-identical runs
//     between two builds prove pixel-exact equivalence of composition and
//     presentation-through-the-panel. These frames are the box-decimated
//     panel, not a point-sampled downsample: dithered areas render as their
//     blended colour.
//   JC_FADETEST : grFadeOut() is force-called every 100 display updates so
//     all five fade types are exercised (and dumped) deterministically.
//     JC_FRAMEDUMP also pins srand so the island's random OCEAN SCR choice
//     is reproducible.
//   JC_WINDUMP=<dir> : additionally dump the SDL window surface after
//     every display update as <dir>/winNNNNN-<W>x<H>-<bpp>ppm, to check
//     the PC-side presentation path.
//--------------------------------------------------------------------
static FILE *frameDumpFp = NULL;
static int   frameCounter = 0;
static int   fadeTest = -1;

//   The presented panel: the canonical RGB888 frame the engine-side
//   presentation fills from the compose canvas (the 2x2 box decimation
//   output, 320x240). Everything downstream consumes this one buffer:
//   - the PC push (graphics_pc.c grPushFrame) maps it into the SDL window,
//   - the frames.bin dump (grDumpFrameRef) derives the RGB565 wire
//     content from it with the same conversion the ESP32 SPI push uses, and
//   - the ESP32 push line-converts RGB888 -> RGB565 the same way before
//     the SPI DMA transfer.
//   It sits in .bss on PC; on ESP32 it lives in PSRAM (230 400 B).
#ifdef __XTENSA__   // ESP32 IDF build (see esp32/sdkconfig.defaults)
static EXT_RAM_BSS_ATTR uint8 grPanel[GR_FRAME_W * GR_FRAME_H * 3];
#else
static uint8 grPanel[GR_FRAME_W * GR_FRAME_H * 3];
#endif

// Byte-pair LUT for the decimation's 2x2 box average: for a packed byte
// holding pixels (high nibble, low nibble), the per-channel sum of their
// grAvgSrc entries. Two LUT loads replace four. The entries are uint16
// because grAvgSrc values reach 252, so a pair sum reaches 504; a uint8
// LUT would wrap modulo 256. Rebuilt by grLoadPalette().
static uint16 grPairSum[256][3];

static void grAvgIndices(uint8 i0, uint8 i1, uint8 i2, uint8 i3, uint8 out[3])
{
    for (int c=0; c < 3; c++)
        out[c] = (uint8)((grAvgSrc[i0][c] + grAvgSrc[i1][c]
                        + grAvgSrc[i2][c] + grAvgSrc[i3][c]) >> 2);
}


static void grDumpFrameRef()
{
    const char *dirFmt = getenv("JC_FRAMEDUMP");

    if (dirFmt == NULL)
        return;

    if (frameDumpFp == NULL) {
        char path[512];
        snprintf(path, sizeof(path), "%s/frames.bin", dirFmt);
        frameDumpFp = fopen(path, "wb");

        if (frameDumpFp == NULL)
            fprintf(stderr, "Warning: JC_FRAMEDUMP: can't create %s "
                            "(bad directory?) - frame dump disabled\n", path);
    }

    if (frameDumpFp != NULL) {
        for (int dy=0; dy < GR_FRAME_H; dy++) {

        const uint8 *row = grPanel + dy * GR_FRAME_W * 3;

        for (int dx=0; dx < GR_FRAME_W; dx++) {
            const uint8 *px = row + dx * 3;

            uint16 rgb565 = grRgb888To565(px);
            fwrite(&rgb565, 2, 1, frameDumpFp);
        }
        }
    }
}


// The grDumpWindow() diagnostic (the SDL-window PPM writer) lives in
// graphics_pc.c: it reads the platform window surface, so it is a PC-side
// concern. The ESP32 push, present_panel_rgb565(), plays its role: the panel
// content is the presentation reference.


static void grSetFullClip(struct TGrSurface *sfc)
{
    sfc->clipX1 = 0;
    sfc->clipY1 = 0;
    sfc->clipX2 = 32767;
    sfc->clipY2 = 32767;
}


static void grFillSurface(struct TGrSurface *sfc, uint8 index)
{
    memset(sfc->pixels, (index << 4) | (index & 0x0f),
           (size_t) sfc->pitch * sfc->height);
}


static struct TGrSurface *grAllocSurface(uint16 w, uint16 h)
{
    struct TGrSurface *sfc = safe_malloc(sizeof(struct TGrSurface));

    sfc->width  = w;
    sfc->height = h;
    sfc->pitch  = (uint16)((w + 1) / 2);
    sfc->pixels = safe_malloc((size_t) sfc->pitch * sfc->height);

    grSetFullClip(sfc);

    return sfc;
}


static void grFreeSurface(struct TGrSurface *sfc)
{
    if (sfc == NULL)
        return;

    safe_free(sfc->pixels);
    safe_free(sfc);
}


//------------------------------------------------------------
//   Pixel access on a 4-bit packed surface (1:1 coordinates)
//------------------------------------------------------------

static uint8 grGetPixel(struct TGrSurface *sfc, sint32 x, sint32 y)
{
    if ((uint32) y < (uint32) sfc->height && (uint32) x < (uint32) sfc->width) {
        uint8 byte = sfc->pixels[y * sfc->pitch + (x >> 1)];
        return (x & 1) ? (byte & 0x0f) : (byte >> 4);
    }

    return 0;   // outside the surface: black
}

static void grRawPutPixel(struct TGrSurface *sfc, sint32 x, sint32 y, uint8 color)
{
    uint8 *byte = &sfc->pixels[y * sfc->pitch + (x >> 1)];

    if (x & 1)
        *byte = (*byte & 0xf0) | (color & 0x0f);
    else
        *byte = (*byte & 0x0f) | (uint8)(color << 4);
}

static void grPutPixel(struct TGrSurface *sfc, sint32 x, sint32 y, uint8 color)
{
    // Direct pixel writes honour the surface bounds but not the clip
    // zone; only fills and blits clip.

#ifdef GR_ODDAUDIT
    grAudPxTotal++;
    if (x & 1) grAudPxOddX++;
    if (y & 1) grAudPxOddY++;
#endif

    if ((uint32) x < (uint32) sfc->width && (uint32) y < (uint32) sfc->height)
        grRawPutPixel(sfc, x, y, color);
}


static void grDrawHorizontalLine(struct TGrSurface *sfc, sint32 x1, sint32 x2, sint32 y, uint8 color)
{
    if ((uint32) y >= (uint32) sfc->height)
        return;

    x1 = x1 < 0          ? 0          : x1;
    x2 = x2 > (sfc->width - 1) ? (sfc->width - 1) : x2;

    for (sint32 x=x1; x<=x2; x++)
        grPutPixel(sfc, x, y, color);
}


// Rectangle fill honouring the clip zone (SDL_FillRect semantics).
static void grFillRect(struct TGrSurface *sfc, sint32 x, sint32 y,
                       sint32 width, sint32 height, uint8 color)
{
#ifdef GR_ODDAUDIT
    grAudFill[(x & 1) | ((y & 1) << 1) | ((width & 1) << 2) | ((height & 1) << 3)]++;
#endif

    sint32 x1 = x,  y1 = y;
    sint32 x2 = x + width, y2 = y + height;   // exclusive

    // intersect with clip zone ...
    if (x1 < sfc->clipX1) x1 = sfc->clipX1;
    if (y1 < sfc->clipY1) y1 = sfc->clipY1;
    if (x2 > sfc->clipX2) x2 = sfc->clipX2;
    if (y2 > sfc->clipY2) y2 = sfc->clipY2;

    // ... and with the surface itself
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > sfc->width)  x2 = sfc->width;
    if (y2 > sfc->height) y2 = sfc->height;

    if (x1 >= x2 || y1 >= y2)
        return;

    uint8 fillByte = (uint8)((color << 4) | (color & 0x0f));

    for (sint32 ry = y1; ry < y2; ry++) {

        uint8 *row = sfc->pixels + (ry * sfc->pitch);

        for (sint32 k = (x1 >> 1); k <= ((x2 - 1) >> 1); k++) {

            int loUsed = x1 <= (2*k + 1) && (2*k + 1) < x2;
            int hiUsed = x1 <= (2*k)     && (2*k)     < x2;

            if (hiUsed && loUsed)
                row[k] = fillByte;
            else if (hiUsed)
                row[k] = (uint8)((row[k] & 0x0f) | (uint8)(color << 4));
            else
                row[k] = (uint8)((row[k] & 0xf0) | (color & 0x0f));
        }
    }
}


// Generic blit.
//
// Copies the source region starting at (sx, sy) sized (sw, sh) to
// destination position (dx, dy), honouring the destination surface's
// clip zone, skipping pixels whose palette entry is transparent when
// 'transparent' is set, with an optional horizontal flip (destination x
// for source column i is dx + (sw - 1 - i)).
static void grBlit(struct TGrSurface *dst, struct TGrSurface *src,
                   sint32 sx, sint32 sy, sint32 sw, sint32 sh,
                   sint32 dx, sint32 dy,
                   int flip, int transparent)
{
#ifdef GR_ODDAUDIT
    {
        int mask = (dx & 1) | ((dy & 1) << 1)
                 | (((sx & 1) | (sy & 1) | (sw & 1) | (sh & 1)) << 2)
                 | (flip << 3);
        grAudBlit[mask]++;
        grAudBlitTotal++;
    }
#endif
    // Fast path: unflipped, nibble-aligned blits (sx/dx same parity) on
    // distinct surfaces process whole packed bytes instead of doing two
    // read-modify-writes per destination byte. Byte-exact with the
    // per-pixel loop below (the same bounds/clip predicates select the
    // same pixel set; a packed byte is written verbatim, skipped, or
    // read-modify-written once, exactly as its two pixels would be).
    // This is the hot path of grUpdateDisplay() (full-screen background
    // and ttm layer blits at (0,0), 640 px wide) — measured on the
    // ESP32: the per-pixel loop costs ~65 ms per full-screen blit, the
    // byte path ~10 ms.
    if (!flip && ((sx ^ dx) & 1) == 0 && src != dst) {

        // Row-valid i range: iStart..iEnd (exclusive) in pixels. The
        // per-pixel loop's srcX/dstX/clip checks collapse to run ends
        // because dstX - srcX is constant (no flip).
        sint32 iStart = 0;
        if (iStart < -sx) iStart = -sx;
        if (iStart < -dx) iStart = -dx;
        if (iStart < dst->clipX1 - dx) iStart = dst->clipX1 - dx;
        sint32 iEnd = sw;
        if (iEnd > src->width - sx) iEnd = src->width - sx;
        if (iEnd > dst->width - dx) iEnd = dst->width - dx;
        if (iEnd > dst->clipX2 - dx) iEnd = dst->clipX2 - dx;
        int byteMode = transparent ? 2 : 0;   // 2 = LUT, 0 = verbatim

        for (sint32 j=0; j < sh; j++) {

            sint32 srcY = sy + j;
            if (srcY < 0 || srcY >= src->height)
                continue;

            sint32 dstY = dy + j;
            if (dstY < 0 || dstY >= dst->height
                || dstY < dst->clipY1 || dstY >= dst->clipY2)
                continue;

            uint8 *srcRow = src->pixels + (size_t) srcY * src->pitch;
            uint8 *dstRow = dst->pixels + (size_t) dstY * dst->pitch;
            sint32 i = iStart;

            // Leading pixel on an odd nibble: single-pixel write.
            if (i < iEnd && ((sx + i) & 1)) {
                uint8 index = (srcRow[(sx + i) >> 1] & 0x0f);
                if (!transparent || !grTransparent[index])
                    dstRow[(dx + i) >> 1] =
                        (uint8) ((dstRow[(dx + i) >> 1] & 0xf0)
                                 | (index & 0x0f));
                i++;
            }

            // Whole-byte run: two pixels per byte, aligned on both
            // surfaces (parity checked above).
            while (i + 1 < iEnd) {
                uint8 *sb = &srcRow[(sx + i) >> 1];
                uint8 *db = &dstRow[(dx + i) >> 1];
                if (byteMode) {
                    switch (grBlitByteMode[*sb]) {
                    case 1:              // both pixels transparent
                        break;
                    case 0:              // both opaque: verbatim
                        *db = *sb;
                        break;
                    default: {           // mixed: one read-modify-write
                        uint8 v = *sb;
                        uint8 nb = *db;
                        if (grTransparent[v >> 4])
                            nb = (uint8) ((nb & 0xf0) | (v & 0x0f));
                        else
                            nb = (uint8) ((v & 0xf0) | (nb & 0x0f));
                        *db = nb;
                        break;
                    }
                    }
                } else {
                    *db = *sb;           // opaque blit: verbatim copy
                }
                i += 2;
            }

            // Trailing pixel on an even nibble (high nibble).
            if (i < iEnd) {
                uint8 index = (srcRow[(sx + i) >> 1] >> 4);
                if (!transparent || !grTransparent[index])
                    dstRow[(dx + i) >> 1] =
                        (uint8) ((dstRow[(dx + i) >> 1] & 0x0f)
                                 | (uint8) (index << 4));
            }
        }

        return;
    }

    for (sint32 j=0; j < sh; j++) {

        sint32 srcY = sy + j;
        if (srcY < 0 || srcY >= src->height)
            continue;

        sint32 dstY = dy + j;
        if (dstY < 0 || dstY >= dst->height
            || dstY < dst->clipY1 || dstY >= dst->clipY2)
            continue;

        for (sint32 i=0; i < sw; i++) {

            sint32 srcX = sx + i;
            if (srcX < 0 || srcX >= src->width)
                continue;

            sint32 dstX = flip ? dx + (sw - 1 - i) : dx + i;
            if (dstX < 0 || dstX >= dst->width
                || dstX < dst->clipX1 || dstX >= dst->clipX2)
                continue;

            uint8 index = grGetPixel(src, srcX, srcY);

            if (transparent && grTransparent[index])
                continue;

            grRawPutPixel(dst, dstX, dstY, index);
        }
    }
}



//--------------------------------------------------------------------
//   Presentation: decimate the 640x480 composition 2:1 with the 2x2
//   box average, fill the canonical panel (grPanel, above), hand it
//   to the platform push.
//--------------------------------------------------------------------
static void grDecimateToPanel()
{
    for (int sy=0; sy < GR_FRAME_H; sy++) {

        const uint8 *inRow0 = grComposeSfc->pixels + (2*sy)     * grComposeSfc->pitch;
        const uint8 *inRow1 = grComposeSfc->pixels + (2*sy + 1) * grComposeSfc->pitch;
        uint8 *out = grPanel + sy * GR_FRAME_W * 3;

        // One compose row is (2*GR_FRAME_W) pixels packed into GR_FRAME_W
        // bytes: byte b holds composed pixels x=2b (high nibble) and
        // x=2b+1 (low nibble). The 2:1 decimation is a 2x2 *box average*
        // of the compose pixels (2b, 2b+1) across rows (2sy, 2sy+1), so
        // dithered one-pixel shading blends into its mixed colour instead
        // of aliasing onto one side of the checker. On the ESP32 this is
        // the exact blend the SPI push shows on the 320x240 panel.
        for (int b=0; b < GR_FRAME_W; b++) {

            uint8 px[3];
            grAvgIndices(inRow0[b] >> 4, inRow0[b] & 0x0f,
                         inRow1[b] >> 4, inRow1[b] & 0x0f, px);

            out[0] = px[0];
            out[1] = px[1];
            out[2] = px[2];
            out += 3;
        }
    }
}

static void grPresentFrame()
{
#ifdef __XTENSA__
    // ESP32 fast present: decimate straight into the platform's RGB565
    // SPI staging buffer (see grPushFrame565Buf()). Same 2x2 box average
    // as the grPanel path (grPairSum keeps the exact per-channel
    // (a+b+c+d)>>2) and the same grRgb888To565() conversion as the glue;
    // the platform present applies the SPI byte order.
    uint16_t *dst565 = (uint16_t *) grPushFrame565Buf();
    if (dst565 != NULL) {
        for (int sy=0; sy < GR_FRAME_H; sy++) {

            const uint8 *inRow0 = grComposeSfc->pixels + (2*sy)     * grComposeSfc->pitch;
            const uint8 *inRow1 = grComposeSfc->pixels + (2*sy + 1) * grComposeSfc->pitch;
            uint16_t *out = dst565 + sy * GR_FRAME_W;

            for (int b=0; b < GR_FRAME_W; b++) {

                const uint16 *pa = grPairSum[inRow0[b]];
                const uint16 *pb = grPairSum[inRow1[b]];
                uint8 px[3];
                px[0] = (pa[0] + pb[0]) >> 2;
                px[1] = (pa[1] + pb[1]) >> 2;
                px[2] = (pa[2] + pb[2]) >> 2;

                out[b] = grRgb888To565(px);
            }
        }

        // Once-per-boot self-test: the PC frame gate cannot execute this
        // device-only path, so verify it against the canonical grPanel
        // path on real content. Costs one extra decimation at the first
        // frame.
        static int selfTestDone = 0;
        if (!selfTestDone) {
            long diffs = 0;
            selfTestDone = 1;
            grDecimateToPanel();
            for (int i=0; i < GR_FRAME_W * GR_FRAME_H; i++)
                if (grRgb888To565(grPanel + i * 3) != dst565[i])
                    diffs++;
            printf("[GR] fused-present self-test: %ld/%d pixel "
                   "mismatches vs grPanel\n",
                   diffs, GR_FRAME_W * GR_FRAME_H);
        }

        grPushFrame565Present();
        return;
    }
#endif

    grDecimateToPanel();
    grPushFrame(grPanel, GR_FRAME_W, GR_FRAME_H);
}


static void grFramePresent()
{
    grPresentFrame();          // compose -> grPanel + grPushFrame
    grDumpFrameRef();
}


static void grReleaseScreen()
{
    grFreeSurface(grBackgroundSfc);
    grBackgroundSfc = NULL;
}


static void grReleaseSavedLayer()
{
    grFreeSurface(grSavedZonesLayer);
    grSavedZonesLayer = NULL;
}


void grLoadPalette(struct TPalResource *palResource)
{
    if (palResource == NULL)
        fatalError("NULL palette\n");

    grMagentaFill = 0xFF;

    for (int i=0; i < 16; i++) {
        ttmPalette[i][0] = palResource->colors[i].b << 2;
        ttmPalette[i][1] = palResource->colors[i].g << 2;
        ttmPalette[i][2] = palResource->colors[i].r << 2;
        ttmPalette[i][3] = 0;

        grAvgSrc[i][0] = ttmPalette[i][2];   // R
        grAvgSrc[i][1] = ttmPalette[i][1];   // G
        grAvgSrc[i][2] = ttmPalette[i][0];   // B

        // A palette index is transparent exactly when its palette entry
        // renders RGB (0xa8, 0, 0xa8).
        grTransparent[i] = (ttmPalette[i][2] == 0xa8
                            && ttmPalette[i][1] == 0
                            && ttmPalette[i][0] == 0xa8);

        if (grTransparent[i] && grMagentaFill == 0xFF)
            grMagentaFill = (uint8) i;
    }

    if (grMagentaFill == 0xFF)
        grMagentaFill = 0;   // palette without magenta (see docs/GRAPHICS.md)

    // Byte-pair LUT for the decimation (see the table's comment).
    for (int v=0; v < 256; v++)
        for (int c=0; c < 3; c++)
            grPairSum[v][c] = (uint16) (grAvgSrc[v >> 4][c]
                                      + grAvgSrc[v & 0x0f][c]);

    // Byte-mode LUT for the aligned grBlit fast path (see the LUT's
    // comment at the tables): a packed byte skips/copies/needs a
    // single read-modify-write depending on its two palette entries.
    for (int v=0; v < 256; v++) {
        int tHi = grTransparent[v >> 4];
        int tLo = grTransparent[v & 0x0f];
        grBlitByteMode[v] = (uint8) ((tHi && tLo) ? 1 : (tHi || tLo) ? 2 : 0);
    }

    grBlackFill = 0xFF;
    for (int i=0; i < 16; i++)
        if (ttmPalette[i][2] == 0 && ttmPalette[i][1] == 0 && ttmPalette[i][0] == 0) {
            grBlackFill = (uint8) i;
            break;
        }
    if (grBlackFill == 0xFF)
        grBlackFill = 0;     // no black entry: literal black unrepresentable

    grWinColorDirty = 1;
}


void graphicsInit()
{
#ifdef GR_ODDAUDIT
    grAuditInit();
#endif

    // SDL window / presentation stage (platform hook, graphics.h):
    // PC — SDL_Init + the 640x480 window; ESP32 — no-op (the app has
    // already initialised the SPI panel).
    grWindowOpen();

    grLoadPalette(palResources[0]);

    // The composition canvas starts as black, expressed as the palette's
    // black index.
    grComposeSfc = grAllocSurface(640, 480);
    grFillSurface(grComposeSfc, grBlackFill);

    srand(time(NULL));
    if (getenv("JC_FRAMEDUMP") != NULL)
        srand(42);   // deterministic OCEAN SCR choice for island runs

    eventsInit();
}


void graphicsEnd()
{
    grWindowClose();
}


void grUpdateDisplay(struct TTtmThread *ttmBackgroundThread,
                     struct TTtmThread *ttmThreads,
                     struct TTtmThread *ttmHolidayThread)
{
    // Blit the background (no colour key: magenta pixels are real pixels)
    if (grBackgroundSfc != NULL)
        grBlit(grComposeSfc, grBackgroundSfc,
               0, 0, grBackgroundSfc->width, grBackgroundSfc->height,
               0, 0, 0, 0);

    // If not NULL, blit the optional layer of saved zones
    if (grSavedZonesLayer != NULL)
        grBlit(grComposeSfc, grSavedZonesLayer,
               0, 0, grSavedZonesLayer->width, grSavedZonesLayer->height,
               0, 0, 0, 1);

    // Blit successively each thread's layer
    for (int i=0; i < MAX_TTM_THREADS; i++)
        if (ttmThreads[i].isRunning)
            grBlit(grComposeSfc, ttmThreads[i].ttmLayer,
                   0, 0, ttmThreads[i].ttmLayer->width, ttmThreads[i].ttmLayer->height,
                   0, 0, 0, 1);

    // Finally, blit the holiday layer
    if (ttmHolidayThread != NULL)
        if (ttmHolidayThread->isRunning)
            grBlit(grComposeSfc, ttmHolidayThread->ttmLayer,
                   0, 0, ttmHolidayThread->ttmLayer->width, ttmHolidayThread->ttmLayer->height,
                   0, 0, 0, 1);

    // fade coverage harness (see the comment block at the top of the file)
    if (fadeTest < 0)
        fadeTest = (getenv("JC_FADETEST") != NULL);
    if (++frameCounter % 100 == 0 && fadeTest)
        grFadeOut();

    // Wait for the tick ...
    eventsWaitTick(grUpdateDelay);

    // ... and refresh the display
    grFramePresent();

}


struct TGrSurface *grNewLayer()
{
    struct TGrSurface *sfc = grAllocSurface(640, 480);

    // Fill with the transparent (magenta) index: every pixel of a fresh
    // layer is transparent.
    grFillSurface(sfc, grMagentaFill);

    return sfc;
}


void grFreeLayer(struct TGrSurface *sfc)
{
    grFreeSurface(sfc);
}


void grSetClipZone(struct TGrSurface *sfc, sint16 x1, sint16 y1, sint16 x2, sint16 y2)
{
#ifdef GR_ODDAUDIT
    grAudClip[(x1 & 1) | ((y1 & 1) << 1) | ((x2 & 1) << 2) | ((y2 & 1) << 3)]++;
#endif
    x1 += grDx; y1 += grDy;
    x2 += grDx; y2 += grDy;

    // SDL_SetClipRect() semantics: intersect the given rect with the
    // surface; an empty/invalid rect produces an empty clip zone
    sfc->clipX1 = x1 < 0 ? 0 : x1;
    sfc->clipY1 = y1 < 0 ? 0 : y1;
    sfc->clipX2 = x2 > sfc->width  ? sfc->width  : (x2 < x1 ? x1 : x2);
    sfc->clipY2 = y2 > sfc->height ? sfc->height : (y2 < y1 ? y1 : y2);

    if (sfc->clipX2 < sfc->clipX1) sfc->clipX2 = sfc->clipX1;
    if (sfc->clipY2 < sfc->clipY1) sfc->clipY2 = sfc->clipY1;
}


void grCopyZoneToBg(struct TGrSurface *sfc, uint16 x, uint16 y, uint16 width, uint16 height)
{
    x += grDx; y += grDy;

#ifdef GR_ODDAUDIT
    grAudZoneTotal++;
    if ((x & 1) || (y & 1) || (width & 1) || (height & 1))
        grAudZone++;
#endif

    if (grSavedZonesLayer == NULL)
        grSavedZonesLayer = grNewLayer();

    // Transparent pixels in the source (a thread layer) leave the saved
    // layer's own magenta filler in place. The +2 on the width covers a
    // coordinate error in GJIVS6.TTM that would otherwise leave two
    // unfilled pixels on the hull of the cargo.
    grBlit(grSavedZonesLayer, sfc, x, y, width + 2, height, x, y, 0, 1);
}


void grSaveImage1(struct TGrSurface *sfc, uint16 arg0, uint16 arg1, uint16 arg2, uint16 arg3)
{
//    ttmSetColors(4,4);
//    ttmDrawRect(arg0,arg1,arg2,arg3);
//    ttmSaveImage0(arg0,arg1,arg2,arg3);
//    ttmUpdate();
}


void grSaveZone(struct TGrSurface *sfc, uint16 x, uint16 y, uint16 width, uint16 height)
{
    // Minimalistic implementation: we don't really save the zone,
    // and let grRestoreZone() simply erase the 'saved zones' layer
}


void grRestoreZone(struct TGrSurface *sfc, uint16 x, uint16 y, uint16 width, uint16 height)
{
    // In Johnny's TTMs, we never have RESTORE_ZONE called
    // while several zones are saved. So we simply free the
    // whole saved zones layer
    grReleaseSavedLayer();
}


void grDrawPixel(struct TGrSurface *sfc, sint16 x, sint16 y, uint8 color)
{
    x += grDx; y += grDy;
    grPutPixel(sfc, x, y, color);
}


void grDrawLine(struct TGrSurface *sfc, sint16 x1, sint16 y1, sint16 x2, sint16 y2, uint8 color)
{
    x1 += grDx; y1 += grDy;
    x2 += grDx; y2 += grDy;

#ifdef GR_ODDAUDIT
    grAudLineTotal++;
    if ((x1 & 1) || (y1 & 1) || (x2 & 1) || (y2 & 1))
        grAudLineOdd++;
#endif

    // Bresenham's line drawing algorithm
    // Note : the code below intends to be pixel-perfect

    uint16 dx, dy, cumul, x, y;
    int xinc, yinc;

    x = x1;
    y = y1;
    dx = abs(x2 - x1);
    dy = abs(y2 - y1);

    xinc = (x2>x1 ? 1 : -1);
    yinc = (y2>y1 ? 1 : -1);

    if (dy < dx) {
        cumul = (dx + 1) >> 1;

        for (int i=0; i < dx; i++) {

            grPutPixel(sfc, x, y, color);

            x += xinc;
            cumul += dy;

            if (cumul > dx) {
                cumul -= dx;
                y += yinc;
            }
        }
    }
    else {
        cumul = (dy + 1) >> 1;

        for (int i=0; i < dy; i++) {

            grPutPixel(sfc, x, y, color);

            y += yinc;
            cumul += dx;

            if (cumul > dy) {
                cumul -= dy;
                x += xinc;
            }
        }
    }
}


void grDrawRect(struct TGrSurface *sfc, sint16 x, sint16 y, uint16 width, uint16 height, uint8 color)
{
    x += grDx; y += grDy;

    grFillRect(sfc, x, y, width, height, color);
}


void grDrawCircle(struct TGrSurface *sfc, sint16 x1, sint16 y1, uint16 width, uint16 height, uint8 fgColor, uint8 bgColor)
{
    x1 += grDx; y1 += grDy;

    // We can only draw regular circles
    if (width != height) {
        fprintf(stderr, "Warning : grDrawCircle() : unable to draw ellipse\n");
        return;
    }

#ifdef GR_ODDAUDIT
    grAudCircTotal++;
    if ((x1 & 1) || (y1 & 1))
        grAudCircOdd++;
#endif

    // Only even diameters are supported.
    if (width % 2) {
        fprintf(stderr, "Warning : grDrawCircle() : unable to process odd diameters\n");
        return;
    }

    // Bresenham's circle drawing algorithm
    // Note : the code below intends to be pixel-perfect

    uint16 r = (width >> 1) - 1;
    uint16 xc = x1 + r;
    uint16 yc = y1 + r;
    sint16 x = 0;
    sint16 y = r;
    int d = 1 - r;

    while (1) {

        grDrawHorizontalLine(sfc, xc-x, xc+x+1, yc+y+1, bgColor);
        grDrawHorizontalLine(sfc, xc-x, xc+x+1, yc-y  , bgColor);

        grDrawHorizontalLine(sfc, xc-y, xc+y+1, yc+x+1, bgColor);
        grDrawHorizontalLine(sfc, xc-y, xc+y+1, yc-x  , bgColor);

        if (y-x <= 1)
            break;

        if (d < 0)
            d += (x << 1) + 3;
        else {
            d += ((x - y) << 1) + 5;
            y--;
        }

        x++;
    }

    if (fgColor != bgColor) {

        x = 0;
        y = r;
        d = 1 - r;

        while (1) {

            grPutPixel(sfc, xc-x  , yc+y+1, fgColor);
            grPutPixel(sfc, xc+x+1, yc+y+1, fgColor);

            grPutPixel(sfc, xc-x  , yc-y  , fgColor);
            grPutPixel(sfc, xc+x+1, yc-y  , fgColor);

            grPutPixel(sfc, xc-y  , yc+x+1, fgColor);
            grPutPixel(sfc, xc+y+1, yc+x+1, fgColor);

            grPutPixel(sfc, xc-y  , yc-x  , fgColor);
            grPutPixel(sfc, xc+y+1, yc-x  , fgColor);

            if (y-x <= 1)
                break;

            if (d < 0)
                d += (x << 1) + 3;
            else {
                d += ((x - y) << 1) + 5;
                y--;
            }

            x++;
        }
    }
}


void grDrawSprite(struct TGrSurface *sfc, struct TTtmSlot *ttmSlot, sint16 x, sint16 y, uint16 spriteNo, uint16 imageNo)
{
    if (spriteNo >= ttmSlot->numSprites[imageNo]) {
        fprintf(stderr, "Warning : grDrawSprite(): less than %d sprites loaded in slot %d\n", imageNo, spriteNo);
        return;
    }

    x += grDx; y += grDy;

    struct TGrSurface *srcSfc = ttmSlot->sprites[imageNo][spriteNo];

    grBlit(sfc, srcSfc, 0, 0, srcSfc->width, srcSfc->height, x, y, 0, 1);
}


void grDrawSpriteFlip(struct TGrSurface *sfc, struct TTtmSlot *ttmSlot, sint16 x, sint16 y, uint16 spriteNo, uint16 imageNo)
{
    if (spriteNo >= ttmSlot->numSprites[imageNo]) {
        fprintf(stderr, "Warning : grDrawSpriteFlip(): less than %d sprites loaded in slot %d\n", imageNo, spriteNo);
        return;
    }

    x += grDx; y += grDy;

    struct TGrSurface *srcSfc = ttmSlot->sprites[imageNo][spriteNo];

    grBlit(sfc, srcSfc, 0, 0, srcSfc->width, srcSfc->height, x, y, 1, 1);
}


void grClearScreen(struct TGrSurface *sfc)
{
    // Disable the clip zone for the fill, then restore it.
    sint16 clipX1 = sfc->clipX1, clipY1 = sfc->clipY1;
    sint16 clipX2 = sfc->clipX2, clipY2 = sfc->clipY2;

    grSetFullClip(sfc);
    grFillRect(sfc, 0, 0, sfc->width, sfc->height, grMagentaFill);
    sfc->clipX1 = clipX1; sfc->clipY1 = clipY1;
    sfc->clipX2 = clipX2; sfc->clipY2 = clipY2;
}


void grLoadScreen(char *strArg)
{
    if (grBackgroundSfc != NULL)
        grReleaseScreen();

    if (grSavedZonesLayer != NULL)
        grReleaseSavedLayer();

    struct TResIndexEntry *idxEntry = resmgrFind(strArg);

    if (idxEntry == NULL || idxEntry->resource == NULL)
        fatalError("SCR resource %s not found.", strArg);

    struct TScrResource *scrResource = (struct TScrResource *) idxEntry->resource;

    memTrackLogf("grLoadScreen(%s): %ux%u (bg surface needs %zu bytes @4bpp/2px-per-byte)",
                 strArg,
                 (unsigned) scrResource->width,
                 (unsigned) scrResource->height,
                 (size_t) scrResource->width * scrResource->height / 2);

    if ((scrResource->width % 2) == 1) {
        fprintf(stderr, "Warning: grLoadScreen(): can't manage odd widths\n");
    }

    if (scrResource->width > 640 || scrResource->height > 480) {
        fatalError("grLoadScreen(): can't manage more than 640x480 resolutions");
    }

    uint16 width  = scrResource->width;
    uint16 height = scrResource->height;

    grBackgroundSfc = grAllocSurface(width, height);

    // The resource's uncompressed bitmap is already in the engine's own
    // 4-bit packed format (two pixels per byte, high nibble = left pixel):
    // a verbatim copy/decode is the whole conversion.
    //
    // Streaming decode (default): the compressed blob is decompressed
    // straight from the resource file into the surface - the uncompressed
    // blob (112,000..153,600 bytes for the shipped SCRs) never exists in
    // RAM, and the resource manager never owns SCR data (nothing to
    // release, nothing to retain).
    if (!idxEntry->dataResident &&
        (scrResource->compressionMethod == 1 || scrResource->compressionMethod == 2) &&
        (size_t) scrResource->uncompressedSize ==
            (size_t) grBackgroundSfc->pitch * grBackgroundSfc->height &&
        (size_t) scrResource->uncompressedSize ==
            (size_t) (width / 2) * height) {

        FILE *f = resmgrOpenMainFile();

        fseek(f, idxEntry->blobOffset, SEEK_SET);

        uncompressIntoBuf(f,
                          scrResource->compressionMethod,
                          scrResource->compressedSize,
                          scrResource->uncompressedSize,
                          grBackgroundSfc->pixels);

        fclose(f);
    }
    else {
        // Fallback when streaming is not possible (for example data that
        // is already resident): acquire, convert, release.
        if (!idxEntry->dataResident)
            resmgrAcquireData(idxEntry);

        memcpy(grBackgroundSfc->pixels, scrResource->uncompressedData,
               (size_t) (width / 2) * height);

        // The SCR data has been converted into the background surface and
        // is not needed anymore.
        resmgrReleaseData(idxEntry);
    }
}


void grInitEmptyBackground()
{
    if (grBackgroundSfc != NULL)
        grReleaseScreen();

    if (grSavedZonesLayer != NULL)
        grReleaseSavedLayer();

    // An empty background is black, expressed as the palette's black
    // index.
    grBackgroundSfc = grAllocSurface(640, 480);
    grFillSurface(grBackgroundSfc, grBlackFill);
}


void grReleaseBmp(struct TTtmSlot *ttmSlot, uint16 bmpSlotNo)
{
    for (int i=0; i < ttmSlot->numSprites[bmpSlotNo]; i++)
        grFreeSurface(ttmSlot->sprites[bmpSlotNo][i]);

    ttmSlot->numSprites[bmpSlotNo] = 0;
}


void grLoadBmp(struct TTtmSlot *ttmSlot, uint16 slotNo, char *strArg)
{
    if (ttmSlot->numSprites[slotNo])
        grReleaseBmp(ttmSlot, slotNo);

    struct TResIndexEntry *idxEntry = resmgrAcquireNamed(strArg);

    if (idxEntry == NULL)
        fatalError("BMP resource %s not found.", strArg);

    struct TBmpResource *bmpResource = (struct TBmpResource *) idxEntry->resource;
    const uint8 *inData = bmpResource->uncompressedData;
    uint32 inOffset = 0;

    memTrackLogf("grLoadBmp(%s): %u images", strArg, (unsigned) bmpResource->numImages);

    ttmSlot->numSprites[slotNo] = bmpResource->numImages;

    for (int image=0; image < bmpResource->numImages; image++) {

        if ((bmpResource->widths[image] % 2) == 1)
            fatalError("grLoadBmp(): can't manage odd widths");

        uint16 width  = bmpResource->widths[image];
        uint16 height = bmpResource->heights[image];

        struct TGrSurface *surface = grAllocSurface(width, height);

        // Each image's bitmap follows the previous one's in the
        // uncompressed data. The resource is already in this engine's
        // packed nibble format, so the copy is verbatim.
        memcpy(surface->pixels, inData + inOffset, (size_t) (width / 2) * height);
        inOffset += (uint32) (width / 2) * height;

        ttmSlot->sprites[slotNo][image] = surface;
    }

    // The BMP data has been converted into per-slot sprite surfaces and
    // is not needed anymore.
    resmgrReleaseData(idxEntry);
}


void grFadeOut()
{
    static int fadeOutType = 0;
    struct TGrSurface *tmpSfc = grNewLayer();

    grDx = grDy = 0;

    switch (fadeOutType) {

        // Circle from center
        case 0:
            // tmpSfc is the 640x480 scratch surface used for the circle
            // drawing.
            for (int radius=20; radius <= 400; radius += 20) {
                grDrawCircle(tmpSfc, 320 - radius, 240 - radius,
                    radius << 1, radius << 1, 5, 5);
                grBlit(grComposeSfc, tmpSfc,
                       0, 0, 640, 480, 0, 0, 0, 1);
                eventsWaitTick(1);
                grFramePresent();
            }
            break;

        // Rectangle from center
        case 1:
            for (int i=1; i <= 20; i++) {
                grDrawRect(grComposeSfc, 320 - i*16, 240 - i*12, i*32, i*24, 5);
                eventsWaitTick(1);
                grFramePresent();
            }
            break;

        // Right to left
        case 2:
            for (int i=600; i >= 0; i -= 40) {
                grDrawRect(grComposeSfc, i, 0, 40, 480, 5);
                eventsWaitTick(1);
                grFramePresent();
            }
            break;

        // Left to right
        case 3:
            for (int i=0; i < 640; i += 40) {
                grDrawRect(grComposeSfc, i, 0, 40, 480, 5);
                eventsWaitTick(1);
                grFramePresent();
            }
            break;

        // Middle to left and right
        case 4:
            for (int i=0; i < 320; i += 20) {
                grDrawRect(grComposeSfc, 320+i, 0, 20, 480, 5);
                grDrawRect(grComposeSfc, 300-i, 0, 20, 480, 5);
                eventsWaitTick(1);
                grFramePresent();
            }
            break;
    }

    grFreeLayer(tmpSfc);

    fadeOutType = (fadeOutType + 1) % 5;
}
