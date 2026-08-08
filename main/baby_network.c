#include "baby_network.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs.h"

/* 页面大缓冲区的 format-truncation 是保守误报（snprintf 运行时正确截断） */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define DISCOVERY_PORT     4210

static void json_escape(char *out, size_t size, const char *in);

static const char *TAG = "baby_network";
static EventGroupHandle_t wifi_events;
static baby_command_handler_t command_handler;
static baby_backup_export_handler_t backup_export_handler;
static baby_backup_import_handler_t backup_import_handler;
static char access_token[20];
static char ap_name[32];
static int retry_count;
static bool recovery_ap_started;
/*
 * HTTP server requests run on a single task. Keep the large command/JSON
 * workspaces out of its 8 KB stack; STATUS on page load otherwise overflows
 * the task before it can reply.
 */
static char command_response[2048];
static char command_escaped[2300];
static char command_json[2400];
static char backup_response[6144];

static bool request_authorized(httpd_req_t *request)
{
    char supplied[32] = {0};
    if (httpd_req_get_hdr_value_str(request, "X-Quote0-Token",
                                    supplied, sizeof(supplied)) == ESP_OK &&
        strcmp(supplied, access_token) == 0) {
        return true;
    }

    char query[96] = {0};
    char token[32] = {0};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "token", token, sizeof(token)) == ESP_OK &&
        strcmp(token, access_token) == 0) {
        return true;
    }
    return false;
}

static esp_err_t send_json(httpd_req_t *request, const char *body)
{
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    return httpd_resp_sendstr(request, body);
}

/* 扫描结果缓存（一次性后台任务填充，前端轮询读取） */
static char scan_cache[2048] = "{\"networks\":[]}";
static volatile bool scan_done = true;
/* The scan task has a 4 KB stack. Keep AP records and JSON off that stack. */
static wifi_ap_record_t scan_records[20];
static char scan_body[2048];

static void wifi_scan_task(void *unused)
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {.active = {.min = 100, .max = 150}},
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    uint16_t count = 0;
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&count);
        if (count > 20) count = 20;
        memset(scan_records, 0, sizeof(scan_records));
        if (count > 0) {
            esp_wifi_scan_get_ap_records(&count, scan_records);
        }
        int written = snprintf(scan_body, sizeof(scan_body),
                               "{\"networks\":[");
        for (uint16_t i = 0;
             i < count && written < (int)sizeof(scan_body) - 32; i++) {
            char ssid_esc[40] = {0};
            int len = strnlen((char *)scan_records[i].ssid, 32);
            for (int c = 0; c < len && c < 38; c++) {
                if (scan_records[i].ssid[c] == '\\' ||
                    scan_records[i].ssid[c] == '"') {
                    ssid_esc[c] = ' ';
                } else {
                    ssid_esc[c] = (char)scan_records[i].ssid[c];
                }
            }
            int rssi = scan_records[i].rssi;
            written += snprintf(scan_body + written,
                                sizeof(scan_body) - written,
                                "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                                i ? "," : "", ssid_esc, rssi,
                                (int)scan_records[i].authmode);
        }
        snprintf(scan_body + written, sizeof(scan_body) - written, "]}");
        snprintf(scan_cache, sizeof(scan_cache), "%s", scan_body);
    } else {
        snprintf(scan_cache, sizeof(scan_cache),
                 "{\"networks\":[],\"error\":\"scan_failed\"}");
    }
    scan_done = true;
    vTaskDelete(NULL);
}

static esp_err_t wifi_scan_handler(httpd_req_t *request)
{
    if (!request_authorized(request)) {
        httpd_resp_set_status(request, "401 Unauthorized");
        return send_json(request, "{\"error\":\"unauthorized\"}");
    }
    if (scan_done) {
        scan_done = false;
        if (xTaskCreate(wifi_scan_task, "wifi_scan", 4096,
                        NULL, 3, NULL) != pdPASS) {
            scan_done = true;
            httpd_resp_set_status(request, "503 Service Unavailable");
            return send_json(request, "{\"error\":\"scan_task_failed\"}");
        }
    }
    return send_json(request, "{\"scanning\":true}");
}

static esp_err_t wifi_scan_result_handler(httpd_req_t *request)
{
    if (!request_authorized(request)) {
        httpd_resp_set_status(request, "401 Unauthorized");
        return send_json(request, "{\"error\":\"unauthorized\"}");
    }
    if (!scan_done) {
        return send_json(request, "{\"networks\":[],\"done\":false}");
    }
    return send_json(request, scan_cache);
}

#define OTA_CHUNK_SIZE 4096
#define OTA_RECV_BUF   2048
#define BABY_OTA_IMAGE_MAGIC 0xE9

static esp_err_t ota_http_handler(httpd_req_t *request)
{
    if (!request_authorized(request)) {
        httpd_resp_set_status(request, "401 Unauthorized");
        return send_json(request, "{\"error\":\"unauthorized\"}");
    }

    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_set_status(request, "500 Internal Server Error");
        return send_json(request, "{\"error\":\"no_ota_partition\"}");
    }

    if (request->content_len < 1024 ||
        request->content_len > (int)update_partition->size) {
        httpd_resp_set_status(request, "413 Content Too Large");
        return send_json(request, "{\"error\":\"invalid_firmware_size\"}");
    }

    esp_err_t err = esp_ota_begin(update_partition, request->content_len,
                                  &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_set_status(request, "500 Internal Server Error");
        return send_json(request, "{\"error\":\"ota_begin_failed\"}");
    }

    char buffer[OTA_RECV_BUF];
    int remaining = request->content_len;
    int received_total = 0;
    bool failed = false;

    while (remaining > 0) {
        int received = httpd_req_recv(request, buffer,
                                      remaining < (int)sizeof(buffer)
                                          ? remaining
                                          : (int)sizeof(buffer));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            failed = true;
            break;
        }
        if (received_total == 0 &&
            (unsigned char)buffer[0] != BABY_OTA_IMAGE_MAGIC) {
            failed = true;
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = esp_ota_write(ota_handle, buffer, received);
        if (err != ESP_OK) {
            failed = true;
            break;
        }
        received_total += received;
        remaining -= received;
    }

    if (!failed) {
        err = esp_ota_end(ota_handle);
        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(update_partition);
        }
        if (err != ESP_OK) failed = true;
    }

    if (failed) {
        esp_ota_abort(ota_handle);
        httpd_resp_set_status(request, "500 Internal Server Error");
        return send_json(request, "{\"error\":\"ota_write_failed\"}");
    }

    ESP_LOGI(TAG, "OTA done: %d bytes -> %s, rebooting",
             received_total, update_partition->label);
    send_json(request, "{\"ok\":true,\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t ping_handler(httpd_req_t *request)
{
    char body[160];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(body, sizeof(body),
             "{\"device\":\"QUOTE0_BABY\",\"api_version\":4,"
             "\"firmware_version\":\"%s\",\"project\":\"%s\"}",
             app->version, app->project_name);
    return send_json(request, body);
}

static esp_err_t command_http_handler(httpd_req_t *request)
{
    if (!request_authorized(request)) {
        httpd_resp_set_status(request, "401 Unauthorized");
        return send_json(request, "{\"error\":\"unauthorized\"}");
    }
    if (request->content_len <= 0 || request->content_len >= 96) {
        httpd_resp_set_status(request, "400 Bad Request");
        return send_json(request, "{\"error\":\"invalid command\"}");
    }

    char command[96] = {0};
    int received = httpd_req_recv(request, command, request->content_len);
    if (received <= 0) {
        httpd_resp_set_status(request, "400 Bad Request");
        return send_json(request, "{\"error\":\"read failed\"}");
    }
    command[received] = '\0';

    command_response[0] = '\0';
    esp_err_t err = command_handler(command, command_response,
                                    sizeof(command_response));
    json_escape(command_escaped, sizeof(command_escaped), command_response);
    snprintf(command_json, sizeof(command_json),
             "{\"ok\":%s,\"response\":\"%s\"}",
             err == ESP_OK ? "true" : "false", command_escaped);
    if (err != ESP_OK) httpd_resp_set_status(request, "400 Bad Request");
    return send_json(request, command_json);
}

static esp_err_t backup_export_http_handler(httpd_req_t *request)
{
    if (!request_authorized(request)) {
        httpd_resp_set_status(request, "401 Unauthorized");
        return send_json(request, "{\"error\":\"unauthorized\"}");
    }
    esp_err_t err = backup_export_handler(backup_response,
                                          sizeof(backup_response));
    if (err != ESP_OK) {
        httpd_resp_set_status(request, "500 Internal Server Error");
        return send_json(request, "{\"error\":\"backup_failed\"}");
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Content-Disposition",
                       "attachment; filename=quote0-baby-backup.json");
    return httpd_resp_sendstr(request, backup_response);
}

static esp_err_t backup_import_http_handler(httpd_req_t *request)
{
    if (!request_authorized(request)) {
        httpd_resp_set_status(request, "401 Unauthorized");
        return send_json(request, "{\"error\":\"unauthorized\"}");
    }
    if (request->content_len <= 0 || request->content_len > 8192) {
        httpd_resp_set_status(request, "413 Content Too Large");
        return send_json(request, "{\"error\":\"invalid_backup_size\"}");
    }
    char *body = malloc((size_t)request->content_len + 1);
    if (!body) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return send_json(request, "{\"error\":\"no_memory\"}");
    }
    int total = 0;
    while (total < request->content_len) {
        int received = httpd_req_recv(request, body + total,
                                      request->content_len - total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (received <= 0) {
            free(body);
            httpd_resp_set_status(request, "400 Bad Request");
            return send_json(request, "{\"error\":\"read_failed\"}");
        }
        total += received;
    }
    body[total] = '\0';
    char result[160] = {0};
    esp_err_t err = backup_import_handler(body, (size_t)total, result,
                                          sizeof(result));
    free(body);
    char escaped[220] = {0};
    json_escape(escaped, sizeof(escaped), result);
    snprintf(command_json, sizeof(command_json),
             "{\"ok\":%s,\"message\":\"%s\"}",
             err == ESP_OK ? "true" : "false", escaped);
    if (err != ESP_OK) httpd_resp_set_status(request, "400 Bad Request");
    return send_json(request, command_json);
}

static esp_err_t manifest_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "application/manifest+json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=86400");
    return httpd_resp_sendstr(request,
        "{\"name\":\"Hermes Studio 喂养宝宝\","
        "\"short_name\":\"宝宝记录\",\"lang\":\"zh-CN\","
        "\"start_url\":\"/\",\"scope\":\"/\","
        "\"display\":\"standalone\",\"background_color\":\"#e9efda\","
        "\"theme_color\":\"#547b5f\",\"icons\":[{\"src\":\"/icon.svg\","
        "\"sizes\":\"any\",\"type\":\"image/svg+xml\","
        "\"purpose\":\"any maskable\"}]}");
}

static esp_err_t icon_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "image/svg+xml");
    httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=86400");
    return httpd_resp_sendstr(request,
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 192 192'>"
        "<rect width='192' height='192' rx='42' fill='#e9efda'/>"
        "<circle cx='96' cy='92' r='58' fill='#fffaf0' stroke='#547b5f' stroke-width='8'/>"
        "<path d='M54 75Q64 34 96 42Q134 34 140 78Q126 62 112 68Q91 52 72 70Z' fill='#29483a'/>"
        "<circle cx='76' cy='94' r='7' fill='#29483a'/><circle cx='116' cy='94' r='7' fill='#29483a'/>"
        "<path d='M79 119Q96 132 113 119' fill='none' stroke='#d8896c' stroke-width='7' stroke-linecap='round'/>"
        "</svg>");
}

static esp_err_t offline_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_sendstr(request,
        "<!doctype html><html lang=zh-CN><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta name=theme-color content='#547b5f'>"
        "<meta name=apple-mobile-web-app-capable content=yes>"
        "<meta name=apple-mobile-web-app-title content='宝宝记录'>"
        "<link rel=manifest href='/manifest.webmanifest'>"
        "<link rel=apple-touch-icon href='/icon.svg'>"
        "<title>宝宝看板离线</title><body style='font-family:serif;background:#e9efda;"
        "color:#29483a;padding:12vh 8vw;text-align:center'><h1>暂时连不上宝宝看板</h1>"
        "<p>请确认手机与 Quote/0 连接同一个 Wi-Fi，然后重新打开。</p></body></html>");
}

static esp_err_t service_worker_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
    return httpd_resp_sendstr(request,
        "const C='quote0-baby-shell-v1';"
        "self.addEventListener('install',e=>e.waitUntil(caches.open(C).then(c=>"
        "c.addAll(['/offline','/manifest.webmanifest','/icon.svg']))));"
        "self.addEventListener('activate',e=>e.waitUntil(self.clients.claim()));"
        "self.addEventListener('fetch',e=>{if(e.request.mode==='navigate')"
        "e.respondWith(fetch(e.request).catch(()=>caches.match('/offline')))});");
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    value = (char)tolower((unsigned char)value);
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

/* 把字符串转义成 JSON 安全形式（引号、反斜杠、控制字符） */
static void json_escape(char *out, size_t size, const char *in)
{
    if (size == 0) return;
    size_t written = 0;
    for (const char *p = in; *p && written + 6 < size; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  out[written++] = '\\'; out[written++] = '"';  break;
        case '\\': out[written++] = '\\'; out[written++] = '\\'; break;
        case '\n': out[written++] = '\\'; out[written++] = 'n';  break;
        case '\r': out[written++] = '\\'; out[written++] = 'r';  break;
        case '\t': out[written++] = '\\'; out[written++] = 't';  break;
        default:
            if (c < 0x20) {
                written += snprintf(out + written, size - written,
                                    "\\u%04x", c);
            } else {
                out[written++] = (char)c;
            }
        }
    }
    out[written] = '\0';
}

static void url_decode(char *destination, size_t size, const char *source)
{
    size_t written = 0;
    while (*source && written + 1 < size) {
        if (*source == '+') {
            destination[written++] = ' ';
            ++source;
        } else if (*source == '%' && source[1] && source[2]) {
            int high = hex_value(source[1]);
            int low = hex_value(source[2]);
            if (high >= 0 && low >= 0) {
                destination[written++] = (char)((high << 4) | low);
                source += 3;
            } else {
                destination[written++] = *source++;
            }
        } else {
            destination[written++] = *source++;
        }
    }
    destination[written] = '\0';
}

static bool form_value(const char *body, const char *key,
                       char *destination, size_t size)
{
    size_t key_len = strlen(key);
    const char *cursor = body;
    while (cursor && *cursor) {
        if (strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            cursor += key_len + 1;
            const char *end = strchr(cursor, '&');
            size_t value_len = end ? (size_t)(end - cursor) : strlen(cursor);
            char encoded[128];
            if (value_len >= sizeof(encoded)) return false;
            memcpy(encoded, cursor, value_len);
            encoded[value_len] = '\0';
            url_decode(destination, size, encoded);
            return true;
        }
        cursor = strchr(cursor, '&');
        if (cursor) ++cursor;
    }
    return false;
}

static void restart_task(void *unused)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t wifi_config_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len >= 256) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_sendstr(request, "参数错误");
    }
    char body[256] = {0};
    int received = httpd_req_recv(request, body, request->content_len);
    if (received <= 0) return ESP_FAIL;
    body[received] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    if (!form_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0' ||
        !form_value(body, "password", password, sizeof(password)) ||
        strlen(password) > 63) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_sendstr(request, "SSID 或密码无效");
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("baby_wifi", NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "password", password);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    if (err != ESP_OK) {
        httpd_resp_set_status(request, "500 Internal Server Error");
        return httpd_resp_sendstr(request, "保存失败");
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_sendstr(request,
        "<meta charset=utf-8><h2>保存成功</h2>"
        "<p>设备正在连接家庭 Wi-Fi，请稍后关闭此页面。</p>");
    xTaskCreate(restart_task, "wifi_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t root_handler(httpd_req_t *request)
{
    static char page[28000];
    snprintf(page, sizeof(page),
        "<!doctype html><html lang=zh-CN><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta name=theme-color content='#547b5f'>"
        "<meta name=apple-mobile-web-app-capable content=yes>"
        "<meta name=apple-mobile-web-app-title content='宝宝记录'>"
        "<link rel=manifest href='/manifest.webmanifest'>"
        "<link rel=apple-touch-icon href='/icon.svg'>"
        "<title>Hermes Studio 喂养宝宝</title><style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        ":root{--ink:#29483a;--leaf:#547b5f;--moss:#87a77d;--sky:#ddecdf;"
        "--paper:#fffaf0;--peach:#d8896c;--line:#b9cbb7}"
        "body{font-family:'PingFang SC','Hiragino Sans GB',sans-serif;color:var(--ink);"
        "min-height:100dvh;padding:22px;background:linear-gradient(180deg,"
        "#cfe3e2 0,#e9efda 48%%,#b8d09e 100%%);background-attachment:fixed}"
        "body:before,body:after{content:'';position:fixed;z-index:-1;border-radius:50%%;"
        "filter:blur(1px);opacity:.55}"
        "body:before{width:420px;height:180px;background:#fff9e8;right:-90px;top:-75px}"
        "body:after{width:620px;height:240px;background:#86aa78;left:-180px;bottom:-155px}"
        ".wrap{width:100%%;max-width:1380px;margin:auto}"
        "header{height:68px;display:flex;align-items:center;gap:13px;padding:0 8px;"
        "margin-bottom:16px}"
        "h1{font-family:'Kaiti SC','STKaiti',serif;font-size:28px;font-weight:800;"
        "letter-spacing:1px;color:#29483a}"
        "header:after{content:'在风里慢慢长大';font-family:'Kaiti SC','STKaiti',serif;"
        "color:#66846b;font-size:14px;margin-left:6px}"
        ".logo{width:52px;height:52px;flex:0 0 52px;border:2px solid #416b50;"
        "border-radius:48%% 52%% 45%% 55%%;object-fit:cover;background:#fff;"
        "box-shadow:3px 4px 0 #6d8d6b55;transform:rotate(-3deg)}"
        ".dot{width:11px;height:11px;border-radius:50%%;background:#4f8d57;"
        "box-shadow:0 0 0 5px #4f8d5725;margin-left:auto}"
        ".dot.off{background:#bd695b;box-shadow:0 0 0 5px #bd695b25}"
        ".sync{font-size:12px;color:#66846b;white-space:nowrap}.sync.off{color:#a45d52}"
        ".layout{display:grid;grid-template-columns:minmax(0,.95fr) minmax(0,1.05fr);"
        "gap:18px;align-items:start}"
        ".pane{min-width:0}"
        ".card{background:#fffaf0e8;border:1px solid #a9bfa7;border-radius:22px 18px 24px 17px;"
        "padding:18px;margin-bottom:16px;box-shadow:5px 7px 0 #63806720;"
        "backdrop-filter:blur(8px)}"
        "h2,.fold{font-family:'Kaiti SC','STKaiti',serif;font-size:18px;color:#365c47;"
        "font-weight:800;margin-bottom:13px;display:flex;align-items:center;gap:9px}"
        "h2:before,.fold:before{content:'✦';color:#d8896c;font-size:13px}"
        ".fold{cursor:pointer;list-style:none;margin:0}.fold::-webkit-details-marker{display:none}"
        ".fold:after{content:'＋';margin-left:auto;color:#6b8b71}"
        "details[open]>.fold{margin-bottom:15px}"
        "details[open]>.fold:after{content:'－'}"
        ".grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:11px}"
        ".stat{background:#edf4e5;border:1px dashed #9db49a;border-radius:16px 12px 17px 13px;"
        "padding:13px;transition:transform .15s,background .15s}"
        ".stat:active{transform:translateY(1px);background:#e3eed9}"
        ".stat .k{font-size:12px;color:#6d876f;margin-bottom:5px;font-weight:600}"
        ".stat .v{font-size:18px;font-weight:800;color:#29483a}"
        ".tag{display:inline-block;padding:4px 10px;border-radius:999px;font-size:12px;"
        "font-weight:600;margin-top:6px;background:#dce9d7;color:#41654c}"
        ".tag.pink{background:#f3dcd1;color:#995d4a}"
        ".btn{display:block;width:100%%;min-height:45px;padding:11px 13px;border:1px solid #365d46;"
        "border-radius:14px 11px 15px 12px;font-size:14px;font-weight:700;color:#fff;"
        "margin-bottom:9px;cursor:pointer;font-family:inherit;background:#547b5f;"
        "box-shadow:2px 3px 0 #2f5140;transition:transform .12s,box-shadow .12s}"
        ".btn:active{transform:translate(2px,2px);box-shadow:none}"
        ".btn.g{background:#547b5f}.btn.b{background:#64869a}.btn.y{background:#be8e55}"
        ".btn.p{background:#c87865}.btn.s{background:#718078}.btn.t{background:#6d8f77}"
        ".row{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-bottom:9px}"
        ".row .btn{margin-bottom:0}"
        "input,select{width:100%%;padding:11px 12px;border-radius:12px 9px 13px 10px;"
        "border:1.5px solid #a9bca7;background:#fffdf5;color:#29483a;font-size:15px;"
        "font-family:inherit;margin-bottom:10px;outline:none}"
        "input:focus,select:focus{border-color:#547b5f;box-shadow:0 0 0 3px #547b5f20}"
        "label{font-size:12px;color:#637c67;font-weight:700;display:block;margin-bottom:5px}"
        ".minilabel{font-size:12px;color:#6d826f;margin:2px 0 6px;display:block;line-height:1.7}"
        "#summary{font-size:14px;color:#365944;padding:3px 2px 10px}"
        ".range,.metrics{display:flex;gap:7px;overflow-x:auto;padding:2px 1px 8px;"
        "scrollbar-width:none}.range::-webkit-scrollbar,.metrics::-webkit-scrollbar{display:none}"
        ".chip{flex:0 0 auto;border:1px solid #9eb49a;background:#f0f4e8;color:#55715c;"
        "border-radius:10px 8px 11px 9px;padding:7px 11px;font:700 12px inherit;cursor:pointer}"
        ".chip.on{background:#547b5f;color:#fff;border-color:#42654d;box-shadow:2px 2px 0 #29483a}"
        ".chart-head{display:flex;align-items:flex-end;justify-content:space-between;"
        "gap:12px;margin:4px 2px 8px}.chart-title{font-size:13px;font-weight:800}"
        ".chart-total{font-size:12px;color:#708271;text-align:right}"
        ".chart{height:142px;display:flex;align-items:flex-end;gap:5px;padding:12px 4px 0;"
        "border-bottom:1px dashed #9db49a;overflow-x:auto;scrollbar-width:none}"
        ".chart::-webkit-scrollbar{display:none}.baritem{height:100%%;min-width:22px;flex:1;"
        "display:flex;flex-direction:column;justify-content:flex-end;align-items:center;gap:5px}"
        ".barvalue{font-size:10px;color:#53705b;line-height:1;min-height:10px}"
        ".bar{width:min(22px,78%%);min-height:3px;background:#d8896c;"
        "border-radius:7px 7px 3px 3px;box-shadow:inset 0 1px 0 #fff7}"
        ".bar.zero{height:3px!important;background:#bdcbb5;box-shadow:none}"
        ".barlabel{font-size:9px;color:#718574;white-space:nowrap;line-height:1.2}"
        ".chart-empty{height:142px;display:grid;place-items:center;color:#718574;"
        "font-size:13px;border:1px dashed #a8bba4;border-radius:13px;background:#f2f5e8}"
        "#hist{background:#eef4e6;border:1px dashed #9fb49c;border-radius:14px;"
        "padding:12px;font-size:13px;line-height:1.9;color:#3f5e48;white-space:pre-wrap;"
        "max-height:270px;overflow:auto}"
        "#toast{position:fixed;bottom:24px;left:50%%;transform:translateX(-50%%);"
        "background:#29483a;color:#fff;padding:11px 22px;border-radius:999px;font-size:14px;"
        "font-weight:600;opacity:0;transition:opacity .3s;pointer-events:none;z-index:9;"
        "box-shadow:0 6px 20px #29483a44}"
        "#toast.err{background:#a94f45}"
        ".net{display:flex;align-items:center;gap:10px;padding:11px 12px;border:1px solid #aabda7;"
        "border-radius:13px;margin-bottom:7px;cursor:pointer;background:#f2f5e8}"
        ".net:active{border-color:#547b5f;background:#e6efdc}"
        ".net .sig{font-size:11px;color:#718574;min-width:34px;text-align:right}"
        ".net .name{flex:1;font-weight:600;font-size:14px;color:#29483a;overflow:hidden;"
        "text-overflow:ellipsis;white-space:nowrap}.net .lock{font-size:12px}"
        ".wifi-head{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}"
        ".wifi-head span{font-size:12px;color:#6d826f}"
        ".backup-actions{display:grid;grid-template-columns:1fr 1fr;gap:9px}"
        ".backup-actions .btn{margin:0}.hint{font-size:12px;line-height:1.7;color:#6d826f;"
        "margin-top:10px}"
        ".ota-file{border:1px dashed #9db49a;padding:12px;background:#f2f5e8}"
        ".progress{height:10px;border-radius:999px;background:#dce6d7;overflow:hidden;"
        "margin:4px 0 10px}.progress>i{display:block;width:0;height:100%%;background:#64869a;"
        "transition:width .2s}.ota-state{font-size:12px;color:#637c67;margin-bottom:8px}"
        "@media(max-width:820px){body{padding:12px}header{height:58px;margin-bottom:10px}"
        "h1{font-size:20px;letter-spacing:0}header:after{display:none}"
        ".logo{width:44px;height:44px;flex-basis:44px}"
        ".layout{grid-template-columns:1fr;gap:0}.card{padding:14px;margin-bottom:12px;"
        "border-radius:17px}.left,.right{display:contents}.quick{order:-1}"
        ".grid{gap:8px}.stat{padding:11px}.stat .v{font-size:16px}"
        ".row{gap:7px}.btn{font-size:13px;padding:10px 8px}}"
        "</style></head><body><div class=wrap>"
        "<header><img class=logo alt='Hermes Studio 宠物' src='data:image/webp;base64,UklGRoQIAABXRUJQVlA4WAoAAAAIAAAAXwAAXwAAVlA4IBoIAADwIQCdASpgAGAAPmEqkEWkIqGWrVUkQAYEs4BpotGkA2wG4Y3hH0AOk3wADp9eDPkh9I+z3HW558wv5F93fyfsd7CeAF7D/v28EgA+qP+p4xPsL5qv+142/zv2APzD/uv7x+UfyPf8PlE+m/+z/lfgH/l/9c/4v9/9s/2IfuT7NX7QNr18yh6sqV1jp6l7eEwOp39Kqd9sPkv9ik5eGe8KEenKSmmBrb9uTd8TIn+W5lQPDXsOCI1iKVyZNlK30P1wBMTBHevh1K1DmaKJWKJpAC8EZNH3w3SeZoRr+DbeRaq2LktioFGc3TSYZstNMX1FVDLfXh6R68cLLedo0x8iHFfl+F/uTxL+lUaLLL66mgu3mHJ6cWVgAP7+clMFnfws0Fu/222zQlFslc+856bj4XpQEqMztJMn4s1QT2L2muViV32q0GjzrbIfD8baF4Dh/VTH2yHDK6ZibHC6AtbsXe4pJaTUSOf6Ykn5Ia4FTNcLItInTdfCUt9LvZq2ls/uosAs7nMwwn0dmYpR3kDAQhdNUV7BEr0Wapvmclf9g7X+J4LCl73ETjGLtB+kvSf0xsQ9qPJE+XpdkXtl6qKiqsouUX5l3ijVBXjnf0xFSIG3vji/FXy+IsfdDzcXwfVWrie7u/r5CyzkoPJEEQNDAAJ+feufILvnyp8hSbZ3H9aAeoIodvrZISVCBYxACoBSMEcQOWgWgRJxIHYML20/iyiMr3+l3mUq/bWCc4CslCNU8FOdeyTsPKDw94XV+G8nYTZdbqeUUZ8Ecw+PZIzX6VI1D3rXCnK5ggeNDFzqHf8xg+PNq/ilBPabLw2ZzCy6BHciGf0bwRfOobGjU9WNmnaG8Bifig1ODBOHm+/i6956wvxMk1D+f/KHKcKEJ5qqzHLOlrze9ECfR4DtbD+AWsQ+ozdexjvNqfI9iHz+Y/4/4/50UYMFxktvc+xqjWKXqbZ8Viihw5IdRZG//4hgWqLrWlZnboArcnDjPOD+DoaRbNDEDql13ZfQp1BUz4z/IR7DAOYdm0ttNkTxCCegGalx8RFWjlPCF9DkGlOGQFqzKmaEV99NLwX//hkYS4QOI3DzuTKCfZT6UzKOPsSeJyjsDS6ny8rwqYYBrXXWUaKeOplIKhA8vlytbjyyJUyIVrafRjP7oPf29+rGB+G33isYf9s/oZx6U1UCxQB4buLLX2zwwCLyTCWmtiJQOFseWQilNwK/G2UNO8I7FAT8dYIp3m64NN4GzbtZNUcyyAncdikuWKXysZTQbq3XZzjtqm7zF7CKCdIV0p+WI8T8onwEjkr32/hiN1wP0e3EcntgDSHRTeDib+V1P4FgC7xlfdzu+nT/OrAyYWtjCRfH45npYhmdp49ZIzUO0KDTvWJYaLM4Zvmwmf9hiSkKBcShvOMtb8haV+35SMGPzAXEAXLITG2C/Uy+JgB/74ImpA+arPCBVNQsyJ2RVXTU7GWods3JnEx7hpoUeymLYC3irXct/E4dwf7qfqdKCbB3wxfbW/xEjgknKStSYUru87UYITeo2aAkk3dj5++Rdz9uiAxDmBSL0GP7+UIW7RMDcZ8X0tjPwurVohiozBchFJ8koC5if+5f5XOdJ5iW9O/6cKWnUsbz28HargBhyJrI+uAmV2hhGSdcgS5EnWwNaeZYtjnnYOUE4yMzDX4S0TqVndFpw46FDwUqkzNr/kupPzyEu/8ttiy+MVESSrjqnHxE6V+4ZFbeJz0eOiapc3CrUDk7cszw1T5ZdTA4DaT+kFH3D4x8zdEN1UsvkDVB8pdyoyRqv2Xv/OJeKeed7nmg/wS4B+cpU8ClgOCs/y8eMHp7tEJdmEX6FqjjCDv8Dp2ROzlp6/emriUwP+//dbsLlgHlrFeP/2ZyIYb8nUsn+h7AX2hGWhl2dA6qXSt2hsfOQott6HConoWMAzegOgDZ0lzXqWU8Rrv6aDeLJBsoyaVYPXp6U9Qst/6WMgYoD18y7Q7ofYsavlrsnFoMCZWRwFUix+4sQnP5VxeqJibTqUBoBcFYkrHDzEItHW9MtUcvUgCdCbU7ZtsYAAVu5GIp/45nbDjPQzyGgciQg2EYz/gSdfWLt/KF9CDCRWMmFHmpB0pFtX0M19MbA4qMgUaPwzFFZ05C/47K2XBUngzgNRZd2060eOB7SGq02SSEAlBoZ9jdW60h+BygltsFG30JUzJVsWdRJU7l8uPe2QxRc/Dy2VzyTS2+k6tV1exzEidsjqshCK0/L6dMLQnODQ5owFnx75dzdwTaPt1NpJlN/ZGIWuk85cDc0OGzIzjweG+kevEO4rrm7lb3qMfyyquQYeTHFz5vnrTLEc4oO102w2/dDuFR/UGpkcBDCpXFEKeAA8PR8OLi0xxAaBdXXpph+NkIYZsBI0myTYAQmbf/0u7DdL5+ZcCX/fszVMlgxdN6kYsNDp9NcBVjQ+pkHwxiLll/kFci6X2UxtFwzK3/lTgf238RQTCyLi0oCK7WRUtyS0W/FuFK/vuK5yKYz/IEEgEUVq9YJCihZMmMsLPHOV70KAkM2PPc1ggF9bw3SY0NOaJ3KAa0/LjL/p7k2JXa3SilmQl7WdMx0p10qQjRHRD7MHuj0gHCns075CtdOODyVv+cmEEeB23EBuOK9cgIHHxnkSBcxlDBEjANfxIM0aik69WfD455rgvBp2Lfgx/DHLlr3e4OlX5meRLRY5bCkhoE2LV6AHr3jRBluCtvfOyD6/5QNldXxHCvqa8GT1Fa8RqDXQpDgAAARVhJRkQAAABNTQAqAAAACAABh2kABAAAAAEAAAAaAAAAAAADoAEAAwAAAAEAAQAAoAIABAAAAAEAAAGQoAMABAAAAAEAAAGQAAAAAA=='>"
        "<h1>Hermes Studio 喂养宝宝</h1>"
        "<div class=dot id=dot></div><span class=sync id=sync>正在同步</span></header>"
        "<main class=layout><section class='pane left'>"
        "<div class=card><h2>实时状态</h2><div class=grid id=stats>"
        "<div class=stat><div class=k>出生天数</div><div class=v id=s-day>--</div></div>"
        "<div class=stat><div class=k>最近喂奶</div><div class=v id=s-feed>--:--</div></div>"
        "<div class=stat><div class=k>睡眠</div><div class=v id=s-sleep>--</div></div>"
        "<div class=stat><div class=k>尿布</div><div class=v id=s-diaper>--</div></div>"
        "<div class=stat style='grid-column:1/-1'><div class=k>下次提醒</div>"
        "<div class=v id=s-next>--:--</div><div class=tag id=s-next-tag></div></div>"
        "</div></div>"
        "<div class=card><h2>成长数据</h2>"
        "<div class=range id=ranges>"
        "<button class='chip on' data-days=1>今日</button><button class=chip data-days=2>2天</button>"
        "<button class=chip data-days=4>4天</button><button class=chip data-days=6>6天</button>"
        "<button class=chip data-days=30>一个月</button></div>"
        "<div id=summary class=minilabel>加载中...</div>"
        "<div class=metrics id=metrics>"
        "<button class='chip on' data-metric=ml>奶量</button><button class=chip data-metric=feed>喂奶次数</button>"
        "<button class=chip data-metric=diaper>尿布</button><button class=chip data-metric=poop>便便</button>"
        "<button class=chip data-metric=sleep>睡眠</button></div>"
        "<div class=chart-head><span class=chart-title id=chart-title>奶量趋势</span>"
        "<span class=chart-total id=chart-total></span></div><div class=chart id=chart></div></div>"
        "<div class=card><h2>最近记录</h2>"
        "<div id=hist>加载中...</div>"
        "<button class='btn s' style='margin-top:10px' onclick='loadHist()'>🔄 刷新记录</button></div>"
        "</section><section class='pane right'>"
        "<div class='card quick'><h2>快速记录</h2>"
        "<label>喂奶量 (ml)</label><input type=number id=ml value=80 min=0 max=999>"
        "<button class='btn g' onclick=\"send('FEED '+now()+' '+ml.value)\">🍼 记录喂奶</button>"
        "<div class=row><button class='btn b' onclick=\"send('SLEEP '+now()+' ON')\">😴 开始睡眠</button>"
        "<button class='btn y' onclick=\"send('SLEEP '+now()+' OFF')\">🌞 结束睡眠</button></div>"
        "<div class=row><button class='btn y' onclick=\"send('DIAPER '+now()+' W')\">🧷 尿布·尿</button>"
        "<button class='btn p' onclick=\"send('DIAPER '+now()+' D')\">🧷 尿布·便</button></div>"
        "<div class=row><button class='btn t' onclick=\"send('DIAPER '+now()+' WD')\">🧷 尿布·都有</button>"
        "<button class='btn s' onclick=\"send('UNDO')\">↩️ 撤销上一条</button></div>"
        "</div>"
        "<details class=card><summary class=fold>提醒与设置</summary><div>"
        "<label>出生天数</label><input type=number id=day value=9 min=0 max=999>"
        "<button class='btn b' onclick=\"send('DAY '+day.value)\">📅 更新天数</button>"
        "<label>下次提醒时间</label><input type=time id=ntime value='21:06'>"
        "<label>提醒类型</label><select id=nkind>"
        "<option value=FEED>🍼 喂奶</option><option value=SLEEP>😴 睡眠</option>"
        "<option value=DIAPER>🧷 尿布</option><option value=MED>💊 用药</option>"
        "</select><button class='btn t' onclick=\"send('NEXT '+ntime.value+' '+nkind.value)\">⏰ 设置提醒</button>"
        "<label>喂奶间隔（分钟）</label><input type=number id=interval value=180 min=30 max=720>"
        "<button class='btn t' onclick=\"send('INTERVAL '+interval.value)\">⏲️ 保存间隔</button>"
        "<label>疫苗提醒日期 (MM-DD)</label><input id=vdate placeholder='08-08'>"
        "<label>疫苗提醒时间</label><input type=time id=vtime value='15:00'>"
        "<button class='btn p' onclick=\"send('REMIND '+vdate.value+' '+vtime.value+' SHOT')\">💉 设置疫苗提醒</button>"
        "</div></details>"
        "<details class=card><summary class=fold>Wi-Fi 设置</summary><div>"
        "<div class=wifi-head><span>附近网络（点击选择）</span>"
        "<button class='btn s' style='width:auto;padding:7px 14px;margin:0;font-size:12px' onclick='scanWifi()'>📡 重新扫描</button></div>"
        "<div id=netlist><span class=minilabel>点击扫描查看附近 Wi-Fi</span></div>"
        "<form id=wifiForm style='display:none;margin-top:10px'>"
        "<label>Wi-Fi 名称</label><input id=ssid readonly>"
        "<label>密码</label><input id=wpass type=password placeholder='输入密码'>"
        "<button class='btn b' type=submit>连接此网络</button></form>"
        "</div></details>"
        "<details class=card><summary class=fold>数据备份</summary><div>"
        "<div class=backup-actions><button class='btn t' onclick='downloadBackup()'>"
        "⬇️ 导出备份</button><button class='btn y' onclick=\"document.getElementById('restoreFile').click()\">"
        "⬆️ 恢复备份</button></div>"
        "<input id=restoreFile type=file accept='.json,application/json' style='display:none'>"
        "<p class=hint>备份包含当前状态、喂奶间隔与最近 32 条记录。恢复前会完整校验文件，"
        "成功后自动刷新墨水屏。</p></div></details>"
        "<details class=card><summary class=fold>宝宝社区固件更新</summary><div>"
        "<p class=minilabel>当前版本：<b id=fwver>读取中...</b>。仅上传本项目 Release 提供的 "
        "<code>quote0_baby.bin</code>，升级时请保持设备供电。</p>"
        "<input class=ota-file id=otaFile type=file accept='.bin,application/octet-stream'>"
        "<div class=progress><i id=otaBar></i></div><div class=ota-state id=otaState>等待选择固件</div>"
        "<button class='btn t' id=onlineButton onclick='checkCommunityUpdate()'>🌿 从 GitHub 在线更新</button>"
        "<button class='btn b' id=otaButton onclick='uploadFirmware()'>⬆️ 上传并更新固件</button>"
        "<p class=hint>在线更新由手机浏览器直接读取本项目 GitHub 固件，再传给设备。成功"
        "后设备自动重启，宝宝记录保留在设备中。请勿上传原厂固件、"
        "其他型号固件或中途断电。</p></div></details></section></main>"
        "</div><div id=toast></div><script>"
        "const token='%s';"
        "function now(){return new Date().toTimeString().slice(0,5)}"
        "function toast(m,e){const t=document.getElementById('toast');t.textContent=m;"
        "t.className=e?'err':'';t.style.opacity=1;setTimeout(()=>t.style.opacity=0,2200)}"
        "const L={FEED:'🍼 喂奶',SLEEP:'😴 睡眠',DIAPER:'🧷 尿布',MED:'💊 用药',SHOT:'💉 预防针'};"
        "const D={W:'尿',D:'便',WD:'尿+便',ON:'睡中',OFF:'醒着'};"
        "const M={ml:{name:'奶量趋势',unit:'ml',i:1},feed:{name:'喂奶次数',unit:'次',i:2},"
        "diaper:{name:'尿布次数',unit:'次',i:3},poop:{name:'便便次数',unit:'次',i:4},"
        "sleep:{name:'睡眠时长',unit:'分钟',i:5}};"
        "let chartDays=1,chartMetric='ml',chartData=[],lastStatus='';"
        "async function cmd(c){let r=await fetch('/api/command?token='+token,"
        "{method:'POST',body:c});return r.json()}"
        "async function send(c){try{let j=await cmd(c);let m=j.response||'';"
        "toast(m.startsWith('OK')||m.startsWith('STATE')?'✅ 已保存':'⚠️ '+m);"
        "if(m.startsWith('OK')||m.startsWith('STATE')){await refresh();await loadHist();await loadChart()}}"
        "catch(e){toast('连接失败',1)}}"
        "async function refresh(){try{let j=await cmd('STATUS'),raw=j.response||'';"
        "if(lastStatus&&lastStatus!==raw){await loadHist();await loadChart()}lastStatus=raw;"
        "const s={};j.response.replace(/\\b(\\w+)=([^\\s]+)/g,(_,k,v)=>s[k]=v);"
        "document.getElementById('dot').className='dot';"
        "const sy=document.getElementById('sync');sy.className='sync';"
        "sy.textContent='已同步 '+now();"
        "document.getElementById('s-day').textContent='第 '+s.DAY+' 天';"
        "const[fT,fM]=s.FEED.split('/');"
        "document.getElementById('s-feed').textContent=fT+' · '+fM+'ml';"
        "const[sT,sS]=s.SLEEP.split('/');"
        "document.getElementById('s-sleep').textContent=sT+' · '+(D[sS]||sS);"
        "const[dT,dC]=s.DIAPER.split('/');"
        "document.getElementById('s-diaper').textContent=dT+' · '+(D[dC]||dC);"
        "const[nT,nL]=s.NEXT.split('/');"
        "document.getElementById('s-next').textContent=nT;"
        "document.getElementById('interval').value=s.INTERVAL||180;"
        "const tag=document.getElementById('s-next-tag');"
        "if(s.REMIND&&s.REMIND!=='-'&&s.REMIND!==''){const[d,rt]=s.REMIND.split('/');"
        "tag.className='tag pink';tag.textContent='💉 '+d+' '+rt+' 打预防针'}"
        "else{tag.className='tag blue';tag.textContent=(L[nL]||nL)+' 提醒'}"
        "}catch(e){document.getElementById('dot').className='dot off';"
        "const sy=document.getElementById('sync');sy.className='sync off';sy.textContent='等待连接'}}"
        "async function loadSummary(){try{let j=await cmd('SUMMARY');"
        "document.getElementById('summary').textContent=j.response||'--'}"
        "catch(e){document.getElementById('summary').textContent='--'}}"
        "function renderChart(){const box=document.getElementById('chart');"
        "const meta=M[chartMetric],values=chartData.map(x=>+x[meta.i]||0);"
        "const max=Math.max(...values,0),total=values.reduce((a,b)=>a+b,0);"
        "document.getElementById('chart-title').textContent=meta.name;"
        "document.getElementById('chart-total').textContent="
        "(chartDays===1?'今日':'所选时段')+'合计 '+total+meta.unit;"
        "box.innerHTML='';if(!chartData.length){box.className='chart-empty';"
        "box.textContent='暂无可绘制的记录';return}box.className='chart';"
        "chartData.forEach((x,n)=>{const v=values[n],item=document.createElement('div');"
        "item.className='baritem';const value=document.createElement('span');"
        "value.className='barvalue';value.textContent=v||'';"
        "const bar=document.createElement('div');bar.className='bar'+(v?'':' zero');"
        "bar.style.height=(v?Math.max(8,Math.round(v/max*88)):3)+'px';"
        "bar.title=x[0]+' '+v+meta.unit;const label=document.createElement('span');"
        "label.className='barlabel';label.textContent=chartDays===30&&n%%5!==0&&n!==29?'':x[0].slice(3);"
        "item.append(value,bar,label);box.appendChild(item)})}"
        "async function loadChart(){try{let j=await cmd('CHART '+chartDays),raw=j.response||'';"
        "chartData=raw.startsWith('CHART|')?raw.slice(6).split(';').filter(Boolean).map(x=>x.split(',')):[];"
        "renderChart();if(chartDays===1)loadSummary();else{const ml=chartData.reduce((a,x)=>a+(+x[1]||0),0),"
        "feed=chartData.reduce((a,x)=>a+(+x[2]||0),0),diaper=chartData.reduce((a,x)=>a+(+x[3]||0),0),"
        "poop=chartData.reduce((a,x)=>a+(+x[4]||0),0),sleep=chartData.reduce((a,x)=>a+(+x[5]||0),0);"
        "document.getElementById('summary').textContent='近 '+chartDays+' 天：喂奶 '+feed+' 次 / '+ml+"
        "'ml，尿布 '+diaper+' 次，便便 '+poop+' 次，睡眠 '+sleep+' 分钟'}}"
        "catch(e){chartData=[];renderChart();document.getElementById('summary').textContent='统计加载失败'}}"
        "document.getElementById('ranges').onclick=e=>{const b=e.target.closest('[data-days]');if(!b)return;"
        "chartDays=+b.dataset.days;document.querySelectorAll('#ranges .chip').forEach(x=>x.classList.toggle('on',x===b));"
        "loadChart()};document.getElementById('metrics').onclick=e=>{const b=e.target.closest('[data-metric]');"
        "if(!b)return;chartMetric=b.dataset.metric;document.querySelectorAll('#metrics .chip').forEach("
        "x=>x.classList.toggle('on',x===b));renderChart()};"
        "async function loadHist(){try{let j=await cmd('HIST 10');"
        "document.getElementById('hist').textContent=j.response||'暂无'}"
        "catch(e){document.getElementById('hist').textContent='加载失败'}}"
        "async function scanWifi(){const list=document.getElementById('netlist');"
        "list.innerHTML='<span class=minilabel>扫描中...</span>';"
        "try{await fetch('/api/wifi/scan?token='+token);"
        "let j=null;"
        "for(let i=0;i<15;i++){"
        "await new Promise(r=>setTimeout(r,1200));"
        "try{let rr=await fetch('/api/wifi/scan/result?token='+token);j=await rr.json();"
        "if(j.done!==false&&(j.networks&&j.networks.length))break}catch(e){}"
        "}"
        "if(!j||!j.networks||!j.networks.length){list.innerHTML='<span class=minilabel>"
        "未发现网络</span>';return}"
        "list.innerHTML='';j.networks.forEach(n=>{"
        "if(!n.ssid)return;"
        "const bars=n.rssi>-60?'▂▄▆█':n.rssi>-75?'▂▄▆':'▂▄';"
        "const lock=n.auth>0?'🔒':'';"
        "const d=document.createElement('div');d.className='net';"
        "d.innerHTML='<span class=name></span><span class=lock></span><span class=sig></span>';"
        "d.querySelector('.name').textContent=n.ssid;"
        "d.querySelector('.lock').textContent=lock;"
        "d.querySelector('.sig').textContent=bars;"
        "d.onclick=()=>{document.getElementById('ssid').value=n.ssid;"
        "document.getElementById('wifiForm').style.display='block'};"
        "list.appendChild(d)})}catch(e){list.innerHTML='<span class=minilabel>"
        "扫描失败</span>'}}"
        "document.getElementById('wifiForm').onsubmit=async(e)=>{e.preventDefault();"
        "const fd=new FormData();fd.append('ssid',document.getElementById('ssid').value);"
        "fd.append('password',document.getElementById('wpass').value);"
        "try{let r=await fetch('/api/wifi?token='+token,{method:'POST',body:new "
        "URLSearchParams(fd)});let t=await r.text();toast(t||'已保存，正在连接...');"
        "}catch(e){toast('提交失败',1)}};"
        "async function downloadBackup(){try{const r=await fetch('/api/backup?token='+token);"
        "if(!r.ok)throw Error();const blob=await r.blob(),a=document.createElement('a');"
        "a.href=URL.createObjectURL(blob);a.download='quote0-baby-'+new Date().toISOString().slice(0,10)+'.json';"
        "a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000);toast('✅ 备份已导出')}"
        "catch(e){toast('备份导出失败',1)}}"
        "document.getElementById('restoreFile').onchange=async e=>{const f=e.target.files[0];"
        "if(!f)return;if(!confirm('恢复会覆盖设备上的当前状态和历史记录，确定继续吗？')){e.target.value='';return}"
        "try{const body=await f.text(),r=await fetch('/api/restore?token='+token,{method:'POST',"
        "headers:{'Content-Type':'application/json'},body}),j=await r.json();"
        "if(!r.ok||!j.ok)throw Error(j.message||'恢复失败');toast('✅ '+j.message);"
        "lastStatus='';await refresh();await loadHist();await loadChart()}catch(err){toast(err.message||'恢复失败',1)}"
        "e.target.value=''};"
        "async function loadFirmwareVersion(){try{const r=await fetch('/api/ping'),j=await r.json();"
        "document.getElementById('fwver').textContent=j.firmware_version||'未知版本'}catch(e){"
        "document.getElementById('fwver').textContent='读取失败'}}"
        "const communityFirmwareUrl='https://api.github.com/repos/thursdaycapital/"
        "hermes-studio-baby-dashboard/contents/docs/firmware/quote0_baby.bin?ref=main';"
        "async function checkCommunityUpdate(){const button=document.getElementById('onlineButton'),"
        "state=document.getElementById('otaState'),bar=document.getElementById('otaBar');"
        "if(!confirm('确定从 GitHub 安装最新社区固件吗？更新期间请勿断电。'))return;"
        "button.disabled=true;document.getElementById('otaButton').disabled=true;"
        "state.textContent='正在从 GitHub 下载最新版…';bar.style.width='3%%';"
        "try{const r=await fetch(communityFirmwareUrl,{cache:'no-store',"
        "headers:{Accept:'application/vnd.github.raw+json'}});"
        "if(!r.ok)throw Error('GitHub '+r.status);const blob=await r.blob();"
        "if(blob.size<1024||blob.size>1048576)throw Error('固件大小不正确');"
        "state.textContent='下载完成，正在传给设备…';startFirmwareUpload(blob,'quote0_baby.bin',true)}"
        "catch(e){button.disabled=false;document.getElementById('otaButton').disabled=false;"
        "state.textContent='GitHub 下载失败：'+e.message;toast('在线更新失败',1)}}"
        "function uploadFirmware(){const file=document.getElementById('otaFile').files[0];"
        "if(!file){toast('请先选择 .bin 固件',1);return}startFirmwareUpload(file,file.name)}"
        "function startFirmwareUpload(file,fileName,confirmed){const "
        "button=document.getElementById('otaButton'),bar=document.getElementById('otaBar'),"
        "state=document.getElementById('otaState');"
        "if(!fileName.toLowerCase().endsWith('.bin')){toast('只能上传 .bin 固件',1);return}"
        "if(file.size<1024||file.size>1048576){toast('固件大小不正确',1);return}"
        "if(!confirmed&&!confirm('确定更新到 '+fileName+' 吗？上传和重启期间请勿断电。'))return;"
        "button.disabled=true;document.getElementById('onlineButton').disabled=true;"
        "bar.style.width='0%%';state.textContent='正在上传 0%%';"
        "const x=new XMLHttpRequest();x.open('POST','/api/ota?token='+token);"
        "x.setRequestHeader('Content-Type','application/octet-stream');"
        "x.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);"
        "bar.style.width=p+'%%';state.textContent='正在上传 '+p+'%%'}};"
        "x.onload=()=>{if(x.status>=200&&x.status<300){bar.style.width='100%%';"
        "state.textContent='更新成功，设备正在重启…';toast('✅ 更新成功，正在重启');"
        "setTimeout(()=>location.reload(),12000)}else{button.disabled=false;"
        "document.getElementById('onlineButton').disabled=false;"
        "state.textContent='更新失败，请确认固件文件';toast('固件更新失败',1)}};"
        "x.onerror=()=>{button.disabled=false;document.getElementById('onlineButton').disabled=false;"
        "state.textContent='连接中断，请等待设备状态恢复';"
        "toast('上传连接中断',1)};x.send(file)}"
        "if('serviceWorker'in navigator)navigator.serviceWorker.register('/sw.js').catch(()=>{});"
        "document.addEventListener('visibilitychange',()=>{if(!document.hidden)refresh()});"
        "async function syncAll(){loadFirmwareVersion();await refresh();await loadChart();await loadHist()}syncAll();"
        "setInterval(()=>{if(!document.hidden)refresh()},4000);"
        "setInterval(loadChart,60000);"
        "</script></body></html>",
        access_token);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Connection", "close");
    return httpd_resp_sendstr(request, page);
}

static httpd_handle_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 14;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) return NULL;

    const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_handler};
    const httpd_uri_t ping = {
        .uri = "/api/ping", .method = HTTP_GET, .handler = ping_handler};
    const httpd_uri_t command = {
        .uri = "/api/command", .method = HTTP_POST,
        .handler = command_http_handler};
    const httpd_uri_t wifi = {
        .uri = "/api/wifi", .method = HTTP_POST,
        .handler = wifi_config_handler};
    const httpd_uri_t wifiscan = {
        .uri = "/api/wifi/scan", .method = HTTP_GET,
        .handler = wifi_scan_handler};
    const httpd_uri_t wifiscanresult = {
        .uri = "/api/wifi/scan/result", .method = HTTP_GET,
        .handler = wifi_scan_result_handler};
    const httpd_uri_t ota = {
        .uri = "/api/ota", .method = HTTP_POST, .handler = ota_http_handler};
    const httpd_uri_t backup = {
        .uri = "/api/backup", .method = HTTP_GET,
        .handler = backup_export_http_handler};
    const httpd_uri_t restore = {
        .uri = "/api/restore", .method = HTTP_POST,
        .handler = backup_import_http_handler};
    const httpd_uri_t manifest = {
        .uri = "/manifest.webmanifest", .method = HTTP_GET,
        .handler = manifest_handler};
    const httpd_uri_t icon = {
        .uri = "/icon.svg", .method = HTTP_GET, .handler = icon_handler};
    const httpd_uri_t offline = {
        .uri = "/offline", .method = HTTP_GET, .handler = offline_handler};
    const httpd_uri_t service_worker = {
        .uri = "/sw.js", .method = HTTP_GET,
        .handler = service_worker_handler};
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &ping);
    httpd_register_uri_handler(server, &command);
    httpd_register_uri_handler(server, &wifi);
    httpd_register_uri_handler(server, &wifiscan);
    httpd_register_uri_handler(server, &wifiscanresult);
    httpd_register_uri_handler(server, &ota);
    httpd_register_uri_handler(server, &backup);
    httpd_register_uri_handler(server, &restore);
    httpd_register_uri_handler(server, &manifest);
    httpd_register_uri_handler(server, &icon);
    httpd_register_uri_handler(server, &offline);
    httpd_register_uri_handler(server, &service_worker);
    return server;
}

static void discovery_task(void *unused)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) vTaskDelete(NULL);
    int enabled = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(sock);
        vTaskDelete(NULL);
    }

    char buffer[64];
    while (true) {
        struct sockaddr_in source;
        socklen_t source_len = sizeof(source);
        int len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                           (struct sockaddr *)&source, &source_len);
        if (len <= 0) continue;
        buffer[len] = '\0';
        if (strcmp(buffer, "QUOTE0_DISCOVER") == 0) {
            const char reply[] = "QUOTE0_BABY";
            sendto(sock, reply, sizeof(reply) - 1, 0,
                   (struct sockaddr *)&source, source_len);
        }
    }
}

static void fill_ap_config(wifi_config_t *config)
{
    memset(config, 0, sizeof(*config));
    snprintf((char *)config->ap.ssid, sizeof(config->ap.ssid), "%s", ap_name);
    snprintf((char *)config->ap.password, sizeof(config->ap.password),
             "baby1234");
    config->ap.ssid_len = strlen(ap_name);
    config->ap.channel = 6;
    config->ap.max_connection = 4;
    config->ap.authmode = WIFI_AUTH_WPA2_PSK;
}

static void recovery_ap_task(void *unused)
{
    esp_netif_create_default_wifi_ap();
    wifi_config_t config;
    fill_ap_config(&config);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_LOGW(TAG, "Station unavailable; recovery AP: %s", ap_name);
    vTaskDelete(NULL);
}

static void wifi_event(void *arg, esp_event_base_t base,
                       int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count++ < 10) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_events, WIFI_FAIL_BIT);
            if (!recovery_ap_started) {
                recovery_ap_started = true;
                xTaskCreate(recovery_ap_task, "recovery_ap", 3072,
                            NULL, 5, NULL);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        retry_count = 0;
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Local address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool load_wifi(char *ssid, size_t ssid_size,
                      char *password, size_t password_size)
{
    nvs_handle_t handle;
    if (nvs_open("baby_wifi", NVS_READONLY, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_get_str(handle, "ssid", ssid, &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "password", password, &password_size);
    }
    nvs_close(handle);
    return err == ESP_OK && ssid[0] != '\0';
}

static void start_access_point(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_config_t config;
    fill_ap_config(&config);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Provisioning AP: %s, http://192.168.4.1", ap_name);
}

static void start_station(const char *ssid, const char *password)
{
    esp_netif_create_default_wifi_sta();
    wifi_config_t config = {0};
    size_t ssid_len = strnlen(ssid, sizeof(config.sta.ssid));
    memcpy(config.sta.ssid, ssid, ssid_len);
    strlcpy((char *)config.sta.password, password,
            sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

esp_err_t baby_network_start(baby_command_handler_t handler,
                             baby_backup_export_handler_t export_handler,
                             baby_backup_import_handler_t import_handler)
{
    if (!handler || !export_handler || !import_handler) {
        return ESP_ERR_INVALID_ARG;
    }
    command_handler = handler;
    backup_export_handler = export_handler;
    backup_import_handler = import_handler;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(access_token, sizeof(access_token), "Q0%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);
    snprintf(ap_name, sizeof(ap_name), "Quote0-Baby-%02X%02X",
             mac[4], mac[5]);

    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, NULL));

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));

    char ssid[33] = {0};
    char password[65] = {0};
    if (load_wifi(ssid, sizeof(ssid), password, sizeof(password))) {
        start_station(ssid, password);
    } else {
        start_access_point();
    }

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("quote0-baby"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Hermes Studio Baby Dashboard"));
    ESP_ERROR_CHECK(mdns_service_add("宝宝看板", "_http", "_tcp", 80,
                                     NULL, 0));

    start_http_server();
    xTaskCreate(discovery_task, "quote0_discovery", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "API token: %s", access_token);
    return ESP_OK;
}
