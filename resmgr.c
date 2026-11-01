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

#include "mytypes.h"
#include "utils.h"
#include "resource.h"
#include "resmgr.h"


static struct TMapFile mapFile;

struct TResIndexEntry *resIndex = NULL;
int resIndexNum = 0;


//---------------
//    Retention policy. Statics govern what happens to the uncompressed
//    data after the last consumer releases it:
//
//    - OFF (default): the data is freed right in resmgrReleaseData().
//
//    - LRU: released data stays resident as a cache up to the byte
//      budget / entry cap. On the next acquire of a retained resource,
//      the data is re-pinned without touching the file. When the budget
//      is exceeded, the least-recently-used entries are evicted.
//      Intended for flash-backed builds, where re-reading and
//      decompressing large resources costs latency.
//---------------

static int     retainPolicy     = RESMGR_RETAIN_OFF;
static size_t  retainMaxBytes   = 0;   // budget: sum of retained sizes
static int     retainMaxEntries = 0;   // budget: cap on retained count (0 = only bytes apply)

static uint32  lruClock      = 0;       // monotonic stamp generator
static long    cacheHits     = 0;       // acquires served from resident data
static long    cacheEvictions = 0;      // retired retained entries


// Does this index entry own freely-releasable uncompressed data (the
// four types with a compressed blob)?. PAL resources keep their palette
// inside the struct itself; VIN/unknown types have no data at all.
static int resTypeHasDataBlob(int resType)
{
    return resType == RES_TYPE_ADS ||
           resType == RES_TYPE_BMP ||
           resType == RES_TYPE_SCR ||
           resType == RES_TYPE_TTM;
}


static int resTypeFromName(char *resName)
{
    char *ext = resName + strlen(resName) - 4;  // get the extension .BMP .ADS etc.

    if (!strcmp(ext, ".ADS"))
        return RES_TYPE_ADS;

    if (!strcmp(ext, ".BMP"))
        return RES_TYPE_BMP;

    if (!strcmp(ext, ".PAL"))
        return RES_TYPE_PAL;

    if (!strcmp(ext, ".SCR"))
        return RES_TYPE_SCR;

    if (!strcmp(ext, ".TTM"))
        return RES_TYPE_TTM;

    // Note: there is one .VIN type file too (FILES.VIN)
    // We dont process it since it's nothing else than a list
    // of files, which we dont need

    if (!strcmp(ext, ".VIN"))
        return RES_TYPE_VIN;

    return RES_TYPE_UNKNOWN;
}


static void parseMapFile(char *fileName)
{
    FILE *f_map;

    f_map = safe_fopen(fileName, "rb");

    mapFile.unknown1 = readUint8(f_map);   // first 5 uint8s unknown
    mapFile.unknown2 = readUint8(f_map);
    mapFile.unknown3 = readUint8(f_map);
    mapFile.unknown4 = readUint8(f_map);   // ? number of resources files available in this index
    mapFile.unknown5 = readUint8(f_map);
    mapFile.unknown6 = readUint8(f_map);

    mapFile.resFileName = (char *) getString(f_map, 13);

    mapFile.numEntries = readUint16(f_map);

    mapFile.Entries = safe_malloc(mapFile.numEntries * sizeof(struct TMapFileEntry));

    for (int i=0; i<mapFile.numEntries; i++) {
        mapFile.Entries[i].length = readUint32(f_map);
        mapFile.Entries[i].offset = readUint32(f_map);
    }

    fclose(f_map);
}


// Index scan: reads the map file, then for every resource entry jumps to its
// position in the main resource file, reads the name, parses the resource
// header (everything except the compressed blob), and optionally loads the
// data right away (eager mode, which loads at boot).
static void scanIndex(char *fileName, int loadEagerly)
{
    FILE *f = safe_fopen(mapFile.resFileName, "rb");

    if (debugMode) {
        printf("Loading resources ");
        fflush(stdout);
    }

    resIndex = safe_malloc(mapFile.numEntries * sizeof(struct TResIndexEntry));

    for (int i=0; i<mapFile.numEntries; i++) {

        fseek(f, mapFile.Entries[i].offset, SEEK_SET);

        char   *resName = (char *) readUint8Block(f, 13);
        uint32  resSize = readUint32(f);
        int     resType = resTypeFromName(resName);

        if (debugMode) {
            putchar('.');
            fflush(stdout);
        }

        uint32 blobOffset = 0;
        void  *resource   = NULL;

        if (resType < RES_TYPE_VIN)
            resource = resParseHeader(f, resType, &blobOffset);

        uint8 dataResident = 0;

        // Data stage. In eager mode, it happens immediately after the
        // header scan of each resource, exactly like in the pre-resmgr
        // boot sequence. In lazy mode, data is only loaded on demand.
        if (resource != NULL && loadEagerly) {

            resLoadData(f, resType, resource, blobOffset);
            dataResident = 1;

            size_t compressedSize, uncompressedSize;
            resDataSizes(resource, resType, &compressedSize, &uncompressedSize);
            memTrackLogResource(resName, compressedSize, uncompressedSize);
        }
        else if (loadEagerly) {
            memTrackLogResource(resName, 0, 0);     // VIN and unknown types
        }

        struct TResIndexEntry *entry = &resIndex[resIndexNum++];

        entry->resName      = resName;
        entry->resType      = resType;
        entry->fileOffset   = mapFile.Entries[i].offset;
        entry->resSize      = resSize;
        entry->blobOffset   = blobOffset;
        entry->resource     = resource;
        entry->dataResident = dataResident;
        entry->pinCount     = 0;
        entry->lastUse      = 0;

        entry->dataBytes = 0;

        // Record the uncompressed data size from the header (available
        // without loading the blob) for the LRU budget accounting.
        if (resTypeHasDataBlob(resType)) {
            size_t compressedSize, uncompressedSize;
            resDataSizes(resource, resType, &compressedSize, &uncompressedSize);
            entry->dataBytes = uncompressedSize;
        }

        // Keep the typed resource arrays in sync: dump.c and graphics.c
        // still access resources through them.
        switch (resType) {

            case RES_TYPE_ADS: {
                struct TAdsResource *r = (struct TAdsResource *) resource;
                r->resName = resName;
                adsResources[numAdsResources++] = r;
                break;
            }
            case RES_TYPE_BMP: {
                struct TBmpResource *r = (struct TBmpResource *) resource;
                r->resName = resName;
                bmpResources[numBmpResources++] = r;
                break;
            }
            case RES_TYPE_PAL: {
                struct TPalResource *r = (struct TPalResource *) resource;
                r->resName = resName;
                palResources[numPalResources++] = r;
                break;
            }
            case RES_TYPE_SCR: {
                struct TScrResource *r = (struct TScrResource *) resource;
                r->resName = resName;
                scrResources[numScrResources++] = r;
                break;
            }
            case RES_TYPE_TTM: {
                struct TTtmResource *r = (struct TTtmResource *) resource;
                r->resName = resName;
                ttmResources[numTtmResources++] = r;
                break;
            }
        }
    }

    fclose(f);

    if (debugMode)
        putchar('\n');
}


void resmgrInit(char *mapFileName, int loadEagerly)
{
    parseMapFile(mapFileName);
    scanIndex(mapFileName, loadEagerly);
}


struct TResIndexEntry *resmgrFind(char *resName)
{
    for (int i=0; i<resIndexNum; i++)
        if (!strcmp(resIndex[i].resName, resName))
            return &resIndex[i];

    return NULL;
}


// Streaming-consumer support: open the resource file so the caller can
// seek to an index entry's blob and decode it without ever materializing
// the uncompressed data (see the streaming-consumers note in resmgr.h).
// The same file handle pattern that resmgrAcquireData() uses for cold
// loads.
FILE *resmgrOpenMainFile(void)
{
    return safe_fopen(mapFile.resFileName, "rb");
}


// Powers the lazy regime: look up a resource by name and make its data
// resident in one step. Returns NULL for unknown names as well as for
// data-less resource kinds (.VIN and unknown extensions).
//
// NOTE: always routed through resmgrAcquireData(), even when the data
// is already resident: the acquire must pin the entry (pinCount++) so
// that the LRU can never evict data a consumer is still using, and its
// release() remains balanced.
struct TResIndexEntry *resmgrAcquireNamed(char *resName)
{
    struct TResIndexEntry *idxEntry = resmgrFind(resName);

    if (idxEntry == NULL || idxEntry->resource == NULL)
        return NULL;

    resmgrAcquireData(idxEntry);

    return idxEntry;
}


// Inverse lookup: which index entry owns this parsed resource struct?
// (Used by code which holds a typed resource pointer and wants to
// release the data afterwards, e.g. the dump functions.)
struct TResIndexEntry *resmgrFindByResource(void *resource)
{
    for (int i=0; i<resIndexNum; i++)
        if (resIndex[i].resource == resource)
            return &resIndex[i];

    return NULL;
}


void resmgrAcquireData(struct TResIndexEntry *idxEntry)
{
    if (idxEntry == NULL || idxEntry->resource == NULL)
        return;

    if (idxEntry->resType >= RES_TYPE_VIN)
        return;     // no data blob for this type

    // Already resident? Either another consumer holds it, or the LRU
    // retained it after the previous release: served without touching
    // the file - this is the caching win of the LRU regime.
    if (idxEntry->dataResident) {

        idxEntry->pinCount++;
        idxEntry->lastUse = ++lruClock;
        cacheHits++;

        // Only logged under the LRU policy: retain-off runs keep the
        // plain memstats output.
        if (retainPolicy == RESMGR_RETAIN_LRU)
            memTrackLogf("resmgr hit    %-16s (no reload)", idxEntry->resName);

        return;
    }

    // Cold load. First reclaim the least valuable retained data, so the
    // decompressed blob grows out of already-unused cache space.
    resmgrTrimCache();

    FILE *f = safe_fopen(mapFile.resFileName, "rb");

    resLoadData(f, idxEntry->resType, idxEntry->resource, idxEntry->blobOffset);

    fclose(f);

    idxEntry->dataResident = 1;
    idxEntry->pinCount++;
    idxEntry->lastUse = ++lruClock;

    size_t compressedSize, uncompressedSize;
    resDataSizes(idxEntry->resource, idxEntry->resType, &compressedSize, &uncompressedSize);
    idxEntry->dataBytes = uncompressedSize;

    memTrackLogResource(idxEntry->resName, compressedSize, uncompressedSize);
}


void resmgrReleaseData(struct TResIndexEntry *idxEntry)
{
    if (idxEntry == NULL || idxEntry->resource == NULL)
        return;

    if (!idxEntry->dataResident)
        return;

    if (idxEntry->pinCount > 0)
        idxEntry->pinCount--;

    if (idxEntry->pinCount > 0)
        return;     // another consumer still owns the data

    // Fully released. Under the LRU policy, recently used resources stay
    // resident as a cache; anything else (including a resource that alone
    // exceeds the whole budget) is freed immediately.
    if (retainPolicy == RESMGR_RETAIN_LRU &&
        resTypeHasDataBlob(idxEntry->resType) &&
        idxEntry->dataBytes != 0 && (size_t) idxEntry->dataBytes <= retainMaxBytes) {

        idxEntry->lastUse = ++lruClock;     // most recently used from now on

        memTrackLogf("resmgr retain %-16s uncompressed=%-9u", idxEntry->resName, idxEntry->dataBytes);

        resmgrTrimCache();                  // enforce budget; may evict this very entry
        return;
    }

    resReleaseUncompressed(idxEntry->resource, idxEntry->resType);

    idxEntry->dataResident = 0;
    idxEntry->pinCount     = 0;
}


//---------------
//    LRU retention
//---------------

static void retentionState(size_t *retainedAll, int *numRetained, struct TResIndexEntry **oldest)
{
    *retainedAll = 0;
    *numRetained = 0;
    *oldest      = NULL;

    for (int i=0; i < resIndexNum; i++) {

        struct TResIndexEntry *entry = &resIndex[i];

        if (!entry->dataResident || entry->pinCount != 0 || entry->dataBytes == 0)
            continue;

        *retainedAll += entry->dataBytes;
        (*numRetained)++;

        if (*oldest == NULL || entry->lastUse < (*oldest)->lastUse)
            *oldest = entry;
    }
}


void resmgrTrimCache(void)
{
    struct TResIndexEntry *oldest;
    size_t retained;
    int    numRetained;

    if (retainPolicy != RESMGR_RETAIN_LRU)
        return;

    while (1) {

        retentionState(&retained, &numRetained, &oldest);

        if (retained <= retainMaxBytes &&
            (retainMaxEntries <= 0 || numRetained <= retainMaxEntries))
            break;

        if (oldest == NULL)
            break;      // everything resident is pinned - cannot free more

        // Evict: release the data, keep the parsed resource struct. The
        // entry stays indexable and becomes a cold load on next acquire.
        memTrackLogf("resmgr evict  %-16s uncompressed=%-9u", oldest->resName, oldest->dataBytes);

        resReleaseUncompressed(oldest->resource, oldest->resType);

        oldest->dataResident = 0;
        oldest->pinCount     = 0;
        cacheEvictions++;
    }
}


void resmgrSetRetentionPolicy(int policy, size_t maxRetainedBytes, int maxRetainedEntries)
{
    retainPolicy     = policy;
    retainMaxBytes   = maxRetainedBytes;
    retainMaxEntries = maxRetainedEntries;

    memTrackLogf("resmgr retention: policy=%s maxBytes=%zu maxEntries=%d",
                 policy == RESMGR_RETAIN_LRU ? "lru" : "off",
                 retainMaxBytes, retainMaxEntries);
}


void resmgrCacheReport(void)
{
    size_t retained;
    int    numRetained;
    struct TResIndexEntry *oldest;

    if (retainPolicy != RESMGR_RETAIN_LRU)
        return;

    retentionState(&retained, &numRetained, &oldest);

    memTrackLogf("resmgr cache  : hits=%ld evictions=%ld retained=%d entries / %zu bytes (budget %zu)",
                 cacheHits, cacheEvictions, numRetained, retained, retainMaxBytes);
}
