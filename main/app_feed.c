// main/app_feed.c —— 一次 HTTP GET + 解析。响应体上限 24 KiB（12 条中文情报约 4 KiB）。
#include "app_feed.h"
#include "app_wifi.h"
#include "intel_feed.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_feed";

#define FEED_MAX_BYTES 24576
#define FEED_HTTP_TIMEOUT_MS 12000

static size_t s_last_bytes;

app_feed_result_t app_feed_fetch(intel_feed_t *out, int *dropped,
                                 char *err, size_t err_cap)
{
    if (err && err_cap) err[0] = 0;
    s_last_bytes = 0;

    if (!app_wifi_is_connected()) {
        if (app_wifi_connect(20000) != ESP_OK) {
            if (err && err_cap) snprintf(err, err_cap, "Wi-Fi 未连接 (%s)", CONFIG_APP_WIFI_SSID);
            return APP_FEED_ERR_WIFI;
        }
    }

    char *body = malloc(FEED_MAX_BYTES + 1);
    if (!body) return APP_FEED_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = CONFIG_APP_FEED_URL,
        .timeout_ms = FEED_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "FoloPassport/1.0 (005-signal-log)",
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        return APP_FEED_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Cache-Control", "no-cache");

    app_feed_result_t result = APP_FEED_ERR_HTTP;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        if (err && err_cap) snprintf(err, err_cap, "连接 %s 失败", CONFIG_APP_FEED_URL);
        goto done;
    }

    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        if (err && err_cap) snprintf(err, err_cap, "HTTP %d", status);
        goto done;
    }

    size_t total = 0;
    for (;;) {
        const int want = (int)(FEED_MAX_BYTES - total);
        if (want <= 0) {
            if (err && err_cap) snprintf(err, err_cap, "响应超过 %d 字节", FEED_MAX_BYTES);
            goto done;
        }
        const int n = esp_http_client_read(client, body + total, want);
        if (n < 0) {
            if (err && err_cap) snprintf(err, err_cap, "读取响应失败");
            goto done;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    body[total] = 0;
    s_last_bytes = total;

    if (total == 0) {
        if (err && err_cap) snprintf(err, err_cap, "响应为空");
        goto done;
    }

    if (intel_feed_parse(body, total, out, dropped, err, err_cap)) {
        result = APP_FEED_OK;
        ESP_LOGI(TAG, "已更新 %d 条，丢弃 %d 条，%u 字节", out->count,
                 dropped ? *dropped : 0, (unsigned)total);
    } else {
        result = APP_FEED_ERR_PARSE;
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body);
    if (result != APP_FEED_OK) {
        ESP_LOGE(TAG, "拉取失败: %s", err && err[0] ? err : "unknown");
    }
    return result;
}

size_t app_feed_last_bytes(void) { return s_last_bytes; }
