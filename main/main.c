/* SPDX-License-Identifier: Apache-2.0 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "baby_network.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "han_font_16.h"
#include "hermes_mascot_42.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_netif_sntp.h"
#include "uc8251d_minimal.h"

/* 这些警告是 GCC 对固定小缓冲区的保守误报（运行时值域安全） */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Warray-bounds"

#define LANDSCAPE_WIDTH  296
#define LANDSCAPE_HEIGHT 152
#define STATE_MAGIC      0x42414259U
#define STATE_VERSION    4U
#define SECONDS_PER_DAY  86400
#define SHANGHAI_OFFSET  (8 * 60 * 60)

static const char *TAG = "quote0_baby";
static uint8_t frame[UC8251D_BUFFER_SIZE];
static SemaphoreHandle_t command_mutex;

static const uc8251d_config_t display_config = {
    .spi_host = SPI2_HOST,
    .pin_vin = 20,
    .pin_busy = 3,
    .pin_reset = 4,
    .pin_dc = 5,
    .pin_cs = 6,
    .pin_sck = 10,
    .pin_mosi = 7,
    .spi_clock_hz = 15 * 1000 * 1000,
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t day;
    char feed_time[6];
    uint16_t feed_ml;
    char diaper_time[6];
    char diaper_code[4];
    char sleep_time[6];
    char sleep_state[4];
    char next_time[6];
    char next_label[8];
    int32_t day_anchor_date;
    char reminder_date[6];
    char reminder_time[6];
} baby_state_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t day;
    char feed_time[6];
    uint16_t feed_ml;
    char diaper_time[6];
    char diaper_code[4];
    char sleep_time[6];
    char sleep_state[4];
    char next_time[6];
    char next_label[8];
} baby_state_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t day;
    char feed_time[6];
    uint16_t feed_ml;
    char diaper_time[6];
    char diaper_code[4];
    char sleep_time[6];
    char sleep_state[4];
    char next_time[6];
    char next_label[8];
    int32_t day_anchor_date;
} baby_state_v2_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t day;
    char feed_time[6];
    uint16_t feed_ml;
    char diaper_time[6];
    char diaper_code[4];
    char sleep_time[6];
    char sleep_state[4];
    char next_time[6];
    char next_label[8];
    int32_t day_anchor_date;
    char reminder_date[6];
} baby_state_v3_t;

static baby_state_t state = {
    .magic = STATE_MAGIC,
    .version = STATE_VERSION,
    .day = 9,
    .feed_time = "--:--",
    .feed_ml = 0,
    .diaper_time = "--:--",
    .diaper_code = "-",
    .sleep_time = "--:--",
    .sleep_state = "OFF",
    .next_time = "--:--",
    .next_label = "FEED",
    .day_anchor_date = 0,
    .reminder_date = "",
    .reminder_time = "",
};

typedef struct {
    char character;
    uint8_t rows[7];
} glyph_t;

static const glyph_t font[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'-', {0, 0, 0, 31, 0, 0, 0}},
    {':', {0, 4, 4, 0, 4, 4, 0}},
    {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'5', {31, 16, 16, 30, 1, 1, 30}},
    {'6', {14, 16, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 2, 12}},
    {'A', {14, 17, 17, 31, 17, 17, 17}},
    {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}},
    {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}},
    {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'W', {17, 17, 17, 21, 21, 21, 10}},
    {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},
};

static void set_native_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= UC8251D_WIDTH || y < 0 || y >= UC8251D_HEIGHT) {
        return;
    }
    int pixel_index = y * UC8251D_WIDTH + x;
    uint8_t mask = (uint8_t)(1U << (7 - (pixel_index % 8)));
    if (black) {
        frame[pixel_index / 8] &= (uint8_t)~mask;
    } else {
        frame[pixel_index / 8] |= mask;
    }
}

static void set_pixel(int x, int y, bool black)
{
    set_native_pixel(y, UC8251D_HEIGHT - 1 - x, black);
}

static void fill_rect(int x, int y, int width, int height, bool black)
{
    for (int py = y; py < y + height; ++py) {
        for (int px = x; px < x + width; ++px) {
            set_pixel(px, py, black);
        }
    }
}

static const uint8_t *find_glyph(char character)
{
    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); ++i) {
        if (font[i].character == character) {
            return font[i].rows;
        }
    }
    return font[0].rows;
}

static void draw_text(const char *text, int x, int y, int scale)
{
    while (*text != '\0') {
        const uint8_t *rows = find_glyph((char)toupper((unsigned char)*text++));
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((rows[row] & (1U << (4 - col))) != 0) {
                    fill_rect(x + col * scale, y + row * scale, scale, scale, true);
                }
            }
        }
        x += 6 * scale;
    }
}

static const uint16_t *find_han_glyph(uint32_t codepoint)
{
    for (size_t i = 0; i < han_font_16_count; ++i) {
        if (han_font_16[i].codepoint == codepoint) {
            return han_font_16[i].rows;
        }
    }
    return NULL;
}

static void draw_han(uint32_t codepoint, int x, int y)
{
    const uint16_t *rows = find_han_glyph(codepoint);
    if (!rows) return;
    for (int row = 0; row < 16; ++row) {
        for (int col = 0; col < 16; ++col) {
            if ((rows[row] & (1U << (15 - col))) != 0) {
                set_pixel(x + col, y + row, true);
            }
        }
    }
}

static void draw_mixed_text(const char *text, int x, int y)
{
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor != '\0') {
        if (*cursor < 0x80) {
            char ascii[2] = {(char)*cursor++, '\0'};
            draw_text(ascii, x, y + 1, 2);
            x += 12;
            continue;
        }
        if ((cursor[0] & 0xF0) == 0xE0 && cursor[1] && cursor[2]) {
            uint32_t codepoint = ((uint32_t)(cursor[0] & 0x0F) << 12) |
                                 ((uint32_t)(cursor[1] & 0x3F) << 6) |
                                 (uint32_t)(cursor[2] & 0x3F);
            draw_han(codepoint, x, y);
            cursor += 3;
            x += 18;
            continue;
        }
        ++cursor;
    }
}

static void draw_hermes_mascot(int x, int y)
{
    for (int row = 0; row < HERMES_MASCOT_HEIGHT; ++row) {
        for (int col = 0; col < HERMES_MASCOT_WIDTH; ++col) {
            if ((hermes_mascot_rows[row] &
                 (1ULL << (HERMES_MASCOT_WIDTH - 1 - col))) != 0) {
                set_pixel(x + col, y + row, true);
            }
        }
    }
}

static void draw_dashboard(void)
{
    char line[32];
    const char *sleep_label =
        strcmp(state.sleep_state, "ON") == 0 ? "开始" : "结束";
    const char *diaper_label = "-";
    if (strcmp(state.diaper_code, "W") == 0) diaper_label = "尿";
    else if (strcmp(state.diaper_code, "D") == 0) diaper_label = "便";
    else if (strcmp(state.diaper_code, "WD") == 0) diaper_label = "都有";

    const char *next_label = state.next_label;
    if (strcmp(state.next_label, "FEED") == 0) next_label = "喂奶";
    else if (strcmp(state.next_label, "SLEEP") == 0) next_label = "睡眠";
    else if (strcmp(state.next_label, "DIAPER") == 0) next_label = "尿布";

    memset(frame, 0xFF, sizeof(frame));
    fill_rect(0, 0, LANDSCAPE_WIDTH, 2, true);
    fill_rect(0, LANDSCAPE_HEIGHT - 2, LANDSCAPE_WIDTH, 2, true);
    fill_rect(0, 0, 2, LANDSCAPE_HEIGHT, true);
    fill_rect(LANDSCAPE_WIDTH - 2, 0, 2, LANDSCAPE_HEIGHT, true);

    draw_mixed_text("HERMES STUDIO", 10, 5);
    snprintf(line, sizeof(line), "喂养宝宝 第%02u天", (unsigned)state.day);
    draw_mixed_text(line, 10, 26);
    draw_hermes_mascot(244, 5);
    fill_rect(8, 47, LANDSCAPE_WIDTH - 16, 2, true);

    snprintf(line, sizeof(line), "喂奶 %s %uML", state.feed_time,
             (unsigned)state.feed_ml);
    draw_mixed_text(line, 10, 53);
    snprintf(line, sizeof(line), "睡眠 %s %s", state.sleep_time, sleep_label);
    draw_mixed_text(line, 10, 73);
    snprintf(line, sizeof(line), "尿布 %s %s", state.diaper_time, diaper_label);
    draw_mixed_text(line, 10, 93);
    snprintf(line, sizeof(line), "下次 %s %s", state.next_time, next_label);
    draw_mixed_text(line, 10, 113);
    if (state.reminder_date[0] != '\0') {
        snprintf(line, sizeof(line), "提醒 %s %s 打预防针",
                 state.reminder_date, state.reminder_time);
        draw_mixed_text(line, 10, 133);
    }
}

static esp_err_t save_state(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("baby", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "state", &state, sizeof(state));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void load_state(void)
{
    nvs_handle_t handle;
    if (nvs_open("baby", NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = 0;
    if (nvs_get_blob(handle, "state", NULL, &size) == ESP_OK) {
        if (size == sizeof(baby_state_t)) {
            baby_state_t loaded;
            if (nvs_get_blob(handle, "state", &loaded, &size) == ESP_OK &&
                loaded.magic == STATE_MAGIC &&
                loaded.version == STATE_VERSION) {
                state = loaded;
            }
        } else if (size == sizeof(baby_state_v3_t)) {
            baby_state_v3_t loaded;
            if (nvs_get_blob(handle, "state", &loaded, &size) == ESP_OK &&
                loaded.magic == STATE_MAGIC && loaded.version == 3U) {
                memcpy(&state, &loaded, sizeof(loaded));
                state.version = STATE_VERSION;
                if (strcmp(state.next_label, "SHOT") == 0) {
                    snprintf(state.reminder_time,
                             sizeof(state.reminder_time), "%s",
                             state.next_time);
                } else {
                    state.reminder_time[0] = '\0';
                }
            }
        } else if (size == sizeof(baby_state_v2_t)) {
            baby_state_v2_t loaded;
            if (nvs_get_blob(handle, "state", &loaded, &size) == ESP_OK &&
                loaded.magic == STATE_MAGIC && loaded.version == 2U) {
                memcpy(&state, &loaded, sizeof(loaded));
                state.version = STATE_VERSION;
                state.reminder_date[0] = '\0';
                state.reminder_time[0] = '\0';
            }
        } else if (size == sizeof(baby_state_v1_t)) {
            baby_state_v1_t loaded;
            if (nvs_get_blob(handle, "state", &loaded, &size) == ESP_OK &&
                loaded.magic == STATE_MAGIC && loaded.version == 1U) {
                memcpy(&state, &loaded, sizeof(loaded));
                state.version = STATE_VERSION;
                state.day_anchor_date = 0;
                state.reminder_date[0] = '\0';
                state.reminder_time[0] = '\0';
            }
        }
    }
    nvs_close(handle);
}

static int32_t current_shanghai_date(void)
{
    time_t now;
    time(&now);
    if (now < 1704067200) return 0;
    return (int32_t)((now + SHANGHAI_OFFSET) / SECONDS_PER_DAY);
}

static bool update_day_from_clock(void)
{
    int32_t today = current_shanghai_date();
    if (today == 0) return false;
    if (state.day_anchor_date == 0) {
        state.day_anchor_date = today;
        return true;
    }
    int32_t calculated = (int32_t)state.day + today - state.day_anchor_date;
    if (calculated < 0) calculated = 0;
    if (calculated > 999) calculated = 999;
    if ((uint16_t)calculated == state.day) return false;
    state.day = (uint16_t)calculated;
    state.day_anchor_date = today;
    return true;
}

static bool valid_time(const char *value)
{
    return value && strlen(value) == 5 &&
           isdigit((unsigned char)value[0]) &&
           isdigit((unsigned char)value[1]) && value[2] == ':' &&
           isdigit((unsigned char)value[3]) &&
           isdigit((unsigned char)value[4]);
}

/* ---------- 历史记录（NVS 环形存储） ---------- */

#define HISTORY_MAX 32

typedef struct {
    char date[6];   /* "MM-DD" */
    char time[6];   /* "HH:MM" */
    char kind[5];   /* "FEED" / "DIAP" / "SLEE" */
    char value[9];  /* "80" / "W" / "ON" / "OFF" */
} baby_history_entry_t; /* 25 bytes */

typedef struct {
    uint32_t magic;
    uint16_t count;
    uint16_t head;
    baby_history_entry_t entries[HISTORY_MAX];
} baby_history_t;

static baby_history_t history = {0};

static void history_init(void)
{
    nvs_handle_t handle;
    if (nvs_open("baby", NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = sizeof(history);
    if (nvs_get_blob(handle, "history", &history, &size) == ESP_OK &&
        history.magic != 0x48495354U) {
        memset(&history, 0, sizeof(history));
    }
    nvs_close(handle);
}

static void history_save(void)
{
    nvs_handle_t handle;
    if (nvs_open("baby", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_blob(handle, "history", &history, sizeof(history));
    nvs_commit(handle);
    nvs_close(handle);
}

static void history_add(const char *kind, const char *when,
                        const char *value)
{
    time_t raw;
    time(&raw);
    time_t shanghai = raw + SHANGHAI_OFFSET;
    struct tm result;
    localtime_r(&shanghai, &result);
    if (history.magic != 0x48495354U) {
        memset(&history, 0, sizeof(history));
        history.magic = 0x48495354U;
    }
    if (history.count < HISTORY_MAX) history.count++;
    baby_history_entry_t *entry = &history.entries[history.head];
    history.head = (history.head + 1) % HISTORY_MAX;
    snprintf(entry->date, sizeof(entry->date), "%02d-%02d",
             result.tm_mon + 1, result.tm_mday);
    snprintf(entry->time, sizeof(entry->time), "%s", when);
    snprintf(entry->kind, sizeof(entry->kind), "%s", kind);
    snprintf(entry->value, sizeof(entry->value), "%s", value);
    history_save();
}

static void history_list(char *out, size_t size, int limit)
{
    if (size == 0) return;
    out[0] = '\0';
    size_t written = 0;
    if (history.magic != 0x48495354U || history.count == 0) {
        snprintf(out, size, "暂无历史记录");
        return;
    }
    int shown = 0;
    int idx = (history.head + HISTORY_MAX - 1) % HISTORY_MAX;
    while (shown < history.count && shown < limit && written < size - 24) {
        baby_history_entry_t *e = &history.entries[idx];
        const char *label = "记录";
        if (strcmp(e->kind, "FEED") == 0) label = "喂奶";
        else if (strcmp(e->kind, "DIAP") == 0) label = "尿布";
        else if (strcmp(e->kind, "SLEE") == 0) label = "睡眠";
        int n = snprintf(out + written, size - written, "%s %s %s %s\n",
                         e->date, e->time, label, e->value);
        if (n > 0) written += n;
        idx = (idx + HISTORY_MAX - 1) % HISTORY_MAX;
        shown++;
    }
    if (written > 0 && out[written - 1] == '\n') out[written - 1] = '\0';
}

static void history_summary(char *out, size_t size)
{
    time_t raw;
    time(&raw);
    time_t shanghai = raw + SHANGHAI_OFFSET;
    struct tm result;
    localtime_r(&shanghai, &result);
    char today[6];
    snprintf(today, sizeof(today), "%02d-%02d",
             result.tm_mon + 1, result.tm_mday);
    int feed_count = 0, feed_ml = 0, diaper_count = 0, poop = 0;
    int sleep_minutes = 0;
    char sleep_start[16] = {0};
    if (history.magic == 0x48495354U) {
        int first = (history.head + HISTORY_MAX - history.count) % HISTORY_MAX;
        for (int i = 0; i < history.count; i++) {
            baby_history_entry_t *e =
                &history.entries[(first + i) % HISTORY_MAX];
            if (strcmp(e->date, today) != 0) continue;
            if (strcmp(e->kind, "FEED") == 0) {
                feed_count++;
                feed_ml += atoi(e->value);
            } else if (strcmp(e->kind, "DIAP") == 0) {
                diaper_count++;
                if (strcmp(e->value, "D") == 0 ||
                    strcmp(e->value, "WD") == 0) poop++;
            } else if (strcmp(e->kind, "SLEE") == 0) {
                if (strcmp(e->value, "ON") == 0) {
                    snprintf(sleep_start, sizeof(sleep_start), "%s", e->time);
                } else if (sleep_start[0]) {
                    int sh, sm, eh, em;
                    sscanf(sleep_start, "%d:%d", &sh, &sm);
                    sscanf(e->time, "%d:%d", &eh, &em);
                    sleep_minutes += (eh * 60 + em) - (sh * 60 + sm);
                    if (sleep_minutes < 0) sleep_minutes += 24 * 60;
                    sleep_start[0] = '\0';
                }
            }
        }
    }
    snprintf(out, size,
             "今日: 喂奶%d次/%dML 尿布%d次 便便%d次 睡眠%d分钟",
             feed_count, feed_ml, diaper_count, poop, sleep_minutes);
}

typedef struct {
    int feed_ml;
    int feed_count;
    int diaper_count;
    int poop_count;
    int sleep_minutes;
} baby_day_stats_t;

static int days_in_year(int year)
{
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
               ? 366
               : 365;
}

static int date_ordinal(int year, int month, int day)
{
    static const int before_month[] = {
        0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    if (month < 1 || month > 12 || day < 1 || day > 31) return -1;
    int ordinal = before_month[month] + day;
    if (month > 2 && days_in_year(year) == 366) ordinal++;
    return ordinal;
}

static int history_entry_age_days(const char *date, const struct tm *today)
{
    int month = 0, day = 0;
    if (sscanf(date, "%d-%d", &month, &day) != 2) return -1;
    int year = today->tm_year + 1900;
    int today_ordinal = today->tm_yday + 1;
    int entry_ordinal = date_ordinal(year, month, day);
    if (entry_ordinal < 1) return -1;
    if (entry_ordinal <= today_ordinal) return today_ordinal - entry_ordinal;
    int previous_ordinal = date_ordinal(year - 1, month, day);
    if (previous_ordinal < 1) return -1;
    return today_ordinal + days_in_year(year - 1) - previous_ordinal;
}

static void history_chart(char *out, size_t size, int days)
{
    if (days != 1 && days != 2 && days != 4 && days != 6 && days != 30) {
        days = 1;
    }
    baby_day_stats_t stats[30] = {0};
    time_t raw;
    time(&raw);
    time_t shanghai = raw + SHANGHAI_OFFSET;
    struct tm today;
    localtime_r(&shanghai, &today);

    int sleep_start_age = -1;
    int sleep_start_minute = -1;
    if (history.magic == 0x48495354U) {
        int first = (history.head + HISTORY_MAX - history.count) % HISTORY_MAX;
        for (int i = 0; i < history.count; i++) {
            baby_history_entry_t *entry =
                &history.entries[(first + i) % HISTORY_MAX];
            int age = history_entry_age_days(entry->date, &today);
            if (strcmp(entry->kind, "SLEE") == 0) {
                int hour = 0, minute = 0;
                if (sscanf(entry->time, "%d:%d", &hour, &minute) != 2) continue;
                if (strcmp(entry->value, "ON") == 0) {
                    sleep_start_age = age;
                    sleep_start_minute = hour * 60 + minute;
                } else if (strcmp(entry->value, "OFF") == 0 &&
                           sleep_start_age >= 0 && sleep_start_minute >= 0) {
                    int duration = hour * 60 + minute - sleep_start_minute;
                    if (duration < 0 || age < sleep_start_age) duration += 1440;
                    if (sleep_start_age < days && duration >= 0 &&
                        duration <= 1440) {
                        stats[sleep_start_age].sleep_minutes += duration;
                    }
                    sleep_start_age = -1;
                    sleep_start_minute = -1;
                }
                continue;
            }
            if (age < 0 || age >= days) continue;
            if (strcmp(entry->kind, "FEED") == 0) {
                stats[age].feed_count++;
                stats[age].feed_ml += atoi(entry->value);
            } else if (strcmp(entry->kind, "DIAP") == 0) {
                stats[age].diaper_count++;
                if (strcmp(entry->value, "D") == 0 ||
                    strcmp(entry->value, "WD") == 0) {
                    stats[age].poop_count++;
                }
            }
        }
    }

    int written = snprintf(out, size, "CHART|");
    for (int age = days - 1; age >= 0 && written < (int)size - 40; age--) {
        time_t point = shanghai - (time_t)age * SECONDS_PER_DAY;
        struct tm date;
        localtime_r(&point, &date);
        baby_day_stats_t *s = &stats[age];
        written += snprintf(
            out + written, size - written, "%s%02d-%02d,%d,%d,%d,%d,%d",
            age == days - 1 ? "" : ";", date.tm_mon + 1, date.tm_mday,
            s->feed_ml, s->feed_count, s->diaper_count, s->poop_count,
            s->sleep_minutes);
    }
}

static void history_undo(char *out, size_t size)
{
    if (history.magic != 0x48495354U || history.count == 0) {
        snprintf(out, size, "没有可撤销的记录");
        return;
    }
    int idx = (history.head + HISTORY_MAX - 1) % HISTORY_MAX;
    baby_history_entry_t *e = &history.entries[idx];
    snprintf(out, size, "已撤销 %s %s %s", e->date, e->time, e->value);
    history.head = idx;
    history.count--;
    history_save();
}

static void history_set_interval(int minutes)
{
    nvs_handle_t handle;
    if (nvs_open("baby", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u16(handle, "interval", (uint16_t)minutes);
    nvs_commit(handle);
    nvs_close(handle);
}

static int history_get_interval(void)
{
    nvs_handle_t handle;
    uint16_t value = 180;
    if (nvs_open("baby", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u16(handle, "interval", &value);
        nvs_close(handle);
    }
    return value;
}

/* 喂奶后自动计算下次提醒时间：HH:MM + interval 分钟 */
static void next_feed_from_interval(void)
{
    int hour, minute;
    if (sscanf(state.feed_time, "%d:%d", &hour, &minute) != 2) return;
    int total = (hour * 60 + minute) + history_get_interval();
    total %= 24 * 60;
    snprintf(state.next_time, sizeof(state.next_time), "%02d:%02d",
             total / 60, total % 60);
    snprintf(state.next_label, sizeof(state.next_label), "FEED");
}

static bool valid_date(const char *value)
{
    return value && strlen(value) == 5 &&
           isdigit((unsigned char)value[0]) &&
           isdigit((unsigned char)value[1]) && value[2] == '-' &&
           isdigit((unsigned char)value[3]) &&
           isdigit((unsigned char)value[4]);
}

static void copy_token(char *destination, size_t size, const char *source)
{
    if (!source || size == 0) return;
    snprintf(destination, size, "%s", source);
    for (size_t i = 0; destination[i] != '\0'; ++i) {
        destination[i] = (char)toupper((unsigned char)destination[i]);
    }
}

static esp_err_t refresh_display(void)
{
    draw_dashboard();
    gpio_set_direction(GPIO_NUM_20, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_20, 1);
    esp_err_t err = uc8251d_display(frame);
    if (err == ESP_OK) err = uc8251d_sleep();
    return err;
}

static bool backup_string(const cJSON *object, const char *key,
                          char *destination, size_t size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || !item->valuestring ||
        strlen(item->valuestring) >= size) {
        return false;
    }
    snprintf(destination, size, "%s", item->valuestring);
    return true;
}

static bool backup_integer(const cJSON *object, const char *key,
                           int minimum, int maximum, int *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || item->valuedouble != item->valueint ||
        item->valueint < minimum || item->valueint > maximum) {
        return false;
    }
    *value = item->valueint;
    return true;
}

static bool backup_time(const char *value, bool allow_placeholder)
{
    if (allow_placeholder && strcmp(value, "--:--") == 0) return true;
    int hour = -1, minute = -1;
    char extra = '\0';
    return strlen(value) == 5 &&
           sscanf(value, "%2d:%2d%c", &hour, &minute, &extra) == 2 &&
           hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

static bool backup_date(const char *value, bool allow_empty)
{
    if (allow_empty && value[0] == '\0') return true;
    int month = -1, day = -1;
    char extra = '\0';
    return strlen(value) == 5 &&
           sscanf(value, "%2d-%2d%c", &month, &day, &extra) == 2 &&
           month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

static esp_err_t export_backup(char *response, size_t response_size)
{
    if (!response || response_size == 0) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(command_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *saved_state = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateArray();
    if (!root || !saved_state || !entries) goto no_memory;
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddStringToObject(root, "device", "QUOTE0_BABY");
    cJSON_AddItemToObject(root, "state", saved_state);
    cJSON_AddNumberToObject(saved_state, "day", state.day);
    cJSON_AddStringToObject(saved_state, "feedTime", state.feed_time);
    cJSON_AddNumberToObject(saved_state, "feedMl", state.feed_ml);
    cJSON_AddStringToObject(saved_state, "diaperTime", state.diaper_time);
    cJSON_AddStringToObject(saved_state, "diaperCode", state.diaper_code);
    cJSON_AddStringToObject(saved_state, "sleepTime", state.sleep_time);
    cJSON_AddStringToObject(saved_state, "sleepState", state.sleep_state);
    cJSON_AddStringToObject(saved_state, "nextTime", state.next_time);
    cJSON_AddStringToObject(saved_state, "nextLabel", state.next_label);
    cJSON_AddNumberToObject(saved_state, "dayAnchor", state.day_anchor_date);
    cJSON_AddStringToObject(saved_state, "reminderDate", state.reminder_date);
    cJSON_AddStringToObject(saved_state, "reminderTime", state.reminder_time);
    cJSON_AddNumberToObject(root, "feedInterval", history_get_interval());
    cJSON_AddItemToObject(root, "history", entries);

    if (history.magic == 0x48495354U) {
        int first = (history.head + HISTORY_MAX - history.count) % HISTORY_MAX;
        for (int i = 0; i < history.count; i++) {
            baby_history_entry_t *entry =
                &history.entries[(first + i) % HISTORY_MAX];
            cJSON *item = cJSON_CreateObject();
            if (!item) goto no_memory;
            cJSON_AddStringToObject(item, "date", entry->date);
            cJSON_AddStringToObject(item, "time", entry->time);
            cJSON_AddStringToObject(item, "kind", entry->kind);
            cJSON_AddStringToObject(item, "value", entry->value);
            cJSON_AddItemToArray(entries, item);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    if (!json) goto no_memory;
    if (strlen(json) + 1 > response_size) {
        cJSON_free(json);
        cJSON_Delete(root);
        xSemaphoreGive(command_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(response, response_size, "%s", json);
    cJSON_free(json);
    cJSON_Delete(root);
    xSemaphoreGive(command_mutex);
    return ESP_OK;

no_memory:
    cJSON_Delete(root);
    xSemaphoreGive(command_mutex);
    return ESP_ERR_NO_MEM;
}

static esp_err_t import_backup(const char *backup, size_t backup_size,
                               char *response, size_t response_size)
{
    if (!backup || backup_size == 0 || !response || response_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_ParseWithLength(backup, backup_size);
    if (!root) {
        snprintf(response, response_size, "备份文件不是有效 JSON");
        return ESP_ERR_INVALID_ARG;
    }

    baby_state_t imported = state;
    baby_history_t imported_history = {
        .magic = 0x48495354U, .count = 0, .head = 0};
    int schema = 0, day = 0, feed_ml = 0, anchor = 0, interval = 0;
    const cJSON *saved_state =
        cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "history");
    bool valid = backup_integer(root, "schema", 1, 1, &schema) &&
                 cJSON_IsObject(saved_state) && cJSON_IsArray(entries) &&
                 cJSON_GetArraySize(entries) <= HISTORY_MAX &&
                 backup_integer(root, "feedInterval", 30, 720, &interval) &&
                 backup_integer(saved_state, "day", 0, 999, &day) &&
                 backup_integer(saved_state, "feedMl", 0, 999, &feed_ml) &&
                 backup_integer(saved_state, "dayAnchor", 0, INT32_MAX,
                                &anchor) &&
                 backup_string(saved_state, "feedTime", imported.feed_time,
                               sizeof(imported.feed_time)) &&
                 backup_string(saved_state, "diaperTime",
                               imported.diaper_time,
                               sizeof(imported.diaper_time)) &&
                 backup_string(saved_state, "diaperCode",
                               imported.diaper_code,
                               sizeof(imported.diaper_code)) &&
                 backup_string(saved_state, "sleepTime", imported.sleep_time,
                               sizeof(imported.sleep_time)) &&
                 backup_string(saved_state, "sleepState",
                               imported.sleep_state,
                               sizeof(imported.sleep_state)) &&
                 backup_string(saved_state, "nextTime", imported.next_time,
                               sizeof(imported.next_time)) &&
                 backup_string(saved_state, "nextLabel", imported.next_label,
                               sizeof(imported.next_label)) &&
                 backup_string(saved_state, "reminderDate",
                               imported.reminder_date,
                               sizeof(imported.reminder_date)) &&
                 backup_string(saved_state, "reminderTime",
                               imported.reminder_time,
                               sizeof(imported.reminder_time));
    imported.day = (uint16_t)day;
    imported.feed_ml = (uint16_t)feed_ml;
    imported.day_anchor_date = anchor;
    valid = valid && backup_time(imported.feed_time, true) &&
            backup_time(imported.diaper_time, true) &&
            backup_time(imported.sleep_time, true) &&
            backup_time(imported.next_time, true) &&
            (strcmp(imported.diaper_code, "-") == 0 ||
             strcmp(imported.diaper_code, "W") == 0 ||
             strcmp(imported.diaper_code, "D") == 0 ||
             strcmp(imported.diaper_code, "WD") == 0) &&
            (strcmp(imported.sleep_state, "ON") == 0 ||
             strcmp(imported.sleep_state, "OFF") == 0) &&
            (strcmp(imported.next_label, "FEED") == 0 ||
             strcmp(imported.next_label, "SLEEP") == 0 ||
             strcmp(imported.next_label, "DIAPER") == 0 ||
             strcmp(imported.next_label, "MED") == 0) &&
            backup_date(imported.reminder_date, true) &&
            ((imported.reminder_date[0] == '\0' &&
              imported.reminder_time[0] == '\0') ||
             backup_time(imported.reminder_time, false));

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, entries) {
        if (!valid || !cJSON_IsObject(item)) {
            valid = false;
            break;
        }
        baby_history_entry_t entry = {0};
        valid = backup_string(item, "date", entry.date,
                              sizeof(entry.date)) &&
                backup_string(item, "time", entry.time,
                              sizeof(entry.time)) &&
                backup_string(item, "kind", entry.kind,
                              sizeof(entry.kind)) &&
                backup_string(item, "value", entry.value,
                              sizeof(entry.value)) &&
                backup_date(entry.date, false) &&
                backup_time(entry.time, false);
        if (!valid) break;
        if (strcmp(entry.kind, "FEED") == 0) {
            char *end = NULL;
            long amount = strtol(entry.value, &end, 10);
            valid = end && *end == '\0' && amount >= 0 && amount <= 999;
        } else if (strcmp(entry.kind, "DIAP") == 0) {
            valid = strcmp(entry.value, "W") == 0 ||
                    strcmp(entry.value, "D") == 0 ||
                    strcmp(entry.value, "WD") == 0;
        } else if (strcmp(entry.kind, "SLEE") == 0) {
            valid = strcmp(entry.value, "ON") == 0 ||
                    strcmp(entry.value, "OFF") == 0;
        } else {
            valid = false;
        }
        if (!valid) break;
        imported_history.entries[imported_history.count++] = entry;
        imported_history.head = imported_history.count % HISTORY_MAX;
    }
    cJSON_Delete(root);
    if (!valid) {
        snprintf(response, response_size, "备份内容不完整或数据超出范围");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(command_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        snprintf(response, response_size, "设备正忙，请稍后再试");
        return ESP_ERR_TIMEOUT;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("baby", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "state", &imported, sizeof(imported));
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "history", &imported_history,
                           sizeof(imported_history));
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, "interval", (uint16_t)interval);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    if (err == ESP_OK) {
        state = imported;
        history = imported_history;
        err = refresh_display();
    }
    xSemaphoreGive(command_mutex);
    if (err != ESP_OK) {
        snprintf(response, response_size, "恢复失败：%s",
                 esp_err_to_name(err));
        return err;
    }
    snprintf(response, response_size, "已恢复 %u 条记录",
             (unsigned)imported_history.count);
    return ESP_OK;
}

static esp_err_t execute_command(const char *input,
                                 char *response, size_t response_size)
{
    if (!input || !response || response_size == 0) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(command_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        snprintf(response, response_size, "ERR BUSY");
        return ESP_ERR_TIMEOUT;
    }

    char line[96];
    snprintf(line, sizeof(line), "%s", input);
    char *command = strtok(line, " \t\r\n");
    if (!command) goto invalid;
    for (char *p = command; *p; ++p) *p = (char)toupper((unsigned char)*p);

    bool should_save = false;
    bool should_refresh = false;
    if (strcmp(command, "SHOW") == 0) {
        should_refresh = true;
        goto apply;
    }
    if (strcmp(command, "STATUS") == 0) {
        snprintf(response, response_size,
                 "STATE DAY=%u FEED=%s/%u DIAPER=%s/%s "
                 "SLEEP=%s/%s NEXT=%s/%s REMIND=%s/%s INTERVAL=%d",
                 (unsigned)state.day, state.feed_time, (unsigned)state.feed_ml,
                 state.diaper_time, state.diaper_code, state.sleep_time,
                 state.sleep_state, state.next_time, state.next_label,
                 state.reminder_date[0] ? state.reminder_date : "-",
                 state.reminder_time[0] ? state.reminder_time : "-",
                 history_get_interval());
        xSemaphoreGive(command_mutex);
        return ESP_OK;
    }
    if (strcmp(command, "INTERVAL") == 0) {
        char *value = strtok(NULL, " \t\r\n");
        if (!value) goto invalid;
        long minutes = strtol(value, NULL, 10);
        if (minutes < 30 || minutes > 720) goto invalid;
        history_set_interval((int)minutes);
        snprintf(response, response_size, "OK 喂奶间隔已设为 %ld 分钟",
                 minutes);
        xSemaphoreGive(command_mutex);
        return ESP_OK;
    }
    if (strcmp(command, "HIST") == 0) {
        char *value = strtok(NULL, " \t\r\n");
        int limit = 10;
        if (value) {
            long parsed = strtol(value, NULL, 10);
            if (parsed >= 1 && parsed <= HISTORY_MAX) limit = (int)parsed;
        }
        char *out = response;
        history_list(out, response_size, limit);
        xSemaphoreGive(command_mutex);
        return ESP_OK;
    }
    if (strcmp(command, "SUMMARY") == 0) {
        history_summary(response, response_size);
        xSemaphoreGive(command_mutex);
        return ESP_OK;
    }
    if (strcmp(command, "CHART") == 0) {
        char *value = strtok(NULL, " \t\r\n");
        int days = value ? atoi(value) : 1;
        history_chart(response, response_size, days);
        xSemaphoreGive(command_mutex);
        return ESP_OK;
    }
    if (strcmp(command, "UNDO") == 0) {
        history_undo(response, response_size);
        xSemaphoreGive(command_mutex);
        return ESP_OK;
    }
    if (strcmp(command, "DAY") == 0) {
        char *value = strtok(NULL, " \t\r\n");
        if (!value) goto invalid;
        long day = strtol(value, NULL, 10);
        if (day < 0 || day > 999) goto invalid;
        state.day = (uint16_t)day;
        state.day_anchor_date = current_shanghai_date();
    } else if (strcmp(command, "FEED") == 0) {
        char *time = strtok(NULL, " \t\r\n");
        char *amount = strtok(NULL, " \t\r\n");
        if (!valid_time(time) || !amount) goto invalid;
        long ml = strtol(amount, NULL, 10);
        if (ml < 0 || ml > 999) goto invalid;
        copy_token(state.feed_time, sizeof(state.feed_time), time);
        state.feed_ml = (uint16_t)ml;
        history_add("FEED", time, amount);
        next_feed_from_interval();
    } else if (strcmp(command, "DIAPER") == 0) {
        char *time = strtok(NULL, " \t\r\n");
        char *code = strtok(NULL, " \t\r\n");
        if (!valid_time(time) || !code) goto invalid;
        copy_token(state.diaper_time, sizeof(state.diaper_time), time);
        copy_token(state.diaper_code, sizeof(state.diaper_code), code);
        history_add("DIAP", time, code);
    } else if (strcmp(command, "SLEEP") == 0) {
        char *time = strtok(NULL, " \t\r\n");
        char *sleep_state = strtok(NULL, " \t\r\n");
        if (!valid_time(time) || !sleep_state) goto invalid;
        copy_token(state.sleep_time, sizeof(state.sleep_time), time);
        copy_token(state.sleep_state, sizeof(state.sleep_state), sleep_state);
        history_add("SLEE", time, sleep_state);
    } else if (strcmp(command, "NEXT") == 0) {
        char *time = strtok(NULL, " \t\r\n");
        char *label = strtok(NULL, " \t\r\n");
        if (!valid_time(time) || !label) goto invalid;
        copy_token(state.next_time, sizeof(state.next_time), time);
        copy_token(state.next_label, sizeof(state.next_label), label);
    } else if (strcmp(command, "REMIND") == 0) {
        char *date = strtok(NULL, " \t\r\n");
        char *time = strtok(NULL, " \t\r\n");
        char *label = strtok(NULL, " \t\r\n");
        if (!valid_date(date) || !valid_time(time) || !label ||
            strcmp(label, "SHOT") != 0) goto invalid;
        copy_token(state.reminder_date, sizeof(state.reminder_date), date);
        copy_token(state.reminder_time, sizeof(state.reminder_time), time);
    } else {
        goto invalid;
    }

    should_save = true;
    should_refresh = true;

apply:
    if (should_save) {
        esp_err_t err = save_state();
        if (err != ESP_OK) {
            snprintf(response, response_size, "ERR SAVE %s",
                     esp_err_to_name(err));
            xSemaphoreGive(command_mutex);
            return err;
        }
    }
    if (should_refresh) {
        esp_err_t err = refresh_display();
        if (err != ESP_OK) {
            snprintf(response, response_size, "ERR DISPLAY %s",
                     esp_err_to_name(err));
            xSemaphoreGive(command_mutex);
            return err;
        }
    }
    snprintf(response, response_size, "OK UPDATED");
    xSemaphoreGive(command_mutex);
    return ESP_OK;

invalid:
    snprintf(response, response_size, "ERR COMMAND");
    xSemaphoreGive(command_mutex);
    return ESP_ERR_INVALID_ARG;
}

static void day_clock_task(void *unused)
{
    esp_sntp_config_t config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
    while (true) {
        if (xSemaphoreTake(command_mutex, pdMS_TO_TICKS(15000)) == pdTRUE) {
            if (update_day_from_clock()) {
                save_state();
                refresh_display();
            }
            xSemaphoreGive(command_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb_config =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
    usb_serial_jtag_vfs_use_driver();

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    load_state();
    history_init();
    command_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(command_mutex ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "Quote/0 baby dashboard starting");
    err = uc8251d_init(&display_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(err));
        return;
    }
    err = refresh_display();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initial refresh failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(baby_network_start(execute_command, export_backup,
                                       import_backup));
    xTaskCreate(day_clock_task, "baby_day_clock", 4096, NULL, 4, NULL);
    printf("READY QUOTE0_BABY_V2\n");

    char line[96];
    char response[192];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        execute_command(line, response, sizeof(response));
        printf("%s\n", response);
    }
}
