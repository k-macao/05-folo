// main/intel_logic.h —— 纯逻辑：不碰 LVGL / ESP-IDF，可在主机上单测。
//
// 字宽约定（与 main/fonts/crt_font_12.c 的生成断言一致）：
//   ASCII 半角 = 6 px = 1 格；汉字/全角标点 = 12 px = 2 格。
// 所有排版都按"格"计算，因此 UI 与网页预览可以用同一套换行规则。
#pragma once

#include "intel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INTEL_HALF_CELLS 6
#define INTEL_LIST_CELLS 38   // 240 - 2*4 边距 = 232 px
#define INTEL_DETAIL_CELLS 36 // 详情页左侧留 2 格给色条

// ---- UTF-8 ----
uint32_t intel_utf8_next(const char **pp);        // 非法字节返回 '?' 并前进 1 字节
int intel_cp_cells(uint32_t cp);                  // 1 或 2
int intel_text_cells(const char *s);
int intel_utf8_len(const char *s);                // 字节数，给拷贝用

// ---- 时间 ----
bool intel_iso8601_to_epoch(const char *iso, int64_t *out);
void intel_hhmm_from_iso(const char *iso, char out[8]);
void intel_hhmm_from_epoch(int64_t epoch, char out[8]);
void intel_mmdd_from_epoch(int64_t epoch, char out[8]);
intel_time_grade_t intel_grade_parse(const char *s);
const char *intel_grade_label(intel_time_grade_t g);
uint32_t intel_grade_color(intel_time_grade_t g);
uint32_t intel_bias_color(intel_bias_t b);
uint32_t intel_change_color(bool has_quote, float change_pct);
const char *intel_bias_label(intel_bias_t b);
int intel_age_minutes(int64_t then_epoch, int64_t now_epoch);
void intel_age_text(int minutes, char out[16]);

// ---- 文本 ----
// 折叠空白、去掉控制字符、替换字体里没有的字符（— ° ✓ ′ 等），
// 这样任何来源的字符串都不会在屏幕上变成豆腐块。
void intel_normalize(char *dst, size_t cap, const char *src);

// 按格换行，最多 max_lines 行；被截断时最后一行以 … 结尾。返回实际行数。
int intel_wrap(const char *text, int max_cells, int max_lines,
               char out[][INTEL_LINE_BYTES], int cap);

// ---- 详情页 ----
int intel_detail_build(const intel_item_t *item, int64_t now_epoch,
                       intel_line_t *out, int cap);

// ---- 列表 / 选择 ----
int intel_clamp_index(int index, int count);
int intel_next_index(int index, int count, int delta);   // 到头后循环
