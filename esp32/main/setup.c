/*
 * Setup mode — see setup.h.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_mac.h>    // MACSTR/MAC2STR
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_attr.h>   // EXT_RAM_BSS_ATTR
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_http_server.h>
#include "dns_server.h"
#include <driver/gpio.h>

#include "mytypes.h"
#include "graphics.h"       // GR_FRAME_W/GR_FRAME_H
#include "board.h"
#include "qr.h"
#include "setup.h"
#include "platform.h"

static const char *TAG = "setup";

#define SETUP_AP_SSID   CONFIG_JC_SETUP_AP_SSID
#define NVS_NS          "jc"

// --- minimal 5x7 font (rows top->bottom, bit4 = left column) ----------
// Only the glyphs the setup screen needs. Drawn at 2x scale.

static const uint8_t FONT[95][9];   // defined at the bottom

// The glyph advance is tightened by half a pixel-column per scale step
// so 27-character lines still fit the 320-wide panel at 2x scale.
static void draw_text(uint16_t *fb, const char *s, int x, int y, int scale)
{
    int advance = scale * 6 - scale / 2;

    for (; *s; s++, x += advance) {
        if (*s < 32 || *s > 126)
            continue;
        const uint8_t *g = FONT[*s - 32];
        for (int r = 0; r < 9; r++)             // 7 glyph rows + 2 descender
            for (int c = 0; c < 5; c++)
                if (g[r] & (0x10 >> c))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            fb[(y + r*scale + sy) * GR_FRAME_W
                               + x + c*scale + sx] = 0x0000;
    }
}

// --- the setup screen: QR + captions ----------------------------------

static void show_qr_screen(void)
{
    uint16_t *fb = panel_frame565();
    if (fb == NULL)
        return;

    // WIFI-join URI for the open setup AP (phone camera scan joins).
    char payload[64];
    snprintf(payload, sizeof(payload), "WIFI:S:%s;T:nopass;;", SETUP_AP_SSID);

    static EXT_RAM_BSS_ATTR unsigned char qr[33 * 33];
    int size = qr_encode(payload, qr, sizeof(qr));

    for (int i = 0; i < GR_FRAME_W * GR_FRAME_H; i++)
        fb[i] = 0xFFFF;                     // white

    if (size > 0) {
        int scale = (size <= 21) ? 8 : (size <= 25) ? 7 : 5;
        int quiet = 2;
        int span = (size + 2 * quiet) * scale;
        int ox = (GR_FRAME_W - span) / 2;
        int oy = 6;

        for (int my = -quiet; my < size + quiet; my++)
            for (int mx = -quiet; mx < size + quiet; mx++) {
                uint16_t v = 0xFFFF;
                if (mx >= 0 && mx < size && my >= 0 && my < size
                    && qr[my * size + mx])
                    v = 0x0000;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        fb[(oy + (my + quiet) * scale + sy) * GR_FRAME_W
                           + ox + (mx + quiet) * scale + sx] = v;
            }

        // Two caption lines inside the 240-high panel: the glyphs are
        // 18 px tall (9 rows at 2x), so the pitch is 20 px.
        int y1 = oy + span + 6;
        int y2 = y1 + 20;
        if (y2 + 18 > GR_FRAME_H)
            y2 = GR_FRAME_H - 18;

        const char *l1 = "Join JohnnyCastaway-Setup";
        draw_text(fb, l1, (GR_FRAME_W - (int) strlen(l1) * 11) / 2,
                  y1, 2);
        const char *l2 = "Setup at http://192.168.4.1";
        draw_text(fb, l2, (GR_FRAME_W - (int) strlen(l2) * 11) / 2,
                  y2, 2);
    }

    present_panel_rgb565(fb);
}

// --- NVS clock store ---------------------------------------------------

static int nvs_read_time(int64_t *epoch, int32_t *tzmin)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return 0;
    int ok = nvs_get_i64(h, "epoch", epoch) == ESP_OK
             && nvs_get_i32(h, "tzmin", tzmin) == ESP_OK;
    nvs_close(h);
    return ok && *epoch > 1700000000;       // reject an unset clock
}

static void nvs_write_time(int64_t epoch, int32_t tzmin)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_i64(h, "epoch", epoch);
    nvs_set_i32(h, "tzmin", tzmin);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_write_sound(int on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "sound_on", (uint8_t) (on != 0));
        nvs_commit(h);
        nvs_close(h);
    }
}

// Sound preference from the last setup (default: on).
int setup_sound_enabled(void)
{
    nvs_handle_t h;
    uint8_t on = 1;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "sound_on", &on);
        nvs_close(h);
    }
    return on != 0;
}

static void apply_time(int64_t epoch, int32_t tzmin)
{
    struct timeval tv = { .tv_sec = (time_t) epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    // POSIX TZ has the sign inverted vs UTC offset (UTC-10 == UTC+10).
    // The standard-zone name must be at least 3 characters, otherwise
    // newlib/glibc reject the whole string and localtime() stays UTC.
    // Offsets are printed from their magnitude.
    int off = (tzmin < 0) ? -tzmin : tzmin;
    char tz[16];
    snprintf(tz, sizeof(tz), "JCT%s%d:%02d",
             (tzmin < 0) ? "+" : "-", off / 60, off % 60);
    setenv("TZ", tz, 1);
    tzset();
}

int setup_clock_valid(void)
{
    int64_t epoch;
    int32_t tzmin;
    return nvs_read_time(&epoch, &tzmin);
}

void setup_restore_time(void)
{
    int64_t epoch;
    int32_t tzmin;
    if (nvs_read_time(&epoch, &tzmin)) {
        apply_time(epoch, tzmin);
        ESP_LOGI(TAG, "clock restored from NVS (%lld)", (long long) epoch);
    }
}

void setup_snapshot_time(void)
{
    int64_t epoch;
    int32_t tzmin;
    if (nvs_read_time(&epoch, &tzmin))
        nvs_write_time((int64_t) time(NULL), tzmin);
}

int setup_needed(void)
{
    nvs_handle_t h;
    uint8_t forced = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "setup_req", &forced);
        nvs_close(h);
    }
    if (forced) {
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, "setup_req");
            nvs_commit(h);
            nvs_close(h);
        }
        return 1;
    }

    int64_t epoch;
    int32_t tzmin;
    return !nvs_read_time(&epoch, &tzmin);
}

// --- boot-button poll (re-enter setup: hold ~5 s) ----------------------

int setup_button_held(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    int held = 0;
    for (int i = 0; i < 50; i++) {          // 50 x 100 ms = 5 s
        if (gpio_get_level(BOARD_BTN_GPIO) != 0) {
            held = 0;
            break;
        }
        held = 1;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return held;
}

void setup_request(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "setup_req", 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

// --- association/lease visibility --------------------------------------

static void setup_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                             void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "station joined: " MACSTR " (aid %d)",
                 MAC2STR(e->mac), e->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "station left: " MACSTR, MAC2STR(e->mac));
    } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        ip_event_assigned_ip_to_client_t *e = data;
        ESP_LOGI(TAG, "DHCP lease: " IPSTR, IP2STR(&e->ip));
    }
}

// --- the portal --------------------------------------------------------

// Form body: local=YYYY-MM-DDTHH:MM&z=<minutes east of UTC>. The value
// is the phone's own clock, so UTC = timegm(local) - z*60. The form
// value arrives URL-ENCODED (Safari sends 'T15%3A00' for the time) —
// decode %XX in place first.
static void urldecode(char *s)
{
    char *w = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *w++ = (char) strtol(hex, NULL, 16);
            s += 3;
        } else {
            *w++ = *s++;                    // '+' -> space is irrelevant here
        }
    }
    *w = 0;
}

// Returns 1 when the clock was set.
static int apply_form_time(char *body)
{
    char *loc = strstr(body, "local=");
    if (loc == NULL)
        goto bad;

    // Decode before locating z= : urldecode() shrinks the string in
    // place, so any pointer taken beforehand goes stale (Safari and
    // friends send the time as "T15%3A00").
    urldecode(loc + 6);
    char *z = strstr(body, "z=");

    int Y, Mo, D, h, mi, zmin = 0;
    if (sscanf(loc + 6, "%d-%d-%dT%d:%d", &Y, &Mo, &D, &h, &mi) != 5)
        goto bad;
    if (z != NULL)
        zmin = (int) strtol(z + 2, NULL, 10);

    struct tm tmLocal = { 0 };
    tmLocal.tm_year = Y - 1900;
    tmLocal.tm_mon = Mo - 1;
    tmLocal.tm_mday = D;
    tmLocal.tm_hour = h;
    tmLocal.tm_min = mi;
    time_t epoch = timegm(&tmLocal);        // the phone's wall time, as
    if (epoch == (time_t) -1)               // if UTC
        goto bad;
    epoch -= (time_t) zmin * 60;            // -> true UTC epoch

    if (epoch > 1700000000) {
        apply_time(epoch, zmin);
        nvs_write_time(epoch, zmin);
        nvs_write_sound(strstr(body, "snd") != NULL);
        ESP_LOGI(TAG, "clock set: epoch=%lld zmin=%d — restarting",
                 (long long) epoch, zmin);
        return 1;
    }

bad:
    ESP_LOGW(TAG, "unusable form body");
    return 0;
}

static esp_err_t api_set(httpd_req_t *req)
{
    char buf[128] = { 0 };
    int len = req->content_len;
    if (len > 0 && len < (int) sizeof(buf)
        && httpd_req_recv(req, buf, len) > 0
        && apply_form_time(buf)) {

        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req,
            "<!doctype html><html><body style='font-family:sans-serif;"
            "text-align:center;margin-top:3em'><h2>Clock set! The story "
            "is starting on the panel.</h2></body></html>",
            HTTPD_RESP_USE_STRLEN);
        vTaskDelay(pdMS_TO_TICKS(1500));    // let the response flush
        esp_restart();
    }
    // Bad body or unusable time: say so, stay in setup mode.
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_OK;
}

static const char PAGE_HTML_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Johnny Castaway setup</title></head>"
    "<body style='font-family:sans-serif;text-align:center;margin-top:3em'>"
    "<h2>Johnny Castaway setup</h2>"
    "<p>Set the time</p>"
    "<form method='POST' action='/set'>"
    "<input id=t name=local type=datetime-local step=60 required "
    "style='font-size:1.3em;padding:.4em'>"
    "<input id=z name=z type=hidden>"
    "<br><label style='font-size:1.2em'>"
    "<input type=checkbox name=snd value=1";

// The ' checked' attribute is injected between the two halves from the
// stored preference (see portal_get), so the box reflects NVS.
static const char PAGE_HTML_TAIL[] =
    "> Sound enabled</label>"
    "<br><button style='font-size:1.4em;padding:.5em 2em;margin:1em'>"
    "Save &amp; restart</button>"
    "</form>"
    "<script>"
    "(function(){var d=new Date();d.setSeconds(0);"
    "var p=function(v){return (v<10?'0':'')+v};"
    "document.getElementById('t').value=d.getFullYear()+'-'+p(d.getMonth()+1)"
    "+'-'+p(d.getDate())+'T'+p(d.getHours())+':'+p(d.getMinutes());"
    "document.getElementById('z').value=-new Date().getTimezoneOffset();})();"
    "</script></body></html>";

static esp_err_t portal_get(httpd_req_t *req)
{
    // The ONLY GET handler: serve the page at / and redirect every
    // other path (the phone's connectivity probes) to / — one handler
    // so the matching order can never turn the redirect into a loop.
    ESP_LOGI(TAG, "GET %s", req->uri);

    if (strcmp(req->uri, "/") == 0) {
        // Reflect the stored sound preference instead of always
        // rendering the box as checked: re-entering setup to fix the
        // clock must not silently re-enable a disabled speaker.
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send_chunk(req, PAGE_HTML_HEAD, HTTPD_RESP_USE_STRLEN);
        if (setup_sound_enabled())      // a zero-length chunk would end the body
            httpd_resp_send_chunk(req, " checked", HTTPD_RESP_USE_STRLEN);
        httpd_resp_send_chunk(req, PAGE_HTML_TAIL, HTTPD_RESP_USE_STRLEN);
        return httpd_resp_send_chunk(req, NULL, 0);
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

void setup_run(void *arg)
{
    (void) arg;
    ESP_LOGI(TAG, "setup mode: AP '%s', portal at http://192.168.4.1/",
             SETUP_AP_SSID);
    esp_log_level_set("httpd", ESP_LOG_DEBUG);   // socket-level trace

    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // Watch the association + DHCP lease (the phone's traffic can
    // then be diagnosed: association -> lease -> HTTP hits).
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               setup_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                               setup_wifi_event, NULL));

    wifi_config_t ap = { 0 };
    strncpy((char *) ap.sta.ssid, SETUP_AP_SSID, sizeof(ap.sta.ssid) - 1);
    ap.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wildcard DNS: every lookup resolves to us -> the phone's
    // connectivity check pops the portal browser.
    dns_server_config_t dnsCfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    dns_server_handle_t dns = start_dns_server(&dnsCfg);
    (void) dns;

    httpd_handle_t server = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 16;               // page + API + probe paths
    if (httpd_start(&server, &hc) == ESP_OK) {
        // The default URI matcher is exact-match, so register every
        // path explicitly: the API, the page, and the well-known
        // captive-probe paths (each of which redirects to /). A bare
        // "*" handler never matches.
        httpd_uri_t set = { .uri = "/set", .method = HTTP_POST, .handler = api_set };
        httpd_register_uri_handler(server, &set);

        static const char *paths[] = {
            "/", "/generate_204", "/gen_204", "/hotspot-detect.html",
            "/connecttest.txt", "/redirect", "/canonical.html",
            "/success.txt", "/ncsi.txt", "/fwlink", "/favicon.ico",
            "/connectivity-check.html", "/check_network_status",
            "/kindle-wifi/wifistub.html", "/mobile/status",
        };
        for (int i = 0; i < (int) (sizeof(paths) / sizeof(paths[0])); i++) {
            httpd_uri_t u = { .uri = paths[i], .method = HTTP_GET,
                              .handler = portal_get };
            httpd_register_uri_handler(server, &u);
        }
        ESP_LOGI(TAG, "portal serving on port %d", hc.server_port);
    } else {
        ESP_LOGE(TAG, "httpd_start failed");
    }

    show_qr_screen();

    // Wait forever: the /set handler restarts the device once the
    // clock is set. Redraw the QR periodically — a full frame push is
    // the only recovery if the panel lost sync on a supply glitch.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        show_qr_screen();
    }
}

// --- font (5x9: 7 glyph rows + 2 descender rows; bit4 = left column)

static const uint8_t FONT[95][9] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00},  // space
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04, 0x00,0x00},  // !
    {0x00,0x0A,0x0A,0x00,0x00,0x00,0x00, 0x00,0x00},  // "
    {0x00,0x0A,0x1F,0x0A,0x0A,0x1F,0x0A, 0x00,0x00},  // #
    {0x00,0x04,0x0F,0x15,0x0E,0x15,0x08, 0x00,0x00},  // $
    {0x00,0x19,0x15,0x0E,0x15,0x13,0x00, 0x00,0x00},  // %
    {0x00,0x0C,0x12,0x0C,0x12,0x12,0x09, 0x00,0x00},  // &
    {0x00,0x04,0x02,0x00,0x00,0x00,0x00, 0x00,0x00},  // '
    {0x00,0x02,0x04,0x04,0x04,0x04,0x02, 0x00,0x00},  // (
    {0x00,0x04,0x02,0x02,0x02,0x02,0x04, 0x00,0x00},  // )
    {0x00,0x00,0x04,0x0A,0x04,0x0A,0x00, 0x00,0x00},  // *
    {0x00,0x00,0x04,0x04,0x1F,0x04,0x04, 0x00,0x00},  // +
    {0x00,0x00,0x00,0x00,0x00,0x04,0x02, 0x04,0x00},  // ,
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00, 0x00,0x00},  // -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C, 0x00,0x00},  // .
    {0x00,0x01,0x02,0x04,0x08,0x10,0x00, 0x00,0x00},  // /
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, 0x00,0x00},  // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E, 0x00,0x00},  // 1  (top flag points LEFT)
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F, 0x00,0x00},  // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E, 0x00,0x00},  // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02, 0x00,0x00},  // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E, 0x00,0x00},  // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E, 0x00,0x00},  // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08, 0x00,0x00},  // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E, 0x00,0x00},  // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x11,0x0E, 0x00,0x00},  // 9
    {0x00,0x00,0x04,0x00,0x00,0x04,0x00, 0x00,0x00},  // :
    {0x00,0x00,0x00,0x02,0x00,0x02,0x00, 0x00,0x00},  // ;
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02, 0x00,0x00},  // <
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00, 0x00,0x00},  // =
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08, 0x00,0x00},  // >
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04, 0x00,0x00},  // ?
    {0x0E,0x11,0x15,0x15,0x0E,0x10,0x08, 0x00,0x00},  // @
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11, 0x00,0x00},  // A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E, 0x00,0x00},  // B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E, 0x00,0x00},  // C
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C, 0x00,0x00},  // D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F, 0x00,0x00},  // E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10, 0x00,0x00},  // F
    {0x0E,0x11,0x10,0x13,0x11,0x11,0x0F, 0x00,0x00},  // G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11, 0x00,0x00},  // H
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E, 0x00,0x00},  // I
    {0x01,0x01,0x01,0x01,0x01,0x11,0x0E, 0x00,0x00},  // J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11, 0x00,0x00},  // K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F, 0x00,0x00},  // L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11, 0x00,0x00},  // M
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11, 0x00,0x00},  // N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E, 0x00,0x00},  // O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10, 0x00,0x00},  // P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D, 0x00,0x00},  // Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11, 0x00,0x00},  // R
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E, 0x00,0x00},  // S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04, 0x00,0x00},  // T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E, 0x00,0x00},  // U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04, 0x00,0x00},  // V
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A, 0x00,0x00},  // W
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11, 0x00,0x00},  // X
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04, 0x00,0x00},  // Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F, 0x00,0x00},  // Z
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E, 0x00,0x00},  // [
    {0x10,0x10,0x08,0x04,0x02,0x01,0x00, 0x00,0x00},  // backslash
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E, 0x00,0x00},  // ]
    {0x00,0x04,0x0A,0x00,0x00,0x00,0x00, 0x00,0x00},  // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F, 0x00,0x00},  // _
    {0x04,0x08,0x08,0x10,0x00,0x00,0x00, 0x00,0x00},  // `
    {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F, 0x00,0x00},  // a
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E, 0x00,0x00},  // b
    {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E, 0x00,0x00},  // c
    {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F, 0x00,0x00},  // d
    {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E, 0x00,0x00},  // e
    {0x02,0x04,0x1E,0x04,0x04,0x04,0x04, 0x00,0x00},  // f
    {0x00,0x00,0x0F,0x11,0x11,0x0F,0x01, 0x01,0x1E},  // g
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x11, 0x00,0x00},  // h
    {0x04,0x00,0x06,0x04,0x04,0x04,0x0E, 0x00,0x00},  // i
    {0x02,0x00,0x02,0x02,0x02,0x12,0x0C, 0x02,0x0C},  // j
    {0x10,0x10,0x12,0x14,0x18,0x14,0x12, 0x00,0x00},  // k
    {0x04,0x04,0x04,0x04,0x04,0x04,0x0E, 0x00,0x00},  // l (plain stem + foot; no flag)
    {0x00,0x00,0x1B,0x15,0x15,0x15,0x15, 0x00,0x00},  // m
    {0x00,0x00,0x1E,0x11,0x11,0x11,0x11, 0x00,0x00},  // n
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E, 0x00,0x00},  // o
    {0x00,0x00,0x1E,0x11,0x11,0x1E,0x10, 0x10,0x10},  // p
    {0x00,0x00,0x0F,0x11,0x11,0x0F,0x01, 0x01,0x01},  // q
    {0x00,0x00,0x12,0x14,0x10,0x10,0x10, 0x00,0x00},  // r
    {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E, 0x00,0x00},  // s
    {0x04,0x04,0x0E,0x04,0x04,0x04,0x0E, 0x00,0x00},  // t
    {0x00,0x00,0x11,0x11,0x11,0x13,0x0D, 0x00,0x00},  // u
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04, 0x00,0x00},  // v
    {0x00,0x00,0x11,0x15,0x15,0x15,0x0A, 0x00,0x00},  // w
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11, 0x00,0x00},  // x
    {0x00,0x00,0x11,0x11,0x11,0x0F,0x01, 0x02,0x02},  // y
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F, 0x00,0x00},  // z
    {0x06,0x04,0x04,0x08,0x04,0x04,0x06, 0x00,0x00},  // {
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04, 0x00,0x00},  // |
    {0x0C,0x04,0x04,0x02,0x04,0x04,0x0C, 0x00,0x00},  // }
    {0x00,0x00,0x00,0x08,0x15,0x02,0x00, 0x00,0x00},  // ~
};
