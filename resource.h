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

#include <stdio.h>
#include <stdlib.h>

 //------------------------
//    Types definitions
//------------------------

// Resource type ids, used by the resource manager's index and by the
// resParseHeader()/resLoadData() dispatchers.

#define RES_TYPE_ADS      0
#define RES_TYPE_BMP      1
#define RES_TYPE_PAL      2
#define RES_TYPE_SCR      3
#define RES_TYPE_TTM      4
#define RES_TYPE_VIN      5
#define RES_TYPE_UNKNOWN  6

struct TMapFile {
    uint8 unknown1;
    uint8 unknown2;
    uint8 unknown3;
    uint8 unknown4;
    uint8 unknown5;
    uint8 unknown6;
    char *resFileName;
    uint16 numEntries;
    struct TMapFileEntry *Entries;
};


struct TMapFileEntry {
    uint32 length;
    uint32 offset;
    char *resName;
    uint32 resSize;
};


struct TAdsResource {
    char *resName;
    uint32 versionSize;
    uint8 *versionString;
    uint8 adsUnknown1;
    uint8 adsUnknown2;
    uint8 adsUnknown3;
    uint8 adsUnknown4;
    uint32 resSize;
    uint16 numRes;
    struct TAdsRes *res;
    uint32 compressedSize;
    uint8 compressionMethod;
    uint32 uncompressedSize;
    uint8 *uncompressedData;
    uint32 tagSize;
    uint16 numTags;
    struct TTags *tags;
};


struct TAdsRes {
    uint16 id;
    char *name;
};


struct TBmpResource {
    char *resName;
    uint16 width;
    uint16 height;
    uint32 dataSize;
    uint16 numImages;
    uint16 *widths;
    uint16 *heights;
    uint32 compressedSize;
    uint8 compressionMethod;
    uint32 uncompressedSize;
    uint8 *uncompressedData;
};


struct TColor {
     uint8 r;
     uint8 g;
     uint8 b;
};


struct TPalResource {
    char *resName;
    uint16 size;
    uint8 unknown1;
    uint8 unknown2;
    struct TColor colors[256];
};


struct TScrResource {
    char *resName;
    uint16 totalSize;
    uint16 flags;
    uint32 dimSize;
    uint16 width;
    uint16 height;
    uint32 compressedSize;
    uint8 compressionMethod;
    uint32 uncompressedSize;
    uint8 *uncompressedData;
};


struct TTtmResource {
    char *resName;
    uint32 versionSize;
    uint8 *versionString;
    uint32 numPages;
    uint8 pagUnknown1;
    uint8 pagUnknown2;
    uint32 compressedSize;
    uint8 compressionMethod;
    uint32 uncompressedSize;
    uint8 *uncompressedData;
    uint8 ttiUnknown1;
    uint8 ttiUnknown2;
    uint8 ttiUnknown3;
    uint8 ttiUnknown4;
    uint32 tagSize;
    uint16 numTags;
    struct TTags *tags;
};


struct TTags {
    uint16 id;
    char *description;
};


//------------------------
//    Public variables
//------------------------

extern struct TAdsResource *adsResources[];
extern struct TBmpResource *bmpResources[];
extern struct TPalResource *palResources[];
extern struct TScrResource *scrResources[];
extern struct TTtmResource *ttmResources[];
extern int numAdsResources;
extern int numBmpResources;
extern int numPalResources;
extern int numScrResources;
extern int numTtmResources;


//----------------------------
//    Functions prototypes
//----------------------------

// Used by resmgr.c, which owns the boot-time orchestration:
//
//   - resParseHeader() parses all the header fields of one resource (in
//     the file type given as argument) and returns a pointer to its
//     freshly typed struct. The compressed blob is NOT read nor
//     decompressed: it is skipped over, and its absolute file offset is
//     returned in *blobOffset for a later resLoadData() call.
//
//   - resLoadData() loads and decompresses the data of an existing, fully
//     parsed resource.
//
//   - resDataSizes() returns the compressed / uncompressed sizes of a
//     resource struct.
//
//   - resReleaseUncompressed() frees the uncompressed data of a resource
//     (pointer kept constant - resName etc. - only the data is released).

void  *resParseHeader(FILE *f, int resType, uint32 *blobOffset);
void   resLoadData(FILE *f, int resType, void *resource, uint32 blobOffset);
void   resDataSizes(void *resource, int resType, size_t *compressed, size_t *uncompressed);
void   resReleaseUncompressed(void *resource, int resType);

struct TAdsResource *findAdsResource(char *);
struct TBmpResource *findBmpResource(char *);
struct TScrResource *findScrResource(char *);
struct TTtmResource *findTtmResource(char *);

