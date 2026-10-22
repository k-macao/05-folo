// main/app_feed.c —— HTTPS GET + 解析。响应体上限 24 KiB（12 条中文情报约 4 KiB）。
//
// 针对实机网络做了三层加固：
//   1) 用 esp_http_client_perform + 流式事件缓冲：自动跟随 301/302 重定向，且在
//      TLS 握手完成后才按需分配响应缓冲，避免无 PSRAM 的 ESP32-C3 握手期堆紧张；
//   2) 当主地址是 raw.githubusercontent.com 时，除备用地址外自动追加 Cloudflare /
//      Gcore jsDelivr 节点与 api.github.com raw 端点，记住上次成功的节点优先直连；
//   3) 失败时提取精确的 DNS / TCP / TLS / HTTP 错误与当前 Wi-Fi IP，直接显示在屏幕上。
#include "app_feed.h"
#include "app_wifi.h"
#include "intel_feed.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_tls_errors.h"
#include "sdkconfig.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_feed";

#define FEED_MAX_BYTES       24576
#define FEED_INIT_BUF_BYTES  4096
#define FEED_HTTP_TIMEOUT_MS 9000
#define MAX_FEED_CANDIDATES  5
#define URL_BUF_BYTES        360

#ifndef CONFIG_APP_FEED_URL_FALLBACK
#define CONFIG_APP_FEED_URL_FALLBACK ""
#endif

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
    bool oom;
    esp_err_t tls_esp_err;
    int tls_code;
    int tls_flags;
} http_rx_t;

static size_t s_last_bytes;
static int s_preferred_idx;
// app_feed_fetch 只在单实例 feed_task 中调用，URL 列表放静态区省下 ~2.2 KB 任务栈。
static char s_urls[MAX_FEED_CANDIDATES][URL_BUF_BYTES];
static char s_mirror_buf[URL_BUF_BYTES];

static void extract_host(const char *url, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!url) return;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *end = strpbrk(p, ":/?#");
    const size_t n = end ? (size_t)(end - p) : strlen(p);
    const size_t keep = n < cap - 1 ? n : cap - 1;
    memcpy(out, p, keep);
    out[keep] = 0;
}

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    http_rx_t *rx = (http_rx_t *)evt->user_data;
    if (!rx) return ESP_OK;

    switch (evt->event_id) {
    case HTTP_EVENT_REDIRECT:
        // 重定向前丢弃 301/302 响应体，只保留最终目标正文。
        rx->len = 0;
        break;

    case HTTP_EVENT_ON_DATA: {
        if (evt->data_len <= 0) break;
        const int status = esp_http_client_get_status_code(evt->client);
        if (status >= 300 && status < 400) break;

        const size_t add = (size_t)evt->data_len;
        if (rx->len + add > FEED_MAX_BYTES) {
            rx->overflow = true;
            return ESP_FAIL;
        }
        if (rx->len + add + 1 > rx->cap) {
            size_t next_cap = rx->cap ? rx->cap * 2 : FEED_INIT_BUF_BYTES;
            const int64_t cl = esp_http_client_get_content_length(evt->client);
            if (rx->cap == 0 && cl > 0 && (size_t)cl <= FEED_MAX_BYTES) {
                next_cap = (size_t)cl + 1;
            }
            while (next_cap < rx->len + add + 1) next_cap *= 2;
            if (next_cap > FEED_MAX_BYTES + 1) next_cap = FEED_MAX_BYTES + 1;

            char *nbuf = realloc(rx->buf, next_cap);
            if (!nbuf) {
                rx->oom = true;
                return ESP_ERR_NO_MEM;
            }
            rx->buf = nbuf;
            rx->cap = next_cap;
        }
        memcpy(rx->buf + rx->len, evt->data, add);
        rx->len += add;
        rx->buf[rx->len] = 0;
        break;
    }

    case HTTP_EVENT_DISCONNECTED:
        if (evt->data) {
            int code = 0, flags = 0;
            const esp_err_t te = esp_tls_get_and_clear_last_error(
                (esp_tls_error_handle_t)evt->data, &code, &flags);
            if (te != ESP_OK) {
                rx->tls_esp_err = te;
                rx->tls_code = code;
                rx->tls_flags = flags;
            }
        }
        break;

    default:
        break;
    }
    return ESP_OK;
}

static void format_net_err(esp_err_t perform_err, int sock_errno,
                           const http_rx_t *rx, char *err, size_t err_cap)
{
    if (!err || err_cap == 0) return;

    if (rx->oom || perform_err == ESP_ERR_NO_MEM) {
        snprintf(err, err_cap, "内存不足");
        return;
    }
    if (rx->overflow) {
        snprintf(err, err_cap, "响应超过 %d 字节", FEED_MAX_BYTES);
        return;
    }

    const esp_err_t te = rx->tls_esp_err;
    if (te == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
        snprintf(err, err_cap, "DNS解析失败");
    } else if (te == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT ||
               perform_err == ESP_ERR_TIMEOUT || sock_errno == ETIMEDOUT) {
        snprintf(err, err_cap, "连接超时");
    } else if (te == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT) {
        snprintf(err, err_cap, "TLS握手超时");
    } else if (te == ESP_ERR_MBEDTLS_SSL_SETUP_FAILED) {
        snprintf(err, err_cap, "TLS内存不足");
    } else if (te == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED || rx->tls_code != 0) {
        const unsigned code = rx->tls_code < 0 ? (unsigned)(-rx->tls_code) : (unsigned)rx->tls_code;
        snprintf(err, err_cap, "TLS握手失败(0x%X)", code);
    } else if (te == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) {
        if (sock_errno > 0) snprintf(err, err_cap, "无法连接(errno=%d)", sock_errno);
        else snprintf(err, err_cap, "无法连接主机");
    } else if (sock_errno > 0) {
        snprintf(err, err_cap, "网络错误(errno=%d)", sock_errno);
    } else {
        snprintf(err, err_cap, "连接失败(0x%X)", (unsigned)perform_err);
    }
}

static app_feed_result_t fetch_once(const char *url, intel_feed_t *out, int *dropped,
                                    char *err, size_t err_cap)
{
    http_rx_t rx = { 0 };

    ESP_LOGI(TAG, "拉取 %s (free=%u largest=%u)", url,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = FEED_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .max_redirection_count = 4,
        .event_handler = on_http_event,
        .user_data = &rx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "FoloPassport/1.0 (005-signal-log)",
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (err && err_cap) snprintf(err, err_cap, "内存不足");
        return APP_FEED_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Cache-Control", "no-cache");
    if (strstr(url, "://api.github.com/") != NULL) {
        esp_http_client_set_header(client, "Accept", "application/vnd.github.raw");
    }

    app_feed_result_t result = APP_FEED_ERR_HTTP;
    const esp_err_t pe = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    const int sock_errno = esp_http_client_get_errno(client);

    if (pe != ESP_OK) {
        format_net_err(pe, sock_errno, &rx, err, err_cap);
        if (rx.oom || pe == ESP_ERR_NO_MEM) result = APP_FEED_ERR_NO_MEM;
        goto done;
    }

    if (status != 200) {
        if (err && err_cap) snprintf(err, err_cap, "HTTP %d", status);
        goto done;
    }

    if (!rx.buf || rx.len == 0) {
        if (err && err_cap) snprintf(err, err_cap, "响应为空");
        goto done;
    }

    if (intel_feed_parse(rx.buf, rx.len, out, dropped, err, err_cap)) {
        result = APP_FEED_OK;
        s_last_bytes = rx.len;
        ESP_LOGI(TAG, "已更新 %d 条，丢弃 %d 条，%u 字节", out->count,
                 dropped ? *dropped : 0, (unsigned)rx.len);
    } else {
        result = APP_FEED_ERR_PARSE;
    }

done:
    esp_http_client_cleanup(client);
    free(rx.buf);
    return result;
}

static void add_candidate(int *count, const char *url)
{
    if (!url || !url[0] || *count >= MAX_FEED_CANDIDATES) return;
    if (strlen(url) >= URL_BUF_BYTES) return;
    for (int i = 0; i < *count; i++) {
        if (strcmp(s_urls[i], url) == 0) return;
    }
    snprintf(s_urls[*count], URL_BUF_BYTES, "%s", url);
    (*count)++;
}

// 若主地址形如 https://raw.githubusercontent.com/<owner>/<repo>/<ref>/<path>，
// 自动生成不经过 raw.githubusercontent.com / cdn.jsdelivr.net 的备用镜像。
static void add_github_raw_mirrors(int *count, const char *raw_url)
{
    const char *prefix = "https://raw.githubusercontent.com/";
    const size_t plen = strlen(prefix);
    if (!raw_url || strncmp(raw_url, prefix, plen) != 0) return;

    const char *p = raw_url + plen;
    const char *s1 = strchr(p, '/');
    if (!s1 || s1 == p) return;
    const char *s2 = strchr(s1 + 1, '/');
    if (!s2 || s2 == s1 + 1) return;
    const char *s3 = strchr(s2 + 1, '/');
    if (!s3 || s3 == s2 + 1 || !s3[1]) return;

    char owner[48], repo[48], ref[48], path[128];
    if ((size_t)(s1 - p) >= sizeof owner ||
        (size_t)(s2 - s1 - 1) >= sizeof repo ||
        (size_t)(s3 - s2 - 1) >= sizeof ref ||
        strlen(s3 + 1) >= sizeof path) {
        return;
    }

    memcpy(owner, p, (size_t)(s1 - p));
    owner[s1 - p] = 0;
    memcpy(repo, s1 + 1, (size_t)(s2 - s1 - 1));
    repo[s2 - s1 - 1] = 0;
    memcpy(ref, s2 + 1, (size_t)(s3 - s2 - 1));
    ref[s3 - s2 - 1] = 0;
    snprintf(path, sizeof path, "%s", s3 + 1);

    int n = snprintf(s_mirror_buf, sizeof s_mirror_buf,
                     "https://testingcf.jsdelivr.net/gh/%s/%s@%s/%s",
                     owner, repo, ref, path);
    if (n > 0 && (size_t)n < sizeof s_mirror_buf) add_candidate(count, s_mirror_buf);

    n = snprintf(s_mirror_buf, sizeof s_mirror_buf,
                 "https://gcore.jsdelivr.net/gh/%s/%s@%s/%s",
                 owner, repo, ref, path);
    if (n > 0 && (size_t)n < sizeof s_mirror_buf) add_candidate(count, s_mirror_buf);

    n = snprintf(s_mirror_buf, sizeof s_mirror_buf,
                 "https://api.github.com/repos/%s/%s/contents/%s?ref=%s",
                 owner, repo, path, ref);
    if (n > 0 && (size_t)n < sizeof s_mirror_buf) add_candidate(count, s_mirror_buf);
}

app_feed_result_t app_feed_fetch(intel_feed_t *out, int *dropped,
                                 char *err, size_t err_cap)
{
    if (err && err_cap) err[0] = 0;
    s_last_bytes = 0;

    if (!app_wifi_is_connected()) {
        if (app_wifi_connect(20000) != ESP_OK) {
            if (err && err_cap) {
                char wifi_line[80];
                app_wifi_describe(wifi_line, sizeof wifi_line);
                snprintf(err, err_cap, "%s\n提示: 请确认 2.4GHz 与密码", wifi_line);
            }
            return APP_FEED_ERR_WIFI;
        }
    }

    int count = 0;
    add_candidate(&count, CONFIG_APP_FEED_URL);
    add_candidate(&count, CONFIG_APP_FEED_URL_FALLBACK);
    add_github_raw_mirrors(&count, CONFIG_APP_FEED_URL);

    if (count == 0) {
        if (err && err_cap) snprintf(err, err_cap, "未配置情报地址");
        return APP_FEED_ERR_HTTP;
    }
    if (s_preferred_idx < 0 || s_preferred_idx >= count) s_preferred_idx = 0;

    char primary_err[64] = { 0 };
    char primary_host[48] = { 0 };
    char last_err[64] = { 0 };
    char last_host[48] = { 0 };
    app_feed_result_t last_res = APP_FEED_ERR_HTTP;

    for (int step = 0; step < count; step++) {
        const int idx = (step == 0) ? s_preferred_idx
                                    : (step <= s_preferred_idx ? step - 1 : step);
        char cur_err[64] = { 0 };
        const app_feed_result_t res = fetch_once(s_urls[idx], out, dropped,
                                                 cur_err, sizeof cur_err);
        if (res == APP_FEED_OK) {
            s_preferred_idx = idx;
            return APP_FEED_OK;
        }

        last_res = res;
        snprintf(last_err, sizeof last_err, "%s", cur_err[0] ? cur_err : "拉取失败");
        extract_host(s_urls[idx], last_host, sizeof last_host);
        if (idx == 0 || !primary_err[0]) {
            snprintf(primary_err, sizeof primary_err, "%s", last_err);
            snprintf(primary_host, sizeof primary_host, "%s", last_host);
        }
        ESP_LOGW(TAG, "候选[%d] %s 失败: %s", idx, last_host, last_err);
    }

    if (err && err_cap) {
        char wifi_line[80];
        app_wifi_describe(wifi_line, sizeof wifi_line);
        if (count > 1 && strcmp(primary_host, last_host) != 0) {
            snprintf(err, err_cap, "主源: %s\n(%s)\n备源: %s\n(%s)\n%s",
                     primary_err, primary_host, last_err, last_host, wifi_line);
        } else {
            snprintf(err, err_cap, "拉取: %s\n(%s)\n%s",
                     primary_err, primary_host, wifi_line);
        }
        ESP_LOGE(TAG, "全部源失败:\n%s", err);
    }
    return last_res;
}

size_t app_feed_last_bytes(void) { return s_last_bytes; }
