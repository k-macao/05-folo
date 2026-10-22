// main/app_feed.c —— HTTPS GET + 解析。响应体上限 24 KiB（12 条中文情报约 4 KiB）。
//
// 针对无 PSRAM 的 ESP32-C3 与实机网络做了四层加固：
//   1) 内存峰值错峰：发送 Connection: close，在 esp_http_client_perform 返回后立即
//      esp_http_client_cleanup 释放 ~39 KB 的 mbedTLS/HTTP 缓冲，再做 gzip 解压与
//      cJSON 解析，彻底消除 TLS 会话未关导致的 cJSON 内存耗尽（假性 JSON 报错）；
//   2) 严格校验流式缓冲：初始缓冲 8 KiB，无条件检查 rx.oom / rx.overflow 与响应完整性，
//      杜绝 esp_http_client 忽略回调返回值造成的半截 JSON；
//   3) 兼容 gzip 与 GitHub Contents API：同时支持 raw 正文、gzip 压缩流（ROM miniz 解压）
//      及 base64 JSON 包装；
//   4) 镜像优先级优化：raw.githubusercontent.com 不可达时紧接着尝试实机已验证可达的
//      api.github.com，再回退 jsDelivr 各节点，并记住上次成功节点优先直连。
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
#include "miniz.h"
#include "sdkconfig.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_feed";

#define FEED_MAX_BYTES       24576
#define FEED_INIT_BUF_BYTES  8192
#define FEED_HTTP_TIMEOUT_MS 7000
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

static void capture_tls_error_handle(http_rx_t *rx, esp_tls_error_handle_t h)
{
    if (!rx || !h) return;
    int code = 0, flags = 0;
    const esp_err_t te = esp_tls_get_and_clear_last_error(h, &code, &flags);
    if (te != ESP_OK) {
        rx->tls_esp_err = te;
        rx->tls_code = code;
        rx->tls_flags = flags;
    }
}

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    http_rx_t *rx = (http_rx_t *)evt->user_data;
    if (!rx) return ESP_OK;

    switch (evt->event_id) {
    case HTTP_EVENT_REDIRECT:
        rx->len = 0;
        rx->oom = false;
        rx->overflow = false;
        break;

    case HTTP_EVENT_ON_DATA: {
        if (evt->data_len <= 0 || rx->oom || rx->overflow) break;
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

    case HTTP_EVENT_ERROR:
    case HTTP_EVENT_DISCONNECTED:
        if (evt->data) {
            capture_tls_error_handle(rx, (esp_tls_error_handle_t)evt->data);
        }
        break;

    default:
        break;
    }
    return ESP_OK;
}

// 若网关/代理忽略了 Accept-Encoding: identity 返回了 gzip（1F 8B），用 ROM miniz 原地解压。
static bool maybe_gunzip(http_rx_t *rx, char *err, size_t err_cap)
{
    if (!rx || !rx->buf || rx->len < 2) return true;
    const unsigned char *b = (const unsigned char *)rx->buf;
    if (b[0] != 0x1F || b[1] != 0x8B) return true;

    if (rx->len < 18 || b[2] != 8) {
        if (err && err_cap) snprintf(err, err_cap, "Gzip头损坏(%uB)", (unsigned)rx->len);
        return false;
    }

    const unsigned char flg = b[3];
    size_t pos = 10;
    if (flg & 0x04) {   // FEXTRA
        if (pos + 2 > rx->len - 8) return false;
        const size_t xlen = (size_t)b[pos] | ((size_t)b[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flg & 0x08) {   // FNAME
        while (pos < rx->len - 8 && b[pos] != 0) pos++;
        pos++;
    }
    if (flg & 0x10) {   // FCOMMENT
        while (pos < rx->len - 8 && b[pos] != 0) pos++;
        pos++;
    }
    if (flg & 0x02) {   // FHCRC
        pos += 2;
    }
    if (pos >= rx->len - 8) {
        if (err && err_cap) snprintf(err, err_cap, "Gzip头越界");
        return false;
    }

    const uint32_t isize = (uint32_t)b[rx->len - 4] |
                           ((uint32_t)b[rx->len - 3] << 8) |
                           ((uint32_t)b[rx->len - 2] << 16) |
                           ((uint32_t)b[rx->len - 1] << 24);
    size_t out_cap = (isize > 0 && isize <= FEED_MAX_BYTES)
                         ? ((size_t)isize + 1)
                         : (FEED_MAX_BYTES + 1);

    tinfl_decompressor *decomp = calloc(1, sizeof(*decomp));
    char *out_buf = malloc(out_cap);
    if (!decomp || !out_buf) {
        free(decomp);
        free(out_buf);
        rx->oom = true;
        if (err && err_cap) snprintf(err, err_cap, "解压内存不足");
        return false;
    }

    tinfl_init(decomp);
    size_t in_bytes = (rx->len - 8) - pos;
    size_t out_bytes = out_cap - 1;
    const tinfl_status st = tinfl_decompress(
        decomp,
        (const mz_uint8 *)(b + pos), &in_bytes,
        (mz_uint8 *)out_buf, (mz_uint8 *)out_buf, &out_bytes,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(decomp);

    if (st != TINFL_STATUS_DONE) {
        free(out_buf);
        if (err && err_cap) snprintf(err, err_cap, "Gzip解压失败(%d)", (int)st);
        return false;
    }

    out_buf[out_bytes] = 0;
    free(rx->buf);
    rx->buf = out_buf;
    rx->len = out_bytes;
    rx->cap = out_cap;
    return true;
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
               perform_err == ESP_ERR_TIMEOUT ||
               perform_err == ESP_ERR_HTTP_CONNECTING ||
               perform_err == ESP_ERR_HTTP_READ_TIMEOUT ||
               sock_errno == ETIMEDOUT) {
        snprintf(err, err_cap, "连接超时");
    } else if (te == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT) {
        snprintf(err, err_cap, "TLS握手超时");
    } else if (te == ESP_ERR_MBEDTLS_SSL_SETUP_FAILED) {
        snprintf(err, err_cap, "TLS内存不足");
    } else if (te == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED || rx->tls_code != 0) {
        const unsigned code = rx->tls_code < 0 ? (unsigned)(-rx->tls_code) : (unsigned)rx->tls_code;
        snprintf(err, err_cap, "TLS握手失败(0x%X)", code);
    } else if (te == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST ||
               perform_err == ESP_ERR_HTTP_CONNECT) {
        if (sock_errno > 0) snprintf(err, err_cap, "无法连接(errno=%d)", sock_errno);
        else snprintf(err, err_cap, "连接被阻断");
    } else if (perform_err == ESP_ERR_HTTP_INCOMPLETE_DATA ||
               perform_err == ESP_ERR_HTTP_CONNECTION_CLOSED) {
        snprintf(err, err_cap, "连接中断(%uB)", (unsigned)rx->len);
    } else if (perform_err == ESP_ERR_HTTP_FETCH_HEADER) {
        snprintf(err, err_cap, "响应头读取失败");
    } else if (sock_errno > 0) {
        snprintf(err, err_cap, "网络错误(errno=%d)", sock_errno);
    } else {
        snprintf(err, err_cap, "连接失败(0x%X)", (unsigned)perform_err);
    }
}

static app_feed_result_t fetch_once(const char *url, intel_feed_t **out_parsed,
                                    int *dropped, char *err, size_t err_cap)
{
    http_rx_t rx = { 0 };

    ESP_LOGI(TAG, "拉取 %s (free=%u largest=%u)", url,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = FEED_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
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
        if (err && err_cap) snprintf(err, err_cap, "HTTP内存不足");
        return APP_FEED_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Cache-Control", "no-cache");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Connection", "close");
    if (strstr(url, "://api.github.com/") != NULL) {
        esp_http_client_set_header(client, "Accept", "application/vnd.github.raw");
        esp_http_client_set_header(client, "X-GitHub-Api-Version", "2022-11-28");
    }

    const esp_err_t pe = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    const int sock_errno = esp_http_client_get_errno(client);
    const bool complete = esp_http_client_is_complete_data_received(client);
    if (rx.tls_esp_err == ESP_OK) {
        int code = 0, flags = 0;
        const esp_err_t te = esp_http_client_get_and_clear_last_tls_error(client, &code, &flags);
        if (te != ESP_OK) {
            rx.tls_esp_err = te;
            rx.tls_code = code;
            rx.tls_flags = flags;
        }
    }

    // 关键：在任何解压 / cJSON 解析之前先销毁 HTTP + mbedTLS 会话，立刻归还 ~39 KB 堆内存！
    esp_http_client_cleanup(client);
    client = NULL;

    if (rx.oom) {
        free(rx.buf);
        if (err && err_cap) snprintf(err, err_cap, "接收内存不足");
        return APP_FEED_ERR_NO_MEM;
    }
    if (rx.overflow) {
        free(rx.buf);
        if (err && err_cap) snprintf(err, err_cap, "响应超过 %d 字节", FEED_MAX_BYTES);
        return APP_FEED_ERR_HTTP;
    }
    if (pe != ESP_OK) {
        format_net_err(pe, sock_errno, &rx, err, err_cap);
        free(rx.buf);
        return (pe == ESP_ERR_NO_MEM) ? APP_FEED_ERR_NO_MEM : APP_FEED_ERR_HTTP;
    }
    if (status != 200) {
        free(rx.buf);
        if (err && err_cap) snprintf(err, err_cap, "HTTP %d", status);
        return APP_FEED_ERR_HTTP;
    }
    if (!rx.buf || rx.len == 0) {
        free(rx.buf);
        if (err && err_cap) snprintf(err, err_cap, "响应为空");
        return APP_FEED_ERR_HTTP;
    }
    if (!complete) {
        if (err && err_cap) snprintf(err, err_cap, "响应不完整(%uB)", (unsigned)rx.len);
        free(rx.buf);
        return APP_FEED_ERR_HTTP;
    }

    if (!maybe_gunzip(&rx, err, err_cap)) {
        free(rx.buf);
        return rx.oom ? APP_FEED_ERR_NO_MEM : APP_FEED_ERR_PARSE;
    }

    ESP_LOGI(TAG, "开始解析 %u 字节 (free=%u largest=%u)",
             (unsigned)rx.len,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    intel_feed_t *parsed = calloc(1, sizeof(*parsed));
    if (!parsed) {
        free(rx.buf);
        if (err && err_cap) snprintf(err, err_cap, "解析内存不足");
        return APP_FEED_ERR_NO_MEM;
    }

    const size_t body_len = rx.len;
    const bool ok = intel_feed_parse(rx.buf, rx.len, parsed, dropped, err, err_cap);
    free(rx.buf);
    rx.buf = NULL;

    if (!ok) {
        free(parsed);
        return APP_FEED_ERR_PARSE;
    }

    s_last_bytes = body_len;
    *out_parsed = parsed;
    ESP_LOGI(TAG, "已更新 %d 条，丢弃 %d 条，%u 字节", parsed->count,
             dropped ? *dropped : 0, (unsigned)body_len);
    return APP_FEED_OK;
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

static bool parse_github_raw_url(const char *raw_url, char owner[48], char repo[48],
                                 char ref[48], char path[128])
{
    const char *prefix = "https://raw.githubusercontent.com/";
    const size_t plen = strlen(prefix);
    if (!raw_url || strncmp(raw_url, prefix, plen) != 0) return false;

    const char *p = raw_url + plen;
    const char *s1 = strchr(p, '/');
    if (!s1 || s1 == p) return false;
    const char *s2 = strchr(s1 + 1, '/');
    if (!s2 || s2 == s1 + 1) return false;
    const char *s3 = strchr(s2 + 1, '/');
    if (!s3 || s3 == s2 + 1 || !s3[1]) return false;

    if ((size_t)(s1 - p) >= 48 ||
        (size_t)(s2 - s1 - 1) >= 48 ||
        (size_t)(s3 - s2 - 1) >= 48 ||
        strlen(s3 + 1) >= 128) {
        return false;
    }

    memcpy(owner, p, (size_t)(s1 - p));
    owner[s1 - p] = 0;
    memcpy(repo, s1 + 1, (size_t)(s2 - s1 - 1));
    repo[s2 - s1 - 1] = 0;
    memcpy(ref, s2 + 1, (size_t)(s3 - s2 - 1));
    ref[s3 - s2 - 1] = 0;
    snprintf(path, 128, "%s", s3 + 1);
    return true;
}

// 候选顺序：主地址 → api.github.com（跨境/国内直连最稳）→ 用户配置备用源 → jsDelivr 镜像节点。
static void build_candidates(int *count)
{
    add_candidate(count, CONFIG_APP_FEED_URL);

    char owner[48], repo[48], ref[48], path[128];
    const bool is_gh_raw = parse_github_raw_url(CONFIG_APP_FEED_URL, owner, repo, ref, path);

    if (is_gh_raw) {
        int n = snprintf(s_mirror_buf, sizeof s_mirror_buf,
                         "https://api.github.com/repos/%s/%s/contents/%s?ref=%s",
                         owner, repo, path, ref);
        if (n > 0 && (size_t)n < sizeof s_mirror_buf) add_candidate(count, s_mirror_buf);
    }

    add_candidate(count, CONFIG_APP_FEED_URL_FALLBACK);

    if (is_gh_raw) {
        int n = snprintf(s_mirror_buf, sizeof s_mirror_buf,
                         "https://testingcf.jsdelivr.net/gh/%s/%s@%s/%s",
                         owner, repo, ref, path);
        if (n > 0 && (size_t)n < sizeof s_mirror_buf) add_candidate(count, s_mirror_buf);

        n = snprintf(s_mirror_buf, sizeof s_mirror_buf,
                     "https://gcore.jsdelivr.net/gh/%s/%s@%s/%s",
                     owner, repo, ref, path);
        if (n > 0 && (size_t)n < sizeof s_mirror_buf) add_candidate(count, s_mirror_buf);
    }
}

app_feed_result_t app_feed_fetch(intel_feed_t **out_parsed, int *dropped,
                                 char *err, size_t err_cap)
{
    if (out_parsed) *out_parsed = NULL;
    if (err && err_cap) err[0] = 0;
    s_last_bytes = 0;

    if (!out_parsed) {
        if (err && err_cap) snprintf(err, err_cap, "参数无效");
        return APP_FEED_ERR_PARSE;
    }

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
    build_candidates(&count);

    if (count == 0) {
        if (err && err_cap) snprintf(err, err_cap, "未配置情报地址");
        return APP_FEED_ERR_HTTP;
    }
    if (s_preferred_idx < 0 || s_preferred_idx >= count) s_preferred_idx = 0;

    char primary_err[64] = { 0 };
    char primary_host[48] = { 0 };
    char alt_err[64] = { 0 };
    char alt_host[48] = { 0 };
    bool alt_informative = false;
    app_feed_result_t last_res = APP_FEED_ERR_HTTP;

    for (int step = 0; step < count; step++) {
        const int idx = (step == 0) ? s_preferred_idx
                                    : (step <= s_preferred_idx ? step - 1 : step);
        char cur_err[64] = { 0 };
        const app_feed_result_t res = fetch_once(s_urls[idx], out_parsed, dropped,
                                                 cur_err, sizeof cur_err);
        if (res == APP_FEED_OK) {
            s_preferred_idx = idx;
            return APP_FEED_OK;
        }

        last_res = res;
        const char *msg = cur_err[0] ? cur_err : "拉取失败";
        char host[48] = { 0 };
        extract_host(s_urls[idx], host, sizeof host);
        if (idx == 0 || !primary_err[0]) {
            snprintf(primary_err, sizeof primary_err, "%s", msg);
            snprintf(primary_host, sizeof primary_host, "%s", host);
        }
        const bool info = (res != APP_FEED_ERR_HTTP) || strncmp(msg, "HTTP ", 5) == 0;
        if (idx != 0 && (!alt_err[0] || info || !alt_informative)) {
            snprintf(alt_err, sizeof alt_err, "%s", msg);
            snprintf(alt_host, sizeof alt_host, "%s", host);
            if (info) alt_informative = true;
        }
        ESP_LOGW(TAG, "候选[%d] %s 失败: %s", idx, host, msg);
    }

    if (err && err_cap) {
        char wifi_line[80];
        app_wifi_describe(wifi_line, sizeof wifi_line);
        if (alt_err[0] && strcmp(primary_host, alt_host) != 0) {
            snprintf(err, err_cap, "主源: %s\n(%s)\n备源: %s\n(%s)\n%s",
                     primary_err, primary_host, alt_err, alt_host, wifi_line);
        } else {
            snprintf(err, err_cap, "拉取: %s\n(%s)\n%s",
                     primary_err, primary_host, wifi_line);
        }
        ESP_LOGE(TAG, "全部源失败:\n%s", err);
    }
    return last_res;
}

size_t app_feed_last_bytes(void) { return s_last_bytes; }
