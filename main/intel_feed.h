// main/intel_feed.h —— 005 feed JSON -> intel_feed_t
#pragma once

#include "intel.h"

#include <stdbool.h>
#include <stddef.h>

// 解析 docs/feed-format.md 定义的单轮推送。任何一条不合格的内容都会被丢弃并计入
// dropped，而不是带着可疑数据上屏（沿用 005 "宁可丢弃，不可臆造" 的规则）。
// err 可传 NULL；失败时写入可读原因。
bool intel_feed_parse(const char *json, size_t len, intel_feed_t *out,
                      int *dropped, char *err, size_t err_cap);
