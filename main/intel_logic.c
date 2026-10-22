// main/intel_logic.c —— 纯逻辑实现，无 LVGL / IDF 依赖（tests/ 里直接编译它）。
#include "intel_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --------------------------------------------------------------------------- //
// UTF-8
// --------------------------------------------------------------------------- //
uint32_t intel_utf8_next(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    if (*p == 0) return 0;

    uint32_t cp;
    int extra;
    if (p[0] < 0x80) { cp = p[0]; extra = 0; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; extra = 1; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; extra = 2; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; extra = 3; }
    else { *pp = (const char *)(p + 1); return '?'; }

    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp = (const char *)(p + 1); return '?'; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp = (const char *)(p + extra + 1);
    return cp;
}

int intel_cp_cells(uint32_t cp)
{
    if (cp < 0x80) return 1;                    // ASCII 半角
    if (cp >= 0x3000 && cp <= 0x303F) return 2; // CJK 标点
    if (cp >= 0xFF00 && cp <= 0xFFEF) return 2; // 全角
    if (cp >= 0x4E00 && cp <= 0x9FFF) return 2; // 汉字
    if (cp >= 0x2190 && cp <= 0x25FF) return 2; // 箭头/几何符号在等宽 CJK 字面里占一格全角
    if (cp >= 0x2000 && cp <= 0x206F) return 2;
    if (cp >= 0x2460 && cp <= 0x24FF) return 2; // ①②③
    return 2;
}

int intel_text_cells(const char *s)
{
    int cells = 0;
    while (s && *s) cells += intel_cp_cells(intel_utf8_next(&s));
    return cells;
}

int intel_utf8_len(const char *s) { return s ? (int)strlen(s) : 0; }

// --------------------------------------------------------------------------- //
// 时间
// --------------------------------------------------------------------------- //
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + (int64_t)doe - 719468LL;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int yy = (int)yoe + (int)(era * 400);
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp + (mp < 10 ? 3u : (unsigned)-9);
    *y = yy + (*m <= 2);
}

static int parse_uint(const char *s, int len)
{
    int v = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

bool intel_iso8601_to_epoch(const char *iso, int64_t *out)
{
    // 支持 2026-10-23T08:42:00(+08:00|Z|.123 后缀)
    if (!iso || strlen(iso) < 19) return false;
    if (iso[4] != '-' || iso[7] != '-' || (iso[10] != 'T' && iso[10] != ' ')) return false;
    if (iso[13] != ':' || iso[16] != ':') return false;

    const int y = parse_uint(iso, 4);
    const int mo = parse_uint(iso + 5, 2);
    const int d = parse_uint(iso + 8, 2);
    const int h = parse_uint(iso + 11, 2);
    const int mi = parse_uint(iso + 14, 2);
    const int se = parse_uint(iso + 17, 2);
    if (y < 1970 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60) return false;

    const char *p = iso + 19;
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }

    int offset_min = 0;
    if (*p == '+' || *p == '-') {
        const int oh = parse_uint(p + 1, 2);
        const int om = (p[3] == ':') ? parse_uint(p + 4, 2) : parse_uint(p + 3, 2);
        if (oh < 0 || om < 0) return false;
        offset_min = oh * 60 + om;
        if (*p == '-') offset_min = -offset_min;
    } else if (*p != 'Z' && *p != 0) {
        return false;
    }

    const int64_t secs = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400LL +
                         h * 3600LL + mi * 60LL + se - offset_min * 60LL;
    if (out) *out = secs;
    return true;
}

void intel_hhmm_from_epoch(int64_t epoch, char out[8])
{
    // 设备统一显示东八区（005 推送时区）。
    int64_t local = epoch + 8 * 3600;
    int64_t day = local / 86400;
    if (local < 0 && local % 86400) day--;
    int64_t sod = local - day * 86400;
    snprintf(out, 8, "%02d:%02d", (int)(sod / 3600), (int)((sod / 60) % 60));
}

void intel_mmdd_from_epoch(int64_t epoch, char out[8])
{
    int64_t local = epoch + 8 * 3600;
    int64_t day = local / 86400;
    if (local < 0 && local % 86400) day--;
    int y;
    unsigned m, d;
    civil_from_days(day, &y, &m, &d);
    snprintf(out, 8, "%02u-%02u", m, d);
}

void intel_hhmm_from_iso(const char *iso, char out[8])
{
    int64_t epoch;
    if (intel_iso8601_to_epoch(iso, &epoch)) {
        intel_hhmm_from_epoch(epoch, out);
        return;
    }
    snprintf(out, 8, "--:--");
}

intel_time_grade_t intel_grade_parse(const char *s)
{
    if (!s) return INTEL_TIME_MISSING;
    if (strcmp(s, "EXACT") == 0) return INTEL_TIME_EXACT;
    if (strcmp(s, "DATE") == 0) return INTEL_TIME_DATE;
    if (strcmp(s, "DERIVED") == 0) return INTEL_TIME_DERIVED;
    return INTEL_TIME_MISSING;
}

const char *intel_grade_label(intel_time_grade_t g)
{
    switch (g) {
    case INTEL_TIME_EXACT: return "EXACT";
    case INTEL_TIME_DATE: return "DATE";
    case INTEL_TIME_DERIVED: return "DERIVED";
    default: return "--";
    }
}

uint32_t intel_grade_color(intel_time_grade_t g)
{
    switch (g) {
    case INTEL_TIME_EXACT: return 0x5CFF9A;
    case INTEL_TIME_DATE: return 0xFFC24A;
    case INTEL_TIME_DERIVED: return 0xFF9A3C;
    default: return 0x1E9E52;
    }
}

uint32_t intel_bias_color(intel_bias_t b)
{
    switch (b) {
    case INTEL_BIAS_BULL: return 0xFF5A4A;
    case INTEL_BIAS_BEAR: return 0x58D8FF;
    case INTEL_BIAS_NEUTRAL: return 0xC9C9A8;
    default: return 0x1E9E52;
    }
}

uint32_t intel_change_color(bool has_quote, float change_pct)
{
    if (!has_quote) return 0x1E9E52;
    if (change_pct > 0.0f) return 0xFF5A4A;   // CGA 亮红 = 涨
    if (change_pct < 0.0f) return 0x58D8FF;   // 亮青 = 跌
    return 0xC9C9A8;
}

const char *intel_bias_label(intel_bias_t b)
{
    switch (b) {
    case INTEL_BIAS_BULL: return "偏多";
    case INTEL_BIAS_BEAR: return "偏空";
    case INTEL_BIAS_NEUTRAL: return "中性";
    default: return "--";
    }
}

int intel_age_minutes(int64_t then_epoch, int64_t now_epoch)
{
    if (now_epoch <= 0) return -1;
    const int64_t diff = now_epoch - then_epoch;
    if (diff < 0) return 0;
    return (int)(diff / 60);
}

void intel_age_text(int minutes, char out[16])
{
    if (minutes < 0) { snprintf(out, 16, "--"); return; }
    if (minutes < 1) { snprintf(out, 16, "刚刚"); return; }
    if (minutes < 60) { snprintf(out, 16, "%d分前", minutes); return; }
    if (minutes < 24 * 60) {
        snprintf(out, 16, "%d时%02d分", minutes / 60, minutes % 60);
        return;
    }
    snprintf(out, 16, "%d天前", minutes / (24 * 60));
}

// --------------------------------------------------------------------------- //
// 文本
// --------------------------------------------------------------------------- //
static uint32_t normalise_cp(uint32_t cp)
{
    switch (cp) {
    case 0x00A0: return ' ';
    case 0x2014: case 0x2015: case 0x2500: case 0x2501: return '-';
    case 0x2018: case 0x2019: case 0x2032: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x00B0: return 'o';
    case 0x2713: case 0x2714: return 'v';
    case 0x2715: case 0x2717: case 0x2718: return 'x';
    default: break;
    }
    if (cp >= 0x0370 && cp <= 0x03FF) return '?';  // 字体未收录希腊字母
    if (cp < 0x20) return 0;                        // 控制字符直接丢弃（0 = 删除）
    return cp;
}

static void put_utf8(char **dst, const char *end, uint32_t cp)
{
    char tmp[4];
    int n;
    if (cp < 0x80) { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*dst + n < end) {
        memcpy(*dst, tmp, (size_t)n);
        *dst += n;
        **dst = 0;
    }
}

void intel_normalize(char *dst, size_t cap, const char *src)
{
    if (!dst || cap < 2) return;
    dst[0] = 0;
    if (!src) return;

    char *d = dst;
    const char *end = dst + cap - 1;
    bool pending_space = false;

    while (*src) {
        const char *start = src;
        uint32_t cp = intel_utf8_next(&src);
        if (cp == 0) break;

        // 空白要在 normalise_cp() 之前判断：它会把 \t\n\r 当控制字符删除。
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000) {
            if (d != dst) pending_space = true;
            continue;
        }

        cp = normalise_cp(cp);
        if (cp == 0) continue;
        if (pending_space) {
            pending_space = false;
            if (d + 1 < end) { *d++ = ' '; *d = 0; }
        }
        if (cp == '?') {  // 原字节无法解码
            put_utf8(&d, end, '?');
            continue;
        }
        (void)start;
        put_utf8(&d, end, cp);
    }
}

static void line_ellipsis(char *line, int *cells, int max_cells)
{
    // 去掉尾部字符直到能放下一个 …（2 格），再加省略号。
    while (*cells > 0 && *cells + 2 > max_cells) {
        // 去掉最后一个 UTF-8 字符
        size_t len = strlen(line);
        if (len == 0) break;
        size_t i = len;
        while (i > 0 && ((unsigned char)line[i - 1] & 0xC0) == 0x80) i--;
        if (i > 0) i--;
        line[i] = 0;
        const char *p = line + i;
        (void)p;
        *cells = intel_text_cells(line);
    }
    if (*cells + 2 <= max_cells) {
        strncat(line, "…", INTEL_LINE_BYTES - strlen(line) - 1);
        *cells += 2;
    }
}

int intel_wrap(const char *text, int max_cells, int max_lines,
               char out[][INTEL_LINE_BYTES], int cap)
{
    if (!text || !*text || max_lines <= 0 || cap <= 0) return 0;
    if (max_lines > cap) max_lines = cap;

    int line = 0;
    size_t used = 0;
    int cells = 0;
    bool truncated = false;
    out[0][0] = 0;

    const char *p = text;
    while (*p) {
        const char *start = p;
        const uint32_t cp = intel_utf8_next(&p);
        const int w = intel_cp_cells(cp);

        if (cells + w > max_cells && used > 0) {
            if (line + 1 >= max_lines) { truncated = true; break; }
            line++;
            used = 0;
            cells = 0;
            out[line][0] = 0;
            if (cp == ' ') continue;
        }

        const size_t n = (size_t)(p - start);
        if (used + n + 4 >= INTEL_LINE_BYTES) { truncated = true; break; }
        memcpy(&out[line][used], start, n);
        used += n;
        cells += w;
        out[line][used] = 0;
    }

    int count = line + 1;
    if (count > 0 && out[count - 1][0] == 0) count--;   // 末行空行不计
    if (truncated && count > 0) line_ellipsis(out[count - 1], &cells, max_cells);
    return count < 0 ? 0 : count;
}

// --------------------------------------------------------------------------- //
// 详情页
// --------------------------------------------------------------------------- //
typedef struct {
    intel_line_t *out;
    int cap;
    int n;
} builder_t;

static void push(builder_t *b, intel_line_style_t style, const char *text)
{
    if (b->n >= b->cap) return;
    snprintf(b->out[b->n].text, INTEL_LINE_BYTES, "%s", text ? text : "");
    b->out[b->n].style = (uint8_t)style;
    b->n++;
}

static void push_rule(builder_t *b)
{
    if (b->n >= b->cap) return;
    memset(b->out[b->n].text, 0, INTEL_LINE_BYTES);
    for (int i = 0; i < INTEL_DETAIL_CELLS && i < INTEL_LINE_BYTES - 1; i++) {
        b->out[b->n].text[i] = '-';
    }
    b->out[b->n].style = INTEL_LINE_RULE;
    b->n++;
}

static void push_wrapped(builder_t *b, intel_line_style_t style, const char *text, int max_lines)
{
    char lines[8][INTEL_LINE_BYTES];
    int n = intel_wrap(text, INTEL_DETAIL_CELLS, max_lines, lines, 8);
    for (int i = 0; i < n; i++) push(b, style, lines[i]);
}

int intel_detail_build(const intel_item_t *item, int64_t now_epoch,
                       intel_line_t *out, int cap)
{
    if (!item || !out || cap <= 0) return 0;
    builder_t b = { .out = out, .cap = cap, .n = 0 };
    char buf[INTEL_LINE_BYTES];

    if (item->one_line[0]) {
        push(&b, INTEL_LINE_HEAD, "AI 一句话");
        push_wrapped(&b, INTEL_LINE_ACCENT, item->one_line, 5);
        push_rule(&b);
    }

    if (item->quote.has_quote || item->quote.name[0] || item->quote.code[0]) {
        const char *name = item->quote.name[0] ? item->quote.name : "--";
        if (item->quote.code[0]) snprintf(buf, sizeof buf, "%s %s", item->quote.code, name);
        else snprintf(buf, sizeof buf, "%s", name);
        push(&b, INTEL_LINE_PLAIN, buf);

        if (item->quote.has_quote) {
            char pct[16];
            snprintf(pct, sizeof pct, "%+.2f%%", (double)item->quote.change_pct);
            snprintf(buf, sizeof buf, "%.2f  %s", (double)item->quote.price, pct);
            push(&b, item->quote.change_pct >= 0 ? INTEL_LINE_BULL : INTEL_LINE_BEAR, buf);
        } else {
            push(&b, INTEL_LINE_DIM, "暂无实时行情");
        }
        push_rule(&b);
    }

    char mmdd[8];
    int64_t epoch;
    if (intel_iso8601_to_epoch(item->time, &epoch)) {
        intel_mmdd_from_epoch(epoch, mmdd);
        int age = intel_age_minutes(epoch, now_epoch);
        char age_txt[16];
        intel_age_text(age, age_txt);
        snprintf(buf, sizeof buf, "%s %s %s · %s", mmdd, item->hhmm,
                 intel_grade_label(item->grade), age_txt);
    } else {
        snprintf(buf, sizeof buf, "%s %s", item->hhmm, intel_grade_label(item->grade));
    }
    push(&b, INTEL_LINE_PLAIN, buf);

    snprintf(buf, sizeof buf, "来源 %s", item->source[0] ? item->source : "--");
    push(&b, INTEL_LINE_PLAIN, buf);

    if (item->tag_count > 0) {
        char tags[INTEL_TAGS_MAX * INTEL_TAG_BYTES + 8] = { 0 };
        for (int i = 0; i < item->tag_count; i++) {
            if (i) strncat(tags, " / ", sizeof tags - strlen(tags) - 1);
            strncat(tags, item->tags[i], sizeof tags - strlen(tags) - 1);
        }
        char line[INTEL_LINE_BYTES];
        snprintf(line, sizeof line, "标签 %.64s", tags);
        push(&b, INTEL_LINE_PLAIN, line);
    }

    snprintf(buf, sizeof buf, "倾向 %s", intel_bias_label(item->bias));
    push(&b, item->bias == INTEL_BIAS_NONE ? INTEL_LINE_DIM : INTEL_LINE_PLAIN, buf);

    if (item->url[0]) {
        push_rule(&b);
        push(&b, INTEL_LINE_HEAD, "原文");
        push_wrapped(&b, INTEL_LINE_DIM, item->url, 3);
    }

    return b.n;
}

// --------------------------------------------------------------------------- //
// 选择
// --------------------------------------------------------------------------- //
int intel_clamp_index(int index, int count)
{
    if (count <= 0) return 0;
    if (index < 0) return 0;
    if (index >= count) return count - 1;
    return index;
}

int intel_next_index(int index, int count, int delta)
{
    if (count <= 0) return 0;
    int next = index + delta;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    return next;
}
