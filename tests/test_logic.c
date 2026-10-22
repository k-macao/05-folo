// tests/test_logic.c —— intel_logic 的主机测试（gcc，无 IDF / 无 LVGL）。
#include "intel_logic.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

#define CHECK_STR(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); failures++; \
    } \
} while (0)

static void test_time(void)
{
    int64_t epoch = 0;
    CHECK(intel_iso8601_to_epoch("2026-10-23T08:42:00+08:00", &epoch));
    CHECK(epoch == 1792716120LL);

    CHECK(intel_iso8601_to_epoch("2026-10-23T00:42:00Z", &epoch));
    CHECK(epoch == 1792716120LL);   // 00:42Z == 08:42+08:00

    CHECK(!intel_iso8601_to_epoch("2026-13-23T08:42:00+08:00", &epoch));
    CHECK(!intel_iso8601_to_epoch("", &epoch));
    CHECK(!intel_iso8601_to_epoch("2026-10-23", &epoch));

    char hhmm[8];
    intel_hhmm_from_epoch(1792716120LL, hhmm);
    CHECK_STR(hhmm, "08:42");

    char mmdd[8];
    intel_mmdd_from_epoch(1792716120LL, mmdd);
    CHECK_STR(mmdd, "10-23");

    intel_hhmm_from_iso("not-a-date", hhmm);
    CHECK_STR(hhmm, "--:--");

    CHECK(intel_grade_parse("EXACT") == INTEL_TIME_EXACT);
    CHECK(intel_grade_parse("DERIVED") == INTEL_TIME_DERIVED);
    CHECK(intel_grade_parse("garbage") == INTEL_TIME_MISSING);
    CHECK_STR(intel_grade_label(INTEL_TIME_DATE), "DATE");

    CHECK(intel_age_minutes(1792716120LL, 1792716120LL + 3 * 3600) == 180);
    CHECK(intel_age_minutes(1792716120LL, 0) == -1);

    char txt[16];
    intel_age_text(0, txt);   CHECK_STR(txt, "刚刚");
    intel_age_text(7, txt);   CHECK_STR(txt, "7分前");
    intel_age_text(125, txt); CHECK_STR(txt, "2时05分");
    intel_age_text(3 * 24 * 60, txt); CHECK_STR(txt, "3天前");

    CHECK(intel_change_color(true, 1.5f) == 0xFF5A4A);   // 涨=亮红
    CHECK(intel_change_color(true, -1.5f) == 0x58D8FF);  // 跌=亮青
    CHECK(intel_change_color(false, 9.9f) == 0x1E9E52);
}

static void test_widths(void)
{
    CHECK(intel_text_cells("AB") == 2);
    CHECK(intel_text_cells("稀土") == 4);
    CHECK(intel_text_cells("A股") == 3);
    CHECK(intel_text_cells("—") == 2);
    CHECK(intel_text_cells("") == 0);
}

static void test_normalize(void)
{
    char out[128];
    intel_normalize(out, sizeof out, "  国常会：   加快\t培育\n人形机器人  ");
    CHECK_STR(out, "国常会： 加快 培育 人形机器人");

    intel_normalize(out, sizeof out, "A—B°C✓");
    CHECK_STR(out, "A-BoCv");

    intel_normalize(out, sizeof out, "\x01""ok");
    CHECK_STR(out, "ok");

    intel_normalize(out, sizeof out, "");
    CHECK_STR(out, "");

    // 截断不能越界，也不能留下半个 UTF-8 字符
    char small[8];
    intel_normalize(small, sizeof small, "一二三四五六七八");
    CHECK(strlen(small) < sizeof small);
}

static void test_wrap(void)
{
    char lines[8][INTEL_LINE_BYTES];

    int n = intel_wrap("国常会：加快培育人形机器人产业", 10, 4, lines, 8);
    CHECK(n >= 2);
    for (int i = 0; i < n; i++) CHECK(intel_text_cells(lines[i]) <= 10);
    CHECK_STR(lines[0], "国常会：加");   // 4 个全角 = 8 格，第 5 个全角正好 10 格

    // 超出行数时最后一行带省略号
    n = intel_wrap("一二三四五六七八九十一二三四五六七八九十", 8, 2, lines, 8);
    CHECK(n == 2);
    CHECK(strstr(lines[1], "…") != NULL);

    // 正好填满时不加省略号
    n = intel_wrap("一二三四", 8, 2, lines, 8);
    CHECK(n == 1);
    CHECK(strstr(lines[0], "…") == NULL);

    // ASCII 按半格
    n = intel_wrap("300750 宁德时代 +2.31%", 38, 2, lines, 8);
    CHECK(n == 1);
    CHECK_STR(lines[0], "300750 宁德时代 +2.31%");

    n = intel_wrap("", 10, 2, lines, 8);
    CHECK(n == 0);

    n = intel_wrap("多空", 1, 2, lines, 8);   // 单字宽于行宽时不崩溃
    CHECK(n >= 1);
}

static void test_detail(void)
{
    intel_item_t item;
    memset(&item, 0, sizeof item);
    snprintf(item.title, sizeof item.title, "国常会：加快培育人形机器人产业");
    snprintf(item.one_line, sizeof item.one_line, "政策定调，上游减速器与丝杠确定性提升");
    snprintf(item.source, sizeof item.source, "东方财富");
    snprintf(item.time, sizeof item.time, "2026-10-23T08:42:00+08:00");
    snprintf(item.hhmm, sizeof item.hhmm, "08:42");
    item.grade = INTEL_TIME_EXACT;
    item.bias = INTEL_BIAS_BULL;
    snprintf(item.quote.code, sizeof item.quote.code, "300750");
    snprintf(item.quote.name, sizeof item.quote.name, "宁德时代");
    item.quote.has_quote = true;
    item.quote.price = 231.5f;
    item.quote.change_pct = 2.31f;
    snprintf(item.tags[0], INTEL_TAG_BYTES, "政策");
    snprintf(item.tags[1], INTEL_TAG_BYTES, "机器人");
    item.tag_count = 2;
    snprintf(item.url, sizeof item.url, "https://example.com/a");

    intel_line_t lines[INTEL_DETAIL_MAX_LINES];
    const int n = intel_detail_build(&item, 1792716120LL + 3600, lines, INTEL_DETAIL_MAX_LINES);
    CHECK(n > 6);
    CHECK(n <= INTEL_DETAIL_MAX_LINES);

    bool has_head = false, has_pct = false, has_source = false;
    for (int i = 0; i < n; i++) {
        if (lines[i].style == INTEL_LINE_HEAD && strcmp(lines[i].text, "AI 一句话") == 0) has_head = true;
        if (strstr(lines[i].text, "+2.31%")) has_pct = true;
        if (strstr(lines[i].text, "东方财富")) has_source = true;
    }
    CHECK(has_head);
    CHECK(has_pct);
    CHECK(has_source);

    for (int i = 0; i < n; i++) {
        if (lines[i].style == INTEL_LINE_RULE) continue;
        CHECK(intel_text_cells(lines[i].text) <= INTEL_DETAIL_CELLS + 2);
    }
}

static void test_selection(void)
{
    CHECK(intel_next_index(0, 5, -1) == 4);
    CHECK(intel_next_index(4, 5, 1) == 0);
    CHECK(intel_clamp_index(9, 3) == 2);
    CHECK(intel_clamp_index(-1, 3) == 0);
    CHECK(intel_clamp_index(0, 0) == 0);
}

int main(void)
{
    test_time();
    test_widths();
    test_normalize();
    test_wrap();
    test_detail();
    test_selection();

    if (failures) {
        printf("test_logic: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_logic: all checks passed\n");
    return 0;
}
