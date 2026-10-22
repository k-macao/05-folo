// main/ui_crt.c —— DOS/CRT 终端风格：黑底、磷光绿、反白选中、等宽网格。
//
// FoloToy AI Passport 的屏幕带 R=30 圆角遮罩与顶部塑料压边（实机顶边约遮 6 px）：
// 顶栏（y=7..21）和底栏（y=299..313）左右各留 18 px 安全边距，确保四角不切字。
// 所有函数均在 bsp_lvgl_lock() 保护下串行执行，换行临时缓冲放静态区，避免占用调用任务栈。
#include "ui_crt.h"
#include "intel_logic.h"
#include "fonts/crt_font_12.h"

#include <stdio.h>
#include <string.h>

#define SCR_W         240
#define SCR_H         320
#define CORNER_X      18    // 顶栏/底栏避开 R=30 圆角的水平安全边距
#define BODY_X        6     // 中段直边区域左右各留 6 px（38 半格 = 228 px）
#define STATUS_Y      7     // 避开实机上沿塑料边框遮挡
#define STATUS_H      24    // 顶栏分割线在 y=23
#define FOOTER_RULE   297   // 底栏分割线
#define FOOTER_Y      299   // 底栏文字 y=299..313，避开下沿与底角圆弧
#define DETAIL_HEAD_Y 40    // 详情标题条 24..38（15 px），正文从 y=40 起 17 行 × 15 px = 295

static lv_obj_t *s_scr;
static char s_seg_buf[INTEL_LINE_BYTES];
static char s_wrap_buf[4][INTEL_LINE_BYTES];

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, int x, int y, const char *s, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, s ? s : "");
    lv_obj_set_style_text_font(label, &crt_font_12, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

static void hline(lv_obj_t *parent, int y, uint32_t color, int x, int w)
{
    box(parent, x, y, w, 1, color);
}

static void status_bar(const ui_view_t *v)
{
    char left[48];
    const char *banner = v->banner;
    if (v->busy) banner = "SYNC..";
    else if (v->stale) banner = "STALE";
    snprintf(left, sizeof left, "|%s", banner ? banner : "SIGNAL.LOG");
    text(s_scr, CORNER_X, STATUS_Y, left, v->stale ? UI_AMBER : UI_FG);

    char right[20];
    if (v->battery_pct >= 0) {
        snprintf(right, sizeof right, "%s %d%%", v->clock ? v->clock : "--:--", v->battery_pct);
    } else {
        snprintf(right, sizeof right, "%s", v->clock ? v->clock : "--:--");
    }
    lv_obj_t *r = text(s_scr, 0, STATUS_Y, right, UI_DIM);
    lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -CORNER_X, STATUS_Y);
    hline(s_scr, STATUS_H - 1, UI_DIM, 0, SCR_W);
}

static void footer(const ui_view_t *v, const char *fallback)
{
    hline(s_scr, FOOTER_RULE, UI_DIM, 0, SCR_W);
    text(s_scr, CORNER_X, FOOTER_Y, v->hint ? v->hint : fallback, UI_DIM);
}

// --------------------------------------------------------------------------- //
// 列表
// --------------------------------------------------------------------------- //
static void render_list(const ui_view_t *v)
{
    const intel_feed_t *feed = v->feed;
    const int count = feed ? feed->count : 0;

    int first = 0;
    if (v->selected >= UI_VISIBLE_ROWS) {
        first = v->selected - UI_VISIBLE_ROWS + 1;
    }
    if (count > UI_VISIBLE_ROWS && first > count - UI_VISIBLE_ROWS) {
        first = count - UI_VISIBLE_ROWS;
    }

    // 6 行 × 45 px（y=24..294）；选中行反白，超过 6 条时随光标滑动窗口。
    for (int row = 0; row < UI_VISIBLE_ROWS; row++) {
        const int idx = first + row;
        if (idx >= count) break;

        const intel_item_t *it = &feed->items[idx];
        const bool sel = idx == v->selected;
        const int y = STATUS_H + row * UI_ROW_H;

        lv_obj_t *card = box(s_scr, 0, y, SCR_W, UI_ROW_H - 1,
                             sel ? UI_BAR : UI_BG);
        const uint32_t main_color = sel ? UI_INK : UI_WHITE;

        char meta[96];
        snprintf(meta, sizeof meta, "[%s] %s %s", intel_grade_label(it->grade),
                 it->hhmm, it->source);
        text(card, BODY_X, 1, meta, sel ? UI_INK : (uint32_t)intel_grade_color(it->grade));

        // 标题两行，38 半格/行（复用静态 s_wrap_buf，零大栈帧）
        int n = intel_wrap(it->title, INTEL_LIST_CELLS, 2, s_wrap_buf, 2);
        for (int i = 0; i < n; i++) {
            text(card, BODY_X, 15 + i * 14, s_wrap_buf[i], main_color);
        }
        if (n == 0) text(card, BODY_X, 15, "(无标题)", main_color);

        // 涨跌：右上角一个小标记，选中时也保持可辨
        if (it->quote.has_quote) {
            char pct[16];
            snprintf(pct, sizeof pct, "%+.2f%%", (double)it->quote.change_pct);
            lv_obj_t *p = text(card, 0, 1, pct,
                               sel ? UI_INK : intel_change_color(true, it->quote.change_pct));
            lv_obj_align(p, LV_ALIGN_TOP_RIGHT, -BODY_X, 1);
        }

        if (row < UI_VISIBLE_ROWS - 1) {
            hline(s_scr, y + UI_ROW_H - 1, sel ? UI_BAR : UI_DIM, 0, SCR_W);
        }
    }

    char hint[64];
    snprintf(hint, sizeof hint, "▲▼ 选择  OK 详情  %d/%d", count ? v->selected + 1 : 0, count);
    footer(v, hint);
}

// --------------------------------------------------------------------------- //
// 详情
// --------------------------------------------------------------------------- //
static uint32_t style_color(uint8_t style)
{
    switch (style) {
    case INTEL_LINE_HEAD: return UI_FG;
    case INTEL_LINE_ACCENT: return UI_WHITE;
    case INTEL_LINE_DIM: return UI_DIM;
    case INTEL_LINE_BULL: return UI_RED;
    case INTEL_LINE_BEAR: return UI_CYAN;
    default: return UI_FG;
    }
}

static void render_detail(const ui_view_t *v)
{
    char head[64];
    snprintf(head, sizeof head, "%d/%d ", v->detail_index + 1, v->feed->count);
    lv_obj_t *bar = box(s_scr, 0, STATUS_H, SCR_W, 15, UI_BAR);
    text(bar, BODY_X, 0, head, UI_INK);

    char src[24];
    snprintf(src, sizeof src, "%.12s", v->feed->items[v->detail_index].source);
    text(bar, (int)strlen(head) * 6 + BODY_X, 0, src[0] ? src : "--", UI_INK);

    for (int i = 0; i < UI_DETAIL_ROWS; i++) {
        const int line_index = v->detail_first_line + i;
        if (line_index >= v->detail_line_count) break;
        const intel_line_t *line = &v->detail_lines[line_index];
        const int y = DETAIL_HEAD_Y + i * 15;

        if (line->style == INTEL_LINE_RULE) {
            hline(s_scr, y + 7, UI_DIM, BODY_X, SCR_W - 2 * BODY_X);
        } else if (line->style == INTEL_LINE_HEAD) {
            lv_obj_t *tag = box(s_scr, BODY_X, y + 1,
                                intel_text_cells(line->text) * 6 + 8, 13, UI_BAR);
            text(tag, 4, 0, line->text, UI_INK);
        } else {
            text(s_scr, BODY_X, y, line->text, style_color(line->style));
        }
    }

    if (v->detail_line_count > v->detail_first_line + UI_DETAIL_ROWS) {
        text(s_scr, SCR_W - 18, DETAIL_HEAD_Y + (UI_DETAIL_ROWS - 1) * 15, "▼", UI_AMBER);
    }

    footer(v, "▲▼ 滚动  OK 翻页  长按 返回");
}

// --------------------------------------------------------------------------- //
// 启动 / 错误
// --------------------------------------------------------------------------- //
static void render_message(const ui_view_t *v)
{
    lv_obj_t *title = text(s_scr, 8, 40, v->headline ? v->headline : "SIGNAL.LOG", UI_FG);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    hline(s_scr, 68, UI_DIM, 8, SCR_W - 16);

    // 按 \n 分段后直接逐行输出 label，不在栈上分配多行大数组。
    const char *para = v->message ? v->message : "";
    int row = 0;
    while (*para && row < 12) {
        const char *nl = strchr(para, '\n');
        const size_t len = nl ? (size_t)(nl - para) : strlen(para);
        if (len > 0) {
            const size_t keep = len < sizeof s_seg_buf - 1 ? len : sizeof s_seg_buf - 1;
            memcpy(s_seg_buf, para, keep);
            s_seg_buf[keep] = 0;
            const int w = intel_wrap(s_seg_buf, INTEL_DETAIL_CELLS, 4, s_wrap_buf, 4);
            for (int i = 0; i < w && row < 12; i++) {
                const bool dim = strncmp(s_wrap_buf[i], "Wi-Fi:", 6) == 0 ||
                                 strncmp(s_wrap_buf[i], "提示:", 7) == 0;
                text(s_scr, 8, 78 + row * 15, s_wrap_buf[i], dim ? UI_DIM : UI_FG);
                row++;
            }
        } else {
            row++;                      // 空行保留，用于段落间距
        }
        if (!nl) break;
        para = nl + 1;
    }

    footer(v, "OK 重试");
}

// --------------------------------------------------------------------------- //
void ui_crt_render(const ui_view_t *v)
{
    if (!v) return;

    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);

    status_bar(v);

    switch (v->kind) {
    case UI_VIEW_LIST:
        render_list(v);
        break;
    case UI_VIEW_DETAIL:
        if (v->feed && v->detail_index >= 0 && v->detail_index < v->feed->count &&
            v->detail_lines && v->detail_line_count > 0) {
            render_detail(v);
        } else {
            render_message(v);
        }
        break;
    case UI_VIEW_BOOT:
    case UI_VIEW_ERROR:
    default:
        render_message(v);
        break;
    }

    // 原子切换活动屏幕并自动销毁旧屏（含首次开机 LVGL 默认白底屏），避免空屏闪烁和池泄漏。
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

void ui_crt_deinit(void)
{
    s_scr = NULL;
}
