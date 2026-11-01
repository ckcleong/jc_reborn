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
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#include "mytypes.h"
#include "utils.h"


#define BUF_LEN 256

int debugMode = 0;

int memTracking = 0;


//=============================
//   Memory instrumentation
//
// Compiled in only with -DMEMTRACK (make MEMTRACK=1). Zero overhead otherwise.
//
// The counters track the TRUE resident memory by having safe_malloc() store
// the block size in a a header in front of each block, so safe_free() can
// subtract it again. All of the engine's allocations go through safe_malloc
// (helpers like getString() and uncompress() use it internally), except for
// buffers allocated by SDL itself (sdl surfaces loaded with SDL_LoadWAV),
// which must be registered manually via memTrackExternalAdd()/-Remove().
//
// At runtime the '[MEM]' logs and reports are printed only if the
// 'memstats' command-line argument was passed.
//=============================

#ifdef MEMTRACK

#define MEM_HEADER      16          // keeps 16-byte alignment intact

static size_t memResident    = 0;   // bytes currently resident
static size_t memPeak        = 0;   // peak resident since program start
static size_t memBaseLine    = 0;   // resident as of the last checkpoint
static long   memNumAllocs   = 0;
static long   memNumFrees    = 0;
static long   memNumExternal = 0;   // SDL-allocated buffers registered

#endif


void memTrackLogf(char *message, ... )
{
    if (memTracking) {
        va_list args;
        va_start(args, message);
        printf("[MEM] ");
        vprintf(message, args);
        printf("\n");
        fflush(stdout);
        va_end(args);
    }
}


void memTrackCheckpoint(char *tag)
{
#ifdef MEMTRACK
    memBaseLine = memResident;
    memTrackReport(tag);
#endif
}


void memTrackReport(char *tag)
{
#ifdef MEMTRACK
    if (memTracking)
        printf("[MEM] %s: resident=%zu  deltaVsCheckpoint=%ld  peak=%zu\n",
               tag, memResident, (long) (memResident - memBaseLine), memPeak);
#endif
}


void memTrackRunReport(void)
{
#ifdef MEMTRACK
    if (!memTracking)
        return;

    printf("[MEM] ---- run summary ----\n");
    printf("[MEM] safe_malloc() calls : %ld\n", memNumAllocs);
    printf("[MEM] safe_free() calls   : %ld\n", memNumFrees);
    printf("[MEM] SDL buffers (wavs)  : %ld\n", memNumExternal);
    printf("[MEM] resident at exit    : %zu\n", memResident);
    printf("[MEM] peak resident       : %zu\n", memPeak);
#endif
}


void memTrackLogResource(char *resName, size_t compressed, size_t uncompressed)
{
#ifdef MEMTRACK
    if (memTracking)
        printf("[MEM] res %-16s compressed=%-9zu uncompressed=%-9zu resident=%zu peak=%zu\n",
               resName, compressed, uncompressed, memResident, memPeak);
#endif
}


void memTrackExternalAdd(size_t bytes)
{
#ifdef MEMTRACK
    memResident += bytes;
    memNumExternal++;
    memNumAllocs++;
    if (memResident > memPeak)
        memPeak = memResident;
#endif
}


void memTrackExternalRemove(size_t bytes)
{
#ifdef MEMTRACK
    memResident -= bytes;
    memNumExternal--;
    memNumFrees++;
#endif
}


void fatalError(char *message, ... )
{
    va_list(args);

    va_start(args, message);
    fprintf(stderr, "\n\n Fatal error : ");
    vfprintf(stderr, message, args);
    fprintf(stderr, "\n\n");
    va_end(args);

    exit(1);
}


void debugMsg(char *message, ... )
{
    if (debugMode) {
        va_list(args);
        va_start(args, message);
        vprintf(message, args);
        printf("\n");
        va_end(args);
    }
}


void *safe_malloc(size_t size)
{
#ifdef MEMTRACK
    uint8 *raw = malloc(size + MEM_HEADER);

    if (raw == NULL)
        fatalError("failed to malloc() %zu bytes", size);

    *(size_t *) raw = size;

    memResident += size;
    memNumAllocs++;

    if (memResident > memPeak)
        memPeak = memResident;

    return raw + MEM_HEADER;
#else
    void *ptr = malloc(size);

    if (ptr == NULL)
        fatalError("failed to malloc() %d bytes", size);

    return ptr;
#endif
}


void safe_free(void *ptr)
{
#ifdef MEMTRACK
    uint8 *raw;
    size_t size;

    if (ptr == NULL)
        return;

    raw = (uint8 *) ptr - MEM_HEADER;
    size  = *(size_t *) raw;

    memResident -= size;
    memNumFrees++;

    free(raw);
#else
    free(ptr);
#endif
}


FILE *safe_fopen(const char *pathname, const char *mode)
{
    FILE *f;

#ifdef __xtensa__
    // The ESP32 IDF VFS has no chdir(), so bare paths are prefixed with
    // the FATFS mount point (esp32/main/main.c mounts the 'resources'
    // partition there). Absolute paths, such as the $HOME config, pass
    // through untouched.
    char devicePath[300];
    if (pathname[0] != '/') {
        snprintf(devicePath, sizeof(devicePath), "/resources/%s", pathname);
        pathname = devicePath;
    }
#endif

    f = fopen(pathname, mode);

    if (f == NULL)
        fatalError("unable to open file %s in mode '%s'", pathname, mode);

    return f;
}


uint8 readUint8(FILE *f)
{
    return fgetc(f);
}


uint16 readUint16(FILE *f)
{
    uint16 a;

    a  = fgetc(f);
    a += fgetc(f) << 8;

    return a;
}


uint32 readUint32(FILE *f)
{
    uint32 a;

    a  = fgetc(f);
    a += fgetc(f) << 8;
    a += fgetc(f) << 16;
    a += fgetc(f) << 24;

    return a;
}


char *getString(FILE *f, int maxlen)
{
    int numread = 0;
    int lastread = -1;
    char buf[BUF_LEN];
    char *out;

    while ((numread < maxlen) && (numread < BUF_LEN) && (lastread != 0)) {

        lastread = buf[numread++] = fgetc(f);
    }

    out = safe_malloc(numread * sizeof(char));
    memcpy(out, buf, numread);
    return out;
}


uint8 *readUint8Block(FILE *f, int len)
{
    uint8 *out = safe_malloc(len * sizeof(char));

    for (int i=0; i < len; i++)
        out[i] = fgetc(f);

    return out;
}


uint16 *readUint16Block(FILE *f, int len)
{
    uint16 *out = safe_malloc(len * sizeof(uint16));

    for (int i=0; i < len; i++)
        out[i] = readUint16(f);

    return out;
}


uint16 peekUint16(uint8 *data, uint32 *offset)
{
    uint16 result;

    result  = data[(*offset)++];
    result |= data[(*offset)++] << 8;

    return result;
}


void peekUint16Block(uint8 *data, uint32 *offset, uint16 *dest, int len)
{
    for (int i=0; i < len ; i++)
        dest[i] = peekUint16(data, offset);
}


void hexdump(uint8 *data, uint32 len)
{
    if (data==NULL)
        printf("Can't dump NULL data\n");

    printf("\n");

    for (uint32 i=0; i < len; i++) {

        printf("%02x ",data[i]);

        if ((i & 0x0f) == 0x07) { printf (" ");  }
        if ((i & 0x0f) == 0x0f) { printf ("\n"); }
    }

    printf("\n");
}


// --- Test-only pseudo-clock pin (baseline support) ---
//
// The story's content selection reads the wall clock (story.c): the
// island day/night choice is getHour() % 8, holiday dates come from
// getMonthAndDay(), and story-day progression compares getDayOfYear()
// against the persisted config. A 'story' run's frame stream therefore
// depends on when it starts. JC_TESTTIME=MMDDHH (e.g. 102014 = Oct 20,
// hour 14) pins all three helpers to a fixed moment, making the
// baseline suite byte-deterministic. Unset: the wall clock is used.
//
// The pinned calendar is a non-leap year (pinned Feb 29 maps to its
// non-leap day-of-year); nothing else in the engine reads minutes or
// the year.

static int      jcTimePinState = -1;    // -1 = env not parsed yet
static int      jcPinYday, jcPinHour;
static char     jcPinMonDay[5];


static void jcTimePinParse()
{
    const char *env;
    int mm, dd, hh, yday = 0, i;

    jcTimePinState = 0;

    if ((env = getenv("JC_TESTTIME")) == NULL)
        return;

    if (strlen(env) != 6
        || sscanf(env, "%2d%2d%2d", &mm, &dd, &hh) != 3
        || mm < 1 || mm > 12 || dd < 1 || dd > 31 || hh < 0 || hh > 23) {
        fprintf(stderr, "Warning : JC_TESTTIME=%s malformed, want MMDDHH "
                        "(pin ignored)\n", env);
        return;
    }

    for (i = 1; i < mm; i++)
        yday += (i == 2) ? 28
              : (i == 4 || i == 6 || i == 9 || i == 11) ? 30 : 31;

    jcPinYday  = yday + dd - 1;         // tm_yday convention (0-based)
    jcPinHour  = hh;
    sprintf(jcPinMonDay, "%02d%02d", mm, dd);
    jcTimePinState = 1;
}


static void jcTimePinEnsure()
{
    if (jcTimePinState < 0)
        jcTimePinParse();
}


int getDayOfYear()
{
    struct timeval tv;
    struct tm *localTime;

    jcTimePinEnsure();
    if (jcTimePinState)
        return jcPinYday;

    gettimeofday(&tv, NULL);
    localTime = localtime(&tv.tv_sec);
    return localTime->tm_yday;
}


int getHour()
{
    struct timeval tv;
    struct tm *localTime;

    jcTimePinEnsure();
    if (jcTimePinState)
        return jcPinHour;

    gettimeofday(&tv, NULL);
    localTime = localtime(&tv.tv_sec);
    return localTime->tm_hour;
}


char *getMonthAndDay()
{
    struct timeval tv;
    struct tm *localTime;
    static char result[5];

    jcTimePinEnsure();
    if (jcTimePinState)
        return jcPinMonDay;

    gettimeofday(&tv, NULL);
    localTime = localtime(&tv.tv_sec);
    strftime(result, 5, "%m%d", localTime);
    return result;
}

