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

#include "mytypes.h"


//---------------
//    Resource manager
//
//    At boot time, resmgrInit() scans the RESOURCE.MAP / RESOURCE.001
//    pair and builds a full index of every resource: name, type, file
//    offsets and - except for the compressed data blob - the complete
//    parsed resource header for each one. This index is tiny and stays
//    resident forever.
//
//    The uncompressed data of each resource is a separately manageable
//    object: it can be loaded with resmgrAcquireData() and released with
//    resmgrReleaseData(). The pointers handed out by find*Resource()
//    borrow that data: they permanently point at the resource struct.
//    The struct's uncompressedData pointer is NULL until the data is
//    resident.
//
//    Two boot regimes exist:
//     - eager mode (loadEagerly = 1): all data is loaded during the boot
//       scan and stays resident.
//     - lazy mode (loadEagerly = 0, used by both entry points): only
//       headers are loaded at boot; data is loaded on demand.
//
//    Release/retention policy. resmgrReleaseData() marks the end of a
//    consumer's use of the data. What happens to the bytes then is
//    configurable:
//
//     - no retention (default, RESMGR_RETAIN_OFF): the data is freed
//       immediately.
//     - LRU retention (RESMGR_RETAIN_LRU, see resmgrSetRetentionPolicy()):
//       released data stays resident as a cache, up to a byte budget and
//       an entry cap; when either is exceeded, the least-recently-used
//       entries are evicted (data freed). Revisiting a retained resource
//       costs no re-decompression: resmgrAcquireData() just re-pins it.
//
//    Ownership safety: eviction never frees the data of an entry which
//    is still pinned by an outstanding acquire. Resource data is only
//    ever released between scenes or between uses.
//---------------

struct TResIndexEntry {
    char   *resName;
    int     resType;       // one of the RES_* constants from resource.h
    uint32  fileOffset;    // offset of the resource in the main resource file
    uint32  resSize;       // "resSize" as advertised by the map file entry
    uint32  blobOffset;    // absolute offset of the compressed blob in the
                           // file (0 for typeless resources, e.g. .PAL),
                           // determined at header-scanning time.
    void   *resource;      // pointer to the parsed resource struct
                           // (TAdsResource, TBmpResource, ... depending on
                           // resType). NULL for unsupported types.
    uint8   dataResident;  // non-zero once uncompressedData is in memory
    uint8   pinCount;      // number of outstanding acquire() calls without
                           // a matching release(). While non-zero, the data
                           // must stay in memory: it is never evicted.
    uint32  lastUse;       // LRU stamp, bumped on every acquire/release
    uint32  dataBytes;     // uncompressed data size (recorded from the
                           // header at scan time), 0 for data-less types
};


//----------------
//    Retention policies (what happens to the data after the last
//    consumer releases it)
//----------------

#define RESMGR_RETAIN_OFF  0    // free immediately (default)
#define RESMGR_RETAIN_LRU  1    // keep released data resident as an LRU
                                // cache up to a byte budget + entry cap

// Select the retention regime. With the LRU policy, 'maxRetainedBytes'
// caps the sum of the retained (released but not yet re-used) data sizes
// and 'maxRetainedEntries' caps their count (0 = cap by bytes only).
// Resources whose data alone would not fit in the budget are always
// released immediately instead of retained.
void resmgrSetRetentionPolicy(int policy, size_t maxRetainedBytes, int maxRetainedEntries);

// Evict least-recently-used retained entries until the byte budget and
// entry cap are met. Called automatically on the acquire/release paths;
// also available explicitly (e.g. by the platform glue between scenes).
void resmgrTrimCache(void);

// One-line [MEM] report of the cache behaviour so far (policy, hits,
// evictions, currently retained bytes). Only printed in memstats runs.
void resmgrCacheReport(void);


void resmgrInit(char *mapFileName, int loadEagerly);

struct TResIndexEntry *resmgrFind(char *resName);
struct TResIndexEntry *resmgrAcquireNamed(char *resName);
struct TResIndexEntry *resmgrFindByResource(void *resource);

void resmgrAcquireData(struct TResIndexEntry *idxEntry);
void resmgrReleaseData(struct TResIndexEntry *idxEntry);

//---------------
//    Streaming consumers (SCR backgrounds): these bypass the uncompressed
//    data management entirely - they never make 'uncompressedData'
//    resident, so there is nothing to acquire/release/retain. Instead the
//    consumer reads and decompresses the blob itself, straight from the
//    resource file into its destination (e.g. the pixel buffer of the
//    surface it is building), using the header + blobOffset recorded in
//    the index entry and the dispatchers below (resource.h).
//
//    resmgrOpenMainFile() opens the resource file (RESOURCE.001, or
//    whatever the map points at) for a streaming read. The caller does
//    the fseek() to idxEntry->blobOffset and closes the handle when done.
//---------------

FILE *resmgrOpenMainFile(void);
