// main/ui_crt.h —— 005 情报在 240x320 上的 DOS/CRT 终端界面。
//
// 一次渲染 = 删掉旧屏、按 view 重建。三按键设备上一秒最多几次按键，重建几十个
// label 的成本可以忽略；换来的是没有"谁负责恢复高亮"这类状态 bug。
#pragma once

#include "intel.h"
#include "lvgl.h"

#include <stdbool.h>

#define UI_BG        0x000000
#define UI_FG        0x3BFF7C   // 磷光绿
#define UI_DIM       0x1E9E52   // 暗绿：次要信息
#define UI_BAR       0x00FF66   // 反白底
#define UI_INK       0x001400   // 反白底上的字
#define UI_AMBER     0xFFC24A
#define UI_RED       0xFF5A4A
#define UI_CYAN      0x58D8FF
#define UI_WHITE     0xE8FFE8

#define UI_ROW_H      46
#define UI_VISIBLE_ROWS 6
#define UI_DETAIL_ROWS 17

typedef enum {
    UI_VIEW_BOOT = 0,
    UI_VIEW_LIST,
    UI_VIEW_DETAIL,
    UI_VIEW_ERROR,
} ui_view_kind_t;

typedef struct {
    ui_view_kind_t kind;
    const intel_feed_t *feed;      // LIST/DETAIL 用；BOOT/ERROR 可为 NULL
    int selected;                  // LIST：选中项
    int detail_index;              // DETAIL：第几条
    int detail_first_line;         // DETAIL：从第几行开始显示
    int detail_line_count;         // DETAIL：总行数
    const intel_line_t *detail_lines;

    const char *clock;             // 顶栏右侧 "09:12"，NULL 显示 "--:--"
    int battery_pct;               // -1 表示读不到
    const char *banner;            // 顶栏左侧，NULL 显示 "SIGNAL.LOG"
    bool stale;                    // 上次同步失败：顶栏显示 "STALE"
    bool busy;                     // 正在拉取：顶栏显示 "SYNC.."

    const char *headline;          // BOOT/ERROR 主标题（ASCII，用 Montserrat 20）
    const char *message;           // BOOT/ERROR 正文，可含 \n，最长 8 行
    const char *hint;              // 底部提示；NULL 用该视图的默认提示
} ui_view_t;

// 调用者必须持有 bsp_lvgl_lock()。
void ui_crt_render(const ui_view_t *view);

// 释放界面（深睡/重启前用）。调用者持锁。
void ui_crt_deinit(void);
