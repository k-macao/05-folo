// main/intel.h —— 005「章鱼AI · 事件雷达」推送内容在设备上的内存模型。
//
// 字段与 docs/feed-format.md 一一对应；解析器只做收窄，不做业务判断，
// 需要判断的东西（时间是否可信、是否过期）都在 intel_logic.c 里，便于主机测试。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define INTEL_MAX_ITEMS      12   // 与 005 单轮推送条数上限一致，防止小屏被刷屏
#define INTEL_TAGS_MAX        3
#define INTEL_ID_BYTES       24
#define INTEL_TITLE_BYTES   224   // 72 个汉字
#define INTEL_LINE_BYTES    144   // 24 个汉字，够 38 半格的一行
#define INTEL_SOURCE_BYTES   32
#define INTEL_TIME_BYTES     28
#define INTEL_URL_BYTES     192
#define INTEL_NAME_BYTES     32
#define INTEL_CODE_BYTES     12
#define INTEL_TAG_BYTES      24
#define INTEL_DETAIL_MAX_LINES 48

// 时间可信度，直接沿用 005 的分级。MISSING 在解析阶段就被丢弃（宁可丢弃，不可臆造）。
typedef enum {
    INTEL_TIME_EXACT = 0,
    INTEL_TIME_DATE,
    INTEL_TIME_DERIVED,
    INTEL_TIME_MISSING,
} intel_time_grade_t;

typedef enum {
    INTEL_BIAS_NONE = 0,
    INTEL_BIAS_BULL,
    INTEL_BIAS_BEAR,
    INTEL_BIAS_NEUTRAL,
} intel_bias_t;

typedef struct {
    char code[INTEL_CODE_BYTES];   // "300750"，可能为空
    char name[INTEL_NAME_BYTES];   // "宁德时代"
    bool has_quote;                // 识别到标的且有行情才算有
    float price;
    float change_pct;
} intel_quote_t;

typedef struct {
    char id[INTEL_ID_BYTES];
    char title[INTEL_TITLE_BYTES];
    char one_line[INTEL_LINE_BYTES * 2];
    char source[INTEL_SOURCE_BYTES];
    char time[INTEL_TIME_BYTES];   // 原样 ISO8601，详情页展示用
    char hhmm[8];                  // "08:42"
    intel_time_grade_t grade;
    char tags[INTEL_TAGS_MAX][INTEL_TAG_BYTES];
    uint8_t tag_count;
    intel_bias_t bias;
    intel_quote_t quote;
    char url[INTEL_URL_BYTES];
} intel_item_t;

typedef struct {
    char schema_ok;                      // 解析时校验过 schema == 1
    char source[INTEL_SOURCE_BYTES];
    char generated_at[INTEL_TIME_BYTES];
    char generated_hhmm[8];
    int64_t generated_epoch;
    uint16_t window_minutes;
    int count;
    intel_item_t items[INTEL_MAX_ITEMS];
} intel_feed_t;

// 详情页一行。style 决定颜色，由 ui_crt.c 翻译成实际色值。
typedef enum {
    INTEL_LINE_HEAD = 0,   // 反白小标题，如 "AI 一句话"
    INTEL_LINE_PLAIN,
    INTEL_LINE_DIM,        // 次要信息：来源、URL
    INTEL_LINE_ACCENT,     // 重点：一句话结论
    INTEL_LINE_BULL,       // 涨
    INTEL_LINE_BEAR,       // 跌
    INTEL_LINE_RULE,       // 分隔线（text 为 "-" 的重复，由 UI 画成线条）
} intel_line_style_t;

typedef struct {
    char text[INTEL_LINE_BYTES];
    uint8_t style;
} intel_line_t;
