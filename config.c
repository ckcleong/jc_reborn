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

#ifdef __xtensa__
#include <esp_err.h>
#include <esp_log.h>
#include <nvs.h>
#endif

#include "mytypes.h"
#include "utils.h"
#include "config.h"

#define BUFFER_LEN 100

#ifdef __xtensa__
#define CFG_NVS_NAMESPACE   "jc"
#define CFG_NVS_CURRENT_DAY "story_day"
#define CFG_NVS_DATE        "story_date"

static const char *TAG = "jc_cfg";
#endif


static char *cfgFullPath()
{
    char *home;
    static char *result = NULL;


    if (result == NULL) {

        home = getenv("HOME");

        if (home != NULL) {
            if (strlen(home)) {
                result = safe_malloc(strlen(home) + strlen(CFG_FILENAME) + 2);
                strcpy(result, home);
                strcat(result, "/");
                strcat(result, CFG_FILENAME);
            }
        }

        if (result == NULL) {
            result = safe_malloc(strlen(CFG_FILENAME) + 1);
            strcpy(result, CFG_FILENAME);
        }
    }

    return result;
}


#ifdef __xtensa__
static int cfgNvsRead(struct TConfig *cfg)
{
    nvs_handle_t h;
    int32_t currentDay, date;
    esp_err_t err;

    err = nvs_open(CFG_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
        return 0;

    err = nvs_get_i32(h, CFG_NVS_CURRENT_DAY, &currentDay);
    if (err == ESP_OK)
        err = nvs_get_i32(h, CFG_NVS_DATE, &date);

    nvs_close(h);

    if (err != ESP_OK)
        return 0;

    cfg->currentDay = currentDay;
    cfg->date       = date;
    return 1;
}


static void cfgNvsWrite(const struct TConfig *cfg)
{
    nvs_handle_t h;
    esp_err_t err;

    err = nvs_open(CFG_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "couldn't open NVS for story state (%s)",
                 esp_err_to_name(err));
        return;
    }

    err = nvs_set_i32(h, CFG_NVS_CURRENT_DAY, cfg->currentDay);
    if (err == ESP_OK)
        err = nvs_set_i32(h, CFG_NVS_DATE, cfg->date);
    if (err == ESP_OK)
        err = nvs_commit(h);

    nvs_close(h);

    if (err == ESP_OK)
        ESP_LOGI(TAG, "story state updated in NVS: currentDay=%d date=%d",
                 cfg->currentDay, cfg->date);
    else
        ESP_LOGW(TAG, "couldn't store story state in NVS (%s)",
                 esp_err_to_name(err));
}
#endif


void cfgFileWrite(struct TConfig *cfg)
{
#ifdef __xtensa__
    cfgNvsWrite(cfg);
#else
    FILE *f = fopen(cfgFullPath(), "w");

    if (f == NULL) {
        debugMsg("Warning: couldn't open %s for writing", CFG_FILENAME);
    }
    else {
        fprintf(f, "currentDay=%d\n", cfg->currentDay);
        fprintf(f, "date=%d\n", cfg->date);
        fclose(f);
    }
#endif
}


void cfgFileRead(struct TConfig *cfg)
{
    char buf[BUFFER_LEN];

    cfg->currentDay = 0;
    cfg->date       = 0;

#ifdef __xtensa__
    // Story state is persistent in NVS. The .jc_reborn file in the
    // read-only resource image is only a first-boot seed.
    if (cfgNvsRead(cfg))
        return;
#endif

    FILE *f = fopen(cfgFullPath(), "r");

    if (f != NULL) {

        // Parse every line, including a final line without a trailing
        // newline.
        while (fgets(buf, BUFFER_LEN, f)) {

            if(strstr(buf, "currentDay=") == buf)
                cfg->currentDay = atoi(buf + 11);

            if(strstr(buf, "date=") == buf)
                cfg->date = atoi(buf + 5);
        }

        fclose(f);

#ifdef __xtensa__
        cfgNvsWrite(cfg);
#endif
    }
}

