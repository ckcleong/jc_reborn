#include "mytypes.h"

// Platform-independent graphics interface.
//
// This header is SDL-free: the engine core compiles for both the PC and
// ESP32 builds. All SDL usage lives in the PC-only files (graphics_pc.c,
// events.c, sound.c).
//
// Platform hooks are defined per platform:
// - PC (graphics_pc.c): SDL window lifecycle, grPushFrame into the SDL
//   window, grTicks via SDL_GetTicks, window operations.
// - ESP32 (esp32/main/platform_glue.c): esp_lcd SPI push in grPushFrame,
//   grTicks via esp_timer, no-op window hooks, and the events/sound
//   implementations.

// The engine's native coordinate space. Surfaces store 4-bit packed
// palette indices at this resolution; decimation to the 320x240 panel
// happens in grPresentFrame().
#define SCREEN_WIDTH        640
#define SCREEN_HEIGHT       480

#define GR_FRAME_W          320     // presented panel width  (640 / 2)
#define GR_FRAME_H          240     // presented panel height (480 / 2)

#define MAX_BMP_SLOTS       6
#define MAX_SPRITES_PER_BMP 120
#define MAX_TTM_SLOTS       10
#define MAX_TTM_THREADS     10


// Internal 4-bit packed surface at the engine's 640-space resolution.
//
// - pixels: 4-bit palette indices, two horizontally adjacent pixels per
//   byte (high nibble = left pixel). Rows are byte-packed; widths are
//   always even in the source data, so rows never straddle.
// - Coordinates are 1:1 with the TTM/ADS 640-space coordinates. The
//   decimation to the 320x240 panel happens only in grPresentFrame().
//   Full-resolution storage is required because odd sprite/layer offsets
//   reference odd source columns; decimating at load time would lose them.
// - *color* arguments are palette indices (0..15) into the current
//   ttmPalette. Palette-to-RGB conversion happens at presentation time.
// - Transparency: palette indices whose ttmPalette entry renders magenta
//   (RGB (0xa8,0,0xa8)) are skipped when blitting — see grBlit().
// - clipX1/clipY1/clipX2/clipY2: clip zone in 640-space, exclusive
//   right/bottom. Only rect fills and blits honour it; direct pixel writes
//   honour only surface bounds.
struct TGrSurface {
    uint16  width;      // 640-space width (always even)
    uint16  height;     // 640-space height
    uint16  pitch;      // packed bytes per row ((width + 1) / 2)
    uint8  *pixels;
    sint16  clipX1, clipY1, clipX2, clipY2;
};

// RGB888 -> RGB565 conversion for the panel's wire format. Both consumers
// use this function:
// - grDumpFrameRef() when it appends to frames.bin (the PC reference), and
// - the ESP32 push (esp32/main/platform_glue.c) when it converts the
//   canonical panel buffer for the SPI transfer.
// Keeping both sites on one inline keeps the reference and the device
// output in sync by construction.
static inline uint16 grRgb888To565(const uint8 *px)
{
    return (uint16)(((px[0] >> 3) << 11)
                  | ((px[1] >> 2) <<  5)
                  |  (px[2] >> 3));
}

struct TAdsScene {
    uint16 slot;
    uint16 tag;
    uint16 numPlays;
};


struct TResIndexEntry;      // defined in resmgr.h

struct TTtmSlot {
    uint8       *data;
    uint32      dataSize;
    struct      TTtmTag *tags;
    int         numTags;
    // Resource-manager bookkeeping: the index entry owning 'data', so
    // ttmResetSlot() can release it, plus the previous owner when the slot
    // has been re-loaded since that data was last used. Releasing the old
    // generation is deferred because threads may still be walking it.
    struct TResIndexEntry *resIdxEntry;
    struct TResIndexEntry *resIdxEntryOld;
    int         numSprites[MAX_BMP_SLOTS];
    struct TGrSurface *sprites[MAX_BMP_SLOTS][MAX_SPRITES_PER_BMP];
};

struct TTtmTag {  // used by TTM and ADS tag tables
    uint16 id;
    uint32 offset;
};

struct TTtmThread {
    struct TTtmSlot   *ttmSlot;
    int    isRunning;
    uint16 sceneSlot;
    uint16 sceneTag;
    short  sceneTimer;
    uint16 sceneIterations;
    uint32 ip;
    uint16 delay;
    uint16 timer;
    uint32 nextGotoOffset;
    uint8  selectedBmpSlot;
    uint8  fgColor;
    uint8  bgColor;
    struct TGrSurface *ttmLayer;
};

extern struct TGrSurface *grBackgroundSfc;

extern int grDx;
extern int grDy;
extern int grWindowed;
extern int grUpdateDelay;


void graphicsInit();
void graphicsEnd();
void grRefreshDisplay();
void grToggleFullScreen();
void grUpdateDisplay(struct TTtmThread *ttmBackgroundThread,
                     struct TTtmThread *ttmThreads,
                     struct TTtmThread *ttmHolidayThreads);

// Engine service: monotonically increasing ticks (ms) used by the pacing
// logic (ads.c). PC: SDL_GetTicks(); ESP32: esp_timer.
uint32 grTicks();

// Platform hooks (PC: graphics_pc.c; ESP32: esp32/main/platform_glue.c):

// grPushFrame: presents the canonical panel frame (RGB888 row-major,
// 3 bytes/pixel) produced by grPresentFrame(), and performs the
// flush/update as part of the same transaction (PC:
// SDL_UpdateWindowSurface; ESP32: the blocking SPI-DMA draw).
void grPushFrame(const uint8 *rgb888, int width, int height);

// ESP32 fused-present seam: when grPushFrame565Buf() returns non-NULL,
// grPresentFrame() decimates the compose surface straight into that RGB565
// buffer (skipping the grPanel RGB888 intermediate and the separate
// convert+swap passes) and calls grPushFrame565Present() to do the SPI
// draw. On PC both hooks are NULL/no-op and the canonical grPanel path
// runs.
const uint16_t *grPushFrame565Buf(void);
void grPushFrame565Present(void);

// grWindowOpen/grWindowClose: stage lifecycle. PC: SDL window creation /
// destruction (graphicsInit/graphicsEnd delegate). ESP32: no-ops, because
// the app owns the panel and calls panel_init() itself.
void grWindowOpen(void);
void grWindowClose(void);

// grWinColorDirty: set by grLoadPalette(), consumed by the PC push (it
// maps the palette into the window surface format when changed). The
// engine half owns the flag because palette reloads are an engine event;
// the platform push half only observes it.
extern int grWinColorDirty;

struct TGrSurface *grNewLayer();
void grFreeLayer(struct TGrSurface *sfc);

void grLoadBmp(struct TTtmSlot *ttmSlot, uint16 slotNo, char *strArg);
void grReleaseBmp(struct TTtmSlot *ttmSlot, uint16 bmpSlotNo);

void grSetClipZone(struct TGrSurface *sfc, sint16 x1, sint16 y1, sint16 x2, sint16 y2);
void grCopyZoneToBg(struct TGrSurface *sfc, uint16 arg0, uint16 arg1, uint16 arg2, uint16 arg3);
void grSaveImage1(struct TGrSurface *sfc, uint16 arg0, uint16 arg1, uint16 arg2, uint16 arg3);
void grSaveZone(struct TGrSurface *sfc, uint16 arg0, uint16 arg1, uint16 arg2, uint16 arg3);
void grRestoreZone(struct TGrSurface *sfc, uint16 arg0, uint16 arg1, uint16 arg2, uint16 arg3);
void grDrawPixel(struct TGrSurface *sfc, sint16 x, sint16 y, uint8 color);
void grDrawLine(struct TGrSurface *sfc, sint16 x1, sint16 y1, sint16 x2, sint16 y2, uint8 color);
void grDrawRect(struct TGrSurface *sfc, sint16 x, sint16 y, uint16 width, uint16 height, uint8 color);
void grDrawCircle(struct TGrSurface *sfc, sint16 x1, sint16 y1, uint16 width, uint16 height, uint8 fgColor, uint8 bgColor);
void grDrawSprite(struct TGrSurface *sfc, struct TTtmSlot *ttmSlot, sint16 x, sint16 y, uint16 spriteNo, uint16 imageNo);
void grDrawSpriteFlip(struct TGrSurface *sfc, struct TTtmSlot *ttmSlot, sint16 x, sint16 y, uint16 spriteNo, uint16 imageNo);
void grInitEmptyBackground();
void grClearScreen(struct TGrSurface *sfc);
void grFadeOut();

void grLoadPalette();
void grLoadScreen(char *strArg);
