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
#include "uncompress.h"

#define MAX_ADS_RESOURCES 100
#define MAX_BMP_RESOURCES 200
#define MAX_PAL_RESOURCES 1
#define MAX_SCR_RESOURCES 20
#define MAX_TTM_RESOURCES 100


struct TAdsResource *adsResources[MAX_ADS_RESOURCES];
struct TBmpResource *bmpResources[MAX_BMP_RESOURCES];
struct TPalResource *palResources[MAX_PAL_RESOURCES];
struct TScrResource *scrResources[MAX_SCR_RESOURCES];
struct TTtmResource *ttmResources[MAX_TTM_RESOURCES];
int numAdsResources = 0;
int numBmpResources = 0;
int numPalResources = 0;
int numScrResources = 0;
int numTtmResources = 0;


//=====================================
//     Resource parsers
//
// Each parser is split in two stages:
//
//   - parseXxxHeader() reads every field of the resource header with the
//     exception of the compressed data blob, which is simply skipped over
//     with an fseek(). It records in *blobOffset the absolute position of
//     the blob, which resLoadData() will need later. uncompressedData is
//     left NULL at that stage.
//
//   - loadXxxData() jumps to the position of the blob and decompresses it.
//     Note that the decompressor reads / writes / decompresses directly
//     from the FILE stream, so the compressed data never needs to be in
//     memory.
//
//=====================================


static struct TAdsResource *parseAdsHeader(FILE *f, uint32 *blobOffset)
{
    struct TAdsResource *adsResource;
    uint8 *buffer;


    adsResource = safe_malloc(sizeof(struct TAdsResource));

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"VER:",4))
        fatalError("'VER:' string not found while parsing ADS resource");

    safe_free(buffer);

    adsResource->versionSize = readUint32(f);
    adsResource->versionString = readUint8Block(f,5);

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"ADS:",4))
        fatalError("'ADS:' string not found while parsing ADS resource");

    safe_free(buffer);

    adsResource->adsUnknown1 = readUint8(f);
    adsResource->adsUnknown2 = readUint8(f);
    adsResource->adsUnknown3 = readUint8(f);
    adsResource->adsUnknown4 = readUint8(f);

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"RES:",4))
        fatalError("'RES:' string not found while parsing ADS resource");

    safe_free(buffer);

    adsResource->resSize = readUint32(f);
    adsResource->numRes = readUint16(f);

    adsResource->res = safe_malloc(adsResource->numRes * sizeof(struct TAdsRes));

    for (int i=0; i < adsResource->numRes; i++) {
        adsResource->res[i].id = readUint16(f);
        adsResource->res[i].name = getString(f,40);
    }

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"SCR:",4))
        fatalError("'SCR:' string not found while parsing ADS resource");

    safe_free(buffer);

    adsResource->compressedSize = readUint32(f) - 5;
    adsResource->compressionMethod = readUint8(f);
    adsResource->uncompressedSize = readUint32(f);

    // skip over the compressed data, the TAG: section comes next
    *blobOffset = (uint32) ftell(f);
    fseek(f, adsResource->compressedSize, SEEK_CUR);
    adsResource->uncompressedData = NULL;

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"TAG:",4))
        fatalError("'TAG:' string not found while parsing ADS resource");

    safe_free(buffer);

    adsResource->tagSize = readUint32(f);
    adsResource->numTags = readUint16(f);

    adsResource->tags = safe_malloc(adsResource->numTags * sizeof(struct TTags));

    for (int i=0; i < adsResource->numTags; i++) {
        adsResource->tags[i].id = readUint16(f);
        adsResource->tags[i].description = getString(f,40);
    }

    return adsResource;
}


static void loadAdsData(FILE *f, struct TAdsResource *adsResource, uint32 blobOffset)
{
    fseek(f, blobOffset, SEEK_SET);

    adsResource->uncompressedData = uncompress(f,
                                      adsResource->compressionMethod,
                                      adsResource->compressedSize,
                                      adsResource->uncompressedSize
                                    );
}


static struct TBmpResource *parseBmpHeader(FILE *f, uint32 *blobOffset)
{
    struct TBmpResource *bmpResource;
    uint8 *buffer;


    bmpResource = safe_malloc(sizeof(struct TBmpResource));

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"BMP:",4))
        fatalError("'BMP:' string not found while parsing BMP resource");

    safe_free(buffer);

    bmpResource->width = readUint16(f);
    bmpResource->height = readUint16(f);

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"INF:",4))
        fatalError("'INF:' string not found while parsing BMP resource");

    safe_free(buffer);

    bmpResource->dataSize = readUint32(f);
    bmpResource->numImages = readUint16(f);

    bmpResource->widths = readUint16Block(f, bmpResource->numImages);
    bmpResource->heights = readUint16Block(f, bmpResource->numImages);

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"BIN:",4))
        fatalError("'BIN:' string not found while parsing BMP resource");

    safe_free(buffer);

    bmpResource->compressedSize = readUint32(f) - 5; // discard size of compressionmethod+uncompressedsize
    bmpResource->compressionMethod = readUint8(f);
    bmpResource->uncompressedSize = readUint32(f);

    // no further field follows the blob
    *blobOffset = (uint32) ftell(f);
    fseek(f, bmpResource->compressedSize, SEEK_CUR);
    bmpResource->uncompressedData = NULL;

    return bmpResource;
}


static void loadBmpData(FILE *f, struct TBmpResource *bmpResource, uint32 blobOffset)
{
    fseek(f, blobOffset, SEEK_SET);

    bmpResource->uncompressedData = uncompress(f,
                                      bmpResource->compressionMethod,
                                      bmpResource->compressedSize,
                                      bmpResource->uncompressedSize
                                    );
}


static struct TPalResource *parsePalResource(FILE *f)
{
    struct TPalResource *palResource;
    uint8 *buffer;


    palResource = safe_malloc(sizeof(struct TPalResource));

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"PAL:",4))
        fatalError("'PAL:' string not found while parsing PAL resource");

    safe_free(buffer);

    palResource->size = readUint16(f);
    palResource->unknown1 = readUint8(f);
    palResource->unknown2 = readUint8(f);

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"VGA:",4))
        fatalError("'VGA:' string not found while parsing PAL resource");

    safe_free(buffer);

    readUint8(f);   // size ?
    readUint8(f);
    readUint8(f);
    readUint8(f);

    for (int i=0; i < 256; i++) {
        palResource->colors[i].r = readUint8(f);
        palResource->colors[i].g = readUint8(f);
        palResource->colors[i].b = readUint8(f);
    }

    return palResource;
}


static struct TScrResource *parseScrHeader(FILE *f, uint32 *blobOffset)
{
    struct TScrResource *scrResource;
    uint8 *buffer;


    scrResource = safe_malloc(sizeof(struct TScrResource));

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"SCR:",4))
        fatalError("'SCR:' string not found while parsing SCR resource");

    safe_free(buffer);

    scrResource->totalSize = readUint16(f);
    scrResource->flags = readUint16(f);

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"DIM:",4))
        fatalError("'DIM:' string not found while parsing SCR resource");

    safe_free(buffer);

    scrResource->dimSize = readUint32(f);
    scrResource->width = readUint16(f);
    scrResource->height = readUint16(f);

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"BIN:",4))
        fatalError("'BIN:' string not found while parsing SCR resource");

    safe_free(buffer);

    scrResource->compressedSize = readUint32(f) - 5; // discard size of compressionmethod+uncompressedsize
    scrResource->compressionMethod = readUint8(f);
    scrResource->uncompressedSize = readUint32(f) ;

    // no further field follows the blob
    *blobOffset = (uint32) ftell(f);
    fseek(f, scrResource->compressedSize, SEEK_CUR);
    scrResource->uncompressedData = NULL;

    return scrResource;
}


static void loadScrData(FILE *f, struct TScrResource *scrResource, uint32 blobOffset)
{
    fseek(f, blobOffset, SEEK_SET);

    scrResource->uncompressedData = uncompress(f,
                                      scrResource->compressionMethod,
                                      scrResource->compressedSize,
                                      scrResource->uncompressedSize
                                    );
}


static struct TTtmResource *parseTtmHeader(FILE *f, uint32 *blobOffset)
{
    struct TTtmResource *ttmResource;
    uint8 *buffer;

    ttmResource = safe_malloc(sizeof(struct TTtmResource));

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"VER:",4))
        fatalError("'VER:' string not found while parsing TTM resource");

    safe_free(buffer);

    ttmResource->versionSize = readUint32(f);
    ttmResource->versionString = readUint8Block(f,5);

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"PAG:",4))
        fatalError("'PAG:' string not found while parsing TTM resource");

    safe_free(buffer);

    ttmResource->numPages = readUint32(f);
    ttmResource->pagUnknown1 = readUint8(f);
    ttmResource->pagUnknown2 = readUint8(f);

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"TT3:",4))
        fatalError("'TT3:' string not found while parsing TTM resource");

    safe_free(buffer);

    ttmResource->compressedSize = readUint32(f) - 5; // discard size of compressionmethod+uncompressedsize
    ttmResource->compressionMethod = readUint8(f);
    ttmResource->uncompressedSize = readUint32(f);

    // skip over the compressed data, the TTI: / TAG: sections come next
    *blobOffset = (uint32) ftell(f);
    fseek(f, ttmResource->compressedSize, SEEK_CUR);
    ttmResource->uncompressedData = NULL;

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"TTI:",4))
        fatalError("'TTI:' string not found while parsing TTM resource");

    safe_free(buffer);

    ttmResource->ttiUnknown1 = readUint8(f);
    ttmResource->ttiUnknown2 = readUint8(f);
    ttmResource->ttiUnknown3 = readUint8(f);
    ttmResource->ttiUnknown4 = readUint8(f);

    buffer = readUint8Block(f,4);
    if (memcmp(buffer,"TAG:",4))
        fatalError("'TAG:' string not found while parsing TTM resource");

    safe_free(buffer);

    ttmResource->tagSize = readUint32(f);
    ttmResource->numTags = readUint16(f);

    ttmResource->tags = safe_malloc(ttmResource->numTags * sizeof(struct TTags));

    for (int i=0; i < ttmResource->numTags; i++) {
        ttmResource->tags[i].id = readUint16(f);
        ttmResource->tags[i].description = getString(f,40);
    }

    return ttmResource;
}


static void loadTtmData(FILE *f, struct TTtmResource *ttmResource, uint32 blobOffset)
{
    fseek(f, blobOffset, SEEK_SET);

    ttmResource->uncompressedData = uncompress(f,
                                      ttmResource->compressionMethod,
                                      ttmResource->compressedSize,
                                      ttmResource->uncompressedSize
                                    );
}


//=====================
//     Dispatchers used by the resource manager
//=====================


void *resParseHeader(FILE *f, int resType, uint32 *blobOffset)
{
    switch (resType) {

        case RES_TYPE_ADS: return parseAdsHeader(f, blobOffset);
        case RES_TYPE_BMP: return parseBmpHeader(f, blobOffset);
        case RES_TYPE_PAL: *blobOffset = 0; return parsePalResource(f);
        case RES_TYPE_SCR: return parseScrHeader(f, blobOffset);
        case RES_TYPE_TTM: return parseTtmHeader(f, blobOffset);
    }

    fatalError("resParseHeader(): unknown resource type %d\n", resType);
    return NULL;
}


void resLoadData(FILE *f, int resType, void *resource, uint32 blobOffset)
{
    switch (resType) {

        case RES_TYPE_ADS: loadAdsData(f, (struct TAdsResource *) resource, blobOffset); return;
        case RES_TYPE_BMP: loadBmpData(f, (struct TBmpResource *) resource, blobOffset); return;
        case RES_TYPE_PAL: return;   // the whole palette resource is its own header
        case RES_TYPE_SCR: loadScrData(f, (struct TScrResource *) resource, blobOffset); return;
        case RES_TYPE_TTM: loadTtmData(f, (struct TTtmResource *) resource, blobOffset); return;
    }

    fatalError("resLoadData(): unknown resource type %d\n", resType);
}


void resDataSizes(void *resource, int resType, size_t *compressed, size_t *uncompressed)
{
    *compressed = 0;
    *uncompressed = 0;

    switch (resType) {

        case RES_TYPE_ADS:
            *compressed   = ((struct TAdsResource *) resource)->compressedSize;
            *uncompressed = ((struct TAdsResource *) resource)->uncompressedSize;
            break;

        case RES_TYPE_BMP:
            *compressed   = ((struct TBmpResource *) resource)->compressedSize;
            *uncompressed = ((struct TBmpResource *) resource)->uncompressedSize;
            break;

        case RES_TYPE_PAL:
            *uncompressed = ((struct TPalResource *) resource)->size;
            break;

        case RES_TYPE_SCR:
            *compressed   = ((struct TScrResource *) resource)->compressedSize;
            *uncompressed = ((struct TScrResource *) resource)->uncompressedSize;
            break;

        case RES_TYPE_TTM:
            *compressed   = ((struct TTtmResource *) resource)->compressedSize;
            *uncompressed = ((struct TTtmResource *) resource)->uncompressedSize;
            break;
    }
}


void resReleaseUncompressed(void *resource, int resType)
{
    switch (resType) {

        case RES_TYPE_ADS:
            safe_free(((struct TAdsResource *) resource)->uncompressedData);
            ((struct TAdsResource *) resource)->uncompressedData = NULL;
            break;

        case RES_TYPE_BMP:
            safe_free(((struct TBmpResource *) resource)->uncompressedData);
            ((struct TBmpResource *) resource)->uncompressedData = NULL;
            break;

        case RES_TYPE_PAL:      // nothing to release, the struct IS the data
            break;

        case RES_TYPE_SCR:
            safe_free(((struct TScrResource *) resource)->uncompressedData);
            ((struct TScrResource *) resource)->uncompressedData = NULL;
            break;

        case RES_TYPE_TTM:
            safe_free(((struct TTtmResource *) resource)->uncompressedData);
            ((struct TTtmResource *) resource)->uncompressedData = NULL;
            break;
    }
}


//=====================
//     Resource lookups
//=====================


struct TAdsResource *findAdsResource(char *searchString)
{
    struct TResIndexEntry *idxEntry = resmgrFind(searchString);

    if (idxEntry == NULL || idxEntry->resType != RES_TYPE_ADS || idxEntry->resource == NULL)
        fatalError("ADS resource %s not found.", searchString);

    return (struct TAdsResource *) idxEntry->resource;
}


struct TBmpResource *findBmpResource(char *searchString)
{
    struct TResIndexEntry *idxEntry = resmgrFind(searchString);

    if (idxEntry == NULL || idxEntry->resType != RES_TYPE_BMP || idxEntry->resource == NULL)
        fatalError("BMP resource %s not found.", searchString);

    return (struct TBmpResource *) idxEntry->resource;
}


struct TScrResource *findScrResource(char *searchString)
{
    struct TResIndexEntry *idxEntry = resmgrFind(searchString);

    if (idxEntry == NULL || idxEntry->resType != RES_TYPE_SCR || idxEntry->resource == NULL)
        fatalError("SCR resource %s not found.", searchString);

    return (struct TScrResource *) idxEntry->resource;
}


struct TTtmResource *findTtmResource(char *searchString)
{
    struct TResIndexEntry *idxEntry = resmgrFind(searchString);

    if (idxEntry == NULL || idxEntry->resType != RES_TYPE_TTM || idxEntry->resource == NULL)
        fatalError("TTM resource %s not found.", searchString);

    return (struct TTtmResource *) idxEntry->resource;
}
