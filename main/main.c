// main/main.c —— 005「章鱼AI·事件雷达」在 FoloToy AI Passport 上的显示端。
//
// 三个按键：
//   列表   ▲▼ 移动   OK 短按=详情   OK 长按=立即刷新
//   详情   ▲▼ 滚动   OK 短按=翻页   OK 长按=返回列表
//   启动/错误  OK（短或长）=重新拉取
//
// 数据流：Wi-Fi → HTTPS 拉 feed.json → intel_feed_parse → intel_detail_build → ui_crt_render
#include "app_feed.h"
#include "app_wifi.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "intel.h"
#include "intel_logic.h"
#include "ui_crt.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "signal_log";

#define INPUT_QUEUE_DEPTH 8
#define INPUT_TASK_STACK  6144
#define FEED_TASK_STACK   16384
#define REFRESH_PERIOD_US ((int64_t)CONFIG_APP_REFRESH_MINUTES * 60 * 1000000)

typedef enum {
    SCR_BOOT = 0,
    SCR_LIST,
    SCR_DETAIL,
    SCR_ERROR,
} screen_id_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

// s_feed 由 bsp_lvgl_lock() 互斥访问；解析在 HTTP 关闭后再临时申请堆缓冲，省下 11.3 KB 常驻 .bss。
static intel_feed_t s_feed;

static screen_id_t s_screen = SCR_BOOT;
static bool s_have_feed;
static bool s_busy;
static bool s_stale;
static int s_selected;
static int s_detail_index;
static int s_detail_first;
static int s_detail_count;
static intel_line_t s_detail_lines[INTEL_DETAIL_MAX_LINES];
static char s_clock[8] = "--:--";
static char s_err[256] = "等待首次同步";
static char s_fetch_err[256];
static int s_battery = -1;
static int64_t s_sync_us;
static uint32_t s_last_minute_key;

static QueueHandle_t s_input_queue;
static volatile bool s_refresh_request;

// 逐行做 intel_normalize 并保留换行符 \n，让状态页能按段落显示多行诊断信息。
static void normalize_lines(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!src) return;

    size_t used = 0;
    const char *p = src;
    char raw_line[96];
    char clean[96];

    while (*p && used + 2 < cap) {
        const char *nl = strchr(p, '\n');
        const size_t raw_len = nl ? (size_t)(nl - p) : strlen(p);
        const size_t keep = raw_len < sizeof raw_line - 1 ? raw_len : sizeof raw_line - 1;
        memcpy(raw_line, p, keep);
        raw_line[keep] = 0;

        intel_normalize(clean, sizeof clean, raw_line);

        const size_t clen = strlen(clean);
        if (used + clen + 2 >= cap) break;
        if (clen > 0) {
            memcpy(dst + used, clean, clen);
            used += clen;
        }
        if (nl && used + 1 < cap) {
            dst[used++] = '\n';
        }
        dst[used] = 0;
        if (!nl) break;
        p = nl + 1;
    }
}

// --------------------------------------------------------------------------- //
// 时间：没有 RTC，用 feed 的 generated_at 做基准 + 开机计时，够显示"几分钟前"。
// --------------------------------------------------------------------------- //
static int64_t now_epoch(void)
{
    if (!s_have_feed || s_feed.generated_epoch <= 0) return 0;
    return s_feed.generated_epoch + (esp_timer_get_time() - s_sync_us) / 1000000;
}

static void update_clock(void)
{
    const int64_t e = now_epoch();
    if (e > 0) intel_hhmm_from_epoch(e, s_clock);
    else snprintf(s_clock, sizeof s_clock, "--:--");
}

static uint32_t minute_key(void)
{
    const int64_t e = now_epoch();
    return e > 0 ? (uint32_t)(e / 60) : 0;
}

// --------------------------------------------------------------------------- //
// 渲染（调用者持有 LVGL 锁）
// --------------------------------------------------------------------------- //
static void render_locked(void)
{
    ui_view_t v = { 0 };
    v.clock = s_clock;
    v.battery_pct = s_battery;
    v.busy = s_busy;
    v.stale = s_stale;
    v.feed = &s_feed;

    switch (s_screen) {
    case SCR_LIST:
        v.kind = UI_VIEW_LIST;
        v.selected = s_selected;
        break;
    case SCR_DETAIL:
        v.kind = UI_VIEW_DETAIL;
        v.detail_index = s_detail_index;
        v.detail_first_line = s_detail_first;
        v.detail_line_count = s_detail_count;
        v.detail_lines = s_detail_lines;
        break;
    case SCR_ERROR:
        v.kind = UI_VIEW_ERROR;
        v.headline = "NO SIGNAL";
        v.message = s_err;
        v.hint = "OK 重试";
        break;
    case SCR_BOOT:
    default:
        v.kind = UI_VIEW_BOOT;
        v.headline = "SIGNAL.LOG";
        v.message = "005 · 章鱼AI 事件雷达\n开机中，等待第一轮情报…";
        v.hint = "OK 重试";
        break;
    }
    ui_crt_render(&v);
}

static void render(void)
{
    if (!bsp_lvgl_lock(500)) return;
    render_locked();
    bsp_lvgl_unlock();
}

static void refresh_battery(void)
{
    const int soc = bsp_battery_soc();
    if (soc >= 0) s_battery = soc;
}

// --------------------------------------------------------------------------- //
// 视图切换
// --------------------------------------------------------------------------- //
static void open_detail(int index)
{
    if (!s_have_feed || index < 0 || index >= s_feed.count) return;
    s_detail_index = index;
    s_detail_first = 0;
    s_detail_count = intel_detail_build(&s_feed.items[index], now_epoch(),
                                       s_detail_lines, INTEL_DETAIL_MAX_LINES);
    s_screen = SCR_DETAIL;
}

static void request_refresh(void) { s_refresh_request = true; }

// --------------------------------------------------------------------------- //
// 按键
// --------------------------------------------------------------------------- //
static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    const bool click = ev == BSP_BTN_CLICK;
    const bool long_press = ev == BSP_BTN_LONG;
    if (!click && !long_press) return;
    if (!bsp_lvgl_lock(500)) return;

    switch (s_screen) {
    case SCR_LIST:
        if (click && btn == BSP_BTN_UP) {
            s_selected = intel_next_index(s_selected, s_feed.count, -1);
        } else if (click && btn == BSP_BTN_DOWN) {
            s_selected = intel_next_index(s_selected, s_feed.count, 1);
        } else if (click && btn == BSP_BTN_OK) {
            open_detail(s_selected);
        } else if (long_press && btn == BSP_BTN_OK) {
            request_refresh();
        }
        break;

    case SCR_DETAIL:
        if (click && btn == BSP_BTN_UP) {
            s_detail_first = s_detail_first > 0 ? s_detail_first - 1 : 0;
        } else if (click && btn == BSP_BTN_DOWN) {
            const int max_first = s_detail_count > UI_DETAIL_ROWS ? s_detail_count - UI_DETAIL_ROWS : 0;
            if (s_detail_first < max_first) s_detail_first++;
        } else if (click && btn == BSP_BTN_OK) {
            const int max_first = s_detail_count > UI_DETAIL_ROWS ? s_detail_count - UI_DETAIL_ROWS : 0;
            s_detail_first += UI_DETAIL_ROWS;
            if (s_detail_first > max_first) s_detail_first = 0;   // 翻到底再按一次回到开头
        } else if (long_press && btn == BSP_BTN_OK) {
            s_screen = SCR_LIST;
        }
        break;

    case SCR_BOOT:
    case SCR_ERROR:
    default:
        if (btn == BSP_BTN_OK) {
            request_refresh();
        }
        break;
    }

    render_locked();
    bsp_lvgl_unlock();
}

// button 回调跑在共享 esp_timer 任务里，只允许入队。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;
    if (!s_input_queue) return;
    const input_event_t in = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &in, 0);
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t in;
    for (;;) {
        if (xQueueReceive(s_input_queue, &in, portMAX_DELAY) != pdTRUE) continue;
        refresh_battery();
        handle_key(in.btn, in.event);   // handle_key 自己负责加锁/解锁
    }
}

// --------------------------------------------------------------------------- //
// 拉取线程
// --------------------------------------------------------------------------- //
static void do_refresh(void)
{
    s_refresh_request = false;
    if (!s_busy) {
        s_busy = true;
        render();
    }

    ESP_LOGI(TAG, "开始拉取 %s", CONFIG_APP_FEED_URL);
    int dropped = 0;
    intel_feed_t *incoming = NULL;
    s_fetch_err[0] = 0;
    const app_feed_result_t result = app_feed_fetch(&incoming, &dropped,
                                                    s_fetch_err, sizeof s_fetch_err);

    if (!bsp_lvgl_lock(1000)) {
        free(incoming);
        return;
    }

    if (result == APP_FEED_OK && incoming) {
        s_feed = *incoming;
        free(incoming);
        incoming = NULL;
        s_have_feed = true;
        s_stale = false;
        s_sync_us = esp_timer_get_time();
        s_selected = intel_clamp_index(s_selected, s_feed.count);
        update_clock();
        if (s_screen != SCR_DETAIL) s_screen = SCR_LIST;
        ESP_LOGI(TAG, "同步成功：%d 条（丢弃 %d）", s_feed.count, dropped);
    } else if (result != APP_FEED_ERR_WIFI || !s_have_feed) {
        free(incoming);
        s_stale = true;
        normalize_lines(s_err, sizeof s_err, s_fetch_err[0] ? s_fetch_err : "拉取失败");
        if (!s_have_feed) s_screen = SCR_ERROR;
        ESP_LOGW(TAG, "同步失败：%s", s_err);
    } else {
        free(incoming);
        s_stale = true;
    }

    s_busy = false;
    render_locked();
    bsp_lvgl_unlock();
}

static void feed_task(void *arg)
{
    (void)arg;
    int64_t next_refresh_us = 0;

    for (;;) {
        const int64_t t = esp_timer_get_time();
        if (s_refresh_request || t >= next_refresh_us) {
            do_refresh();
            next_refresh_us = esp_timer_get_time() + REFRESH_PERIOD_US;
        }

        // 顶栏时钟 / "几分钟前" 每分钟变一次，重绘一次即可。
        const uint32_t key = minute_key();
        if (key != s_last_minute_key) {
            s_last_minute_key = key;
            if (s_have_feed) {
                update_clock();
                if (bsp_lvgl_lock(300)) {
                    render_locked();
                    bsp_lvgl_unlock();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// --------------------------------------------------------------------------- //
void app_main(void)
{
    ESP_LOGI(TAG, "005 signal log 启动");

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK) ESP_LOGW(TAG, "NVS 初始化失败: %s", esp_err_to_name(nvs));

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败，无法继续");
        return;
    }
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计不可用，顶栏不显示电量");

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "按键队列创建失败");
        return;
    }
    if (xTaskCreate(input_task, "input", INPUT_TASK_STACK, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "按键任务创建失败");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        // 没有按键仍可以自动刷新，只是无法翻页：不要因为外设坏掉就重启循环。
        ESP_LOGE(TAG, "按键初始化失败，仅保留自动刷新");
        normalize_lines(s_err, sizeof s_err, "按键不可用");
    }

    refresh_battery();
    s_busy = true;
    render();
    // 让 esp_lvgl_port 自身任务先刷完首帧黑底再开背光（不要在 app_main 里跨任务调 lv_refr_now）。
    vTaskDelay(pdMS_TO_TICKS(40));
    bsp_display_backlight(100);

    if (xTaskCreate(feed_task, "feed", FEED_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "拉取任务创建失败");
        s_busy = false;
        normalize_lines(s_err, sizeof s_err, "拉取任务创建失败");
        s_screen = SCR_ERROR;
        render();
    }
}
