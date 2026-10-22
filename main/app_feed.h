// main/app_feed.h —— 拉取 005 的 feed.json（HTTPS），交给 intel_feed_parse。
#pragma once

#include "intel.h"

#include <stddef.h>

typedef enum {
    APP_FEED_OK = 0,
    APP_FEED_ERR_WIFI,
    APP_FEED_ERR_HTTP,
    APP_FEED_ERR_PARSE,
    APP_FEED_ERR_NO_MEM,
} app_feed_result_t;

// 阻塞式下载 + 解析，必须在工作线程里调用。
// 成功时 *out_parsed 指向一块新分配的 intel_feed_t（调用方复制后 free），
// 这样在 TLS 握手期无需常驻 11.3 KB 静态二级缓冲。
app_feed_result_t app_feed_fetch(intel_feed_t **out_parsed, int *dropped,
                                 char *err, size_t err_cap);

// 最近一次下载的字节数，仅用于日志/UI 诊断。
size_t app_feed_last_bytes(void);
