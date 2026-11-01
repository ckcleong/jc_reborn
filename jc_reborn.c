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
#include "dump.h"
#include "graphics.h"
#include "events.h"
#include "sound.h"
#include "ttm.h"
#include "ads.h"
#include "story.h"

#ifdef __linux__
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

// Env-gated segfault diagnostic (see docs/TESTING.md): JC_SEGVDUMP prints a
// symbolized backtrace on SIGSEGV so rare crashes can be triaged on
// systems where core dumps are unavailable. Link with -rdynamic for
// symbol names.
static void jcSegvHandler(int sig)
{
    void *bt[64];
    int n = backtrace(bt, 64);
    fprintf(stderr, "JC_SEGVDUMP: fatal signal %d, backtrace:\n", sig);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    _exit(139);
}
#endif


static int  argDump     = 0;
static int  argBench    = 0;
static int  argTtm      = 0;
static int  argAds      = 0;
static int  argPlayAll  = 0;
static int  argIsland   = 0;
static int  argLru      = 0;
static int  argLruBudget= 0;

static char *args[3];
static int  numArgs  = 0;


static void usage()
{
        printf("\n");
        printf(" Usage :\n");
        printf("         jc_reborn\n");
        printf("         jc_reborn help\n");
        printf("         jc_reborn version\n");
        printf("         jc_reborn dump\n");
        printf("         jc_reborn [<options>] bench\n");
        printf("         jc_reborn [<options>] ttm <TTM name>\n");
        printf("         jc_reborn [<options>] ads <ADS name> <ADS tag no>\n");
        printf("\n");
        printf(" Available options are:\n");
        printf("         window     - play in windowed mode\n");
        printf("         nosound    - quiet mode\n");
        printf("         island     - display the island as background for ADS play\n");
        printf("         debug      - print some debug info on stdout\n");
        printf("         hotkeys    - enable hot keys\n");
        printf("         memstats   - print memory resident/peak stats (needs -DMEMTRACK build)\n");
        printf("         lru[=bytes] - retain recently used resource data in an LRU cache (default budget 1 MiB)\n");
        printf("\n");
        printf(" While-playing hot-keys (if enabled):\n");
        printf("         Esc        - Terminate immediately\n");
        printf("         Alt+Return - Toggle full screen / windowed mode\n");
        printf("         Space      - Toggle pause / unpause\n");
        printf("         Return     - When paused, advance one frame\n");
        printf("         <M>        - toggle max / normal speed\n");
        printf("\n");
        exit(1);
}


static void version()
{
        printf("\n");
        printf("    Johnny Reborn, an open-source engine for\n");
        printf("    the classic Johnny Castaway screensaver by Sierra.\n");
        printf("    Development version Copyright (C) 2019 Jeremie GUILLAUME\n");
        printf("\n");
        exit(1);
}


static void parseArgs(int argc, char **argv)
{
    int numExpectedArgs = 0;

    for (int i=1; i < argc; i++) {

        if (numExpectedArgs) {
            args[numArgs++] = argv[i];
            numExpectedArgs--;
        }
        else {
            if (!strcmp(argv[i], "help")) {
                usage();
            }
            if (!strcmp(argv[i], "version")) {
                version();
            }
            else if (!strcmp(argv[i], "dump")) {
                argDump = 1;
            }
            else if (!strcmp(argv[i], "bench")) {
                argBench = 1;
            }
            else if (!strcmp(argv[i], "ttm")) {
                argTtm = 1;
                numExpectedArgs = 1;
            }
            else if (!strcmp(argv[i], "ads")) {
                argAds = 1;
                numExpectedArgs = 2;
            }
            else if (!strcmp(argv[i], "window")) {
                grWindowed = 1;
            }
            else if (!strcmp(argv[i], "nosound")) {
                soundDisabled = 1;
            }
            else if (!strcmp(argv[i], "island")) {
                argIsland = 1;
            }
            else if (!strcmp(argv[i], "debug")) {
                debugMode = 1;
            }
            else if (!strcmp(argv[i], "hotkeys")) {
                evHotKeysEnabled = 1;
            }
            else if (!strcmp(argv[i], "memstats")) {
                memTracking = 1;
            }
            else if (!strncmp(argv[i], "lru", 3)) {

                // LRU retention of resource data (see RESMGR.md). Bare
                // 'lru' selects the default budget, 'lru=<bytes>' an
                // explicit one.
                argLru = 1;

                if (argv[i][3] == '=')
                    argLruBudget = atoi(argv[i] + 4);
            }
        }
    }

    if (numExpectedArgs)
        usage();

    if (argDump + argBench + argTtm + argAds > 1)
        usage();

    if (argDump + argBench + argTtm + argAds == 0)
        argPlayAll = 1;
}


int main(int argc, char **argv)
{
#ifdef __linux__
    if (getenv("JC_SEGVDUMP") != NULL)
        signal(SIGSEGV, jcSegvHandler);
#endif

    parseArgs(argc, argv);

    if (argDump)
        debugMode = 1;

    // Boot-phase resource indexing. Lazy regime: only the (tiny)
    // headers of all resources are parsed at boot; the data is loaded
    // on demand, by resmgrAcquireData() / resmgrAcquireNamed().
    resmgrInit("RESOURCE.MAP", 0);
    memTrackCheckpoint("boot");     // peak during resource loading

    // Optional LRU retention behind the acquire/release API. Off by
    // default: each resource's data is released as soon as its consumer
    // is done.
    if (argLru)
        resmgrSetRetentionPolicy(RESMGR_RETAIN_LRU,
                                 argLruBudget ? argLruBudget : 1024 * 1024,   // 1 MiB default
                                 0);

    if (argPlayAll) {
        graphicsInit();
        soundInit();

        storyPlay();

        memTrackRunReport();        // peak during a whole story
        resmgrCacheReport();        // cache hit/eviction figures (LRU policy)

        soundEnd();
        graphicsEnd();
    }

    else if (argDump) {
        dumpAllResources();
    }

    else if (argBench) {
        graphicsInit();
        adsPlayBench();
        graphicsEnd();
    }

    else if (argTtm) {
        graphicsInit();
        soundInit();

        adsPlaySingleTtm(args[0]);

        resmgrCacheReport();        // cache hit/eviction figures (LRU policy)

        soundEnd();
        graphicsEnd();
    }

    else if (argAds) {

        graphicsInit();
        soundInit();

        if (argIsland)
            adsInitIsland();
        else
            adsNoIsland();

        adsPlay(args[0], atoi(args[1]));

        resmgrCacheReport();        // cache hit/eviction figures (LRU policy)

        soundEnd();
        graphicsEnd();
    }

    return 0;
}

