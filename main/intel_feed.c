// main/intel_feed.c —— 用 IDF 自带的 cJSON 组件（REQUIRES json）解析 005 推送。
//
// 同时兼容两种响应体：
//   1) 直接的 005 feed.json 原文（raw.githubusercontent.com / jsDelivr / GitHub raw accept）；
//   2) GitHub Contents API 默认 JSON 包装（{"encoding":"base64","content":"..."}），
//      即便网关或边缘节点忽略了 Accept: application/vnd.github.raw 也能自动解包。
#include "intel_feed.h"
#include "intel_logic.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FEED_MAX_FUTURE_SECONDS 600  // 源站时钟漂移容忍度，与 005 一致
#define B64_MAX_DECODED_BYTES   24576

static int b64_val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static char *decode_base64_alloc(const char *src, size_t *out_len)
{
    if (!src || !out_len) return NULL;
    const size_t slen = strlen(src);
    const size_t cap = (slen / 4) * 3 + 4;
    if (cap == 0 || cap > B64_MAX_DECODED_BYTES + 4) return NULL;

    char *out = malloc(cap);
    if (!out) return NULL;

    size_t olen = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < slen; i++) {
        const unsigned char c = (unsigned char)src[i];
        if (c == '=' || c == '\0') break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        const int v = b64_val(c);
        if (v < 0) {
            free(out);
            return NULL;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (olen >= B64_MAX_DECODED_BYTES) {
                free(out);
                return NULL;
            }
            out[olen++] = (char)((acc >> bits) & 0xFFu);
        }
    }
    out[olen] = 0;
    *out_len = olen;
    return out;
}

static void copy_str(char *dst, size_t cap, const cJSON *obj, const char *key, bool normalize)
{
    dst[0] = 0;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v) || !v->valuestring) return;
    if (normalize) intel_normalize(dst, cap, v->valuestring);
    else snprintf(dst, cap, "%s", v->valuestring);
}

static bool parse_quote(const cJSON *item, intel_item_t *out)
{
    const cJSON *sec = cJSON_GetObjectItemCaseSensitive(item, "security");
    if (!cJSON_IsObject(sec)) return false;

    copy_str(out->quote.code, sizeof out->quote.code, sec, "code", true);
    copy_str(out->quote.name, sizeof out->quote.name, sec, "name", true);

    const cJSON *price = cJSON_GetObjectItemCaseSensitive(sec, "price");
    const cJSON *change = cJSON_GetObjectItemCaseSensitive(sec, "change_pct");
    if (cJSON_IsNumber(price) && cJSON_IsNumber(change)) {
        out->quote.has_quote = true;
        out->quote.price = (float)price->valuedouble;
        out->quote.change_pct = (float)change->valuedouble;
    }
    return out->quote.code[0] || out->quote.name[0];
}

static void parse_tags(const cJSON *item, intel_item_t *out)
{
    out->tag_count = 0;
    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(item, "tags");
    if (!cJSON_IsArray(tags)) return;
    const cJSON *tag = NULL;
    cJSON_ArrayForEach(tag, tags) {
        if (out->tag_count >= INTEL_TAGS_MAX) break;
        if (!cJSON_IsString(tag) || !tag->valuestring) continue;
        intel_normalize(out->tags[out->tag_count], sizeof out->tags[0], tag->valuestring);
        if (out->tags[out->tag_count][0]) out->tag_count++;
    }
}

static intel_bias_t parse_bias(const cJSON *item)
{
    char s[12] = {0};
    copy_str(s, sizeof s, item, "bias", false);
    if (strcmp(s, "bull") == 0) return INTEL_BIAS_BULL;
    if (strcmp(s, "bear") == 0) return INTEL_BIAS_BEAR;
    if (strcmp(s, "neutral") == 0) return INTEL_BIAS_NEUTRAL;
    return INTEL_BIAS_NONE;
}

static bool parse_item(const cJSON *item, const intel_feed_t *feed, intel_item_t *out)
{
    if (!cJSON_IsObject(item)) return false;
    memset(out, 0, sizeof *out);

    copy_str(out->title, sizeof out->title, item, "title", true);
    if (!out->title[0]) return false;                       // 没标题，丢弃

    copy_str(out->id, sizeof out->id, item, "id", true);
    copy_str(out->one_line, sizeof out->one_line, item, "one_line", true);
    copy_str(out->source, sizeof out->source, item, "source_name", true);
    copy_str(out->url, sizeof out->url, item, "url", false);

    char time_raw[INTEL_TIME_BYTES] = {0};
    copy_str(time_raw, sizeof time_raw, item, "time", false);
    if (!time_raw[0]) return false;                         // 没时间，丢弃

    char grade[12] = {0};
    copy_str(grade, sizeof grade, item, "time_grade", false);
    out->grade = intel_grade_parse(grade);
    if (out->grade == INTEL_TIME_MISSING) return false;     // 不可信，丢弃

    int64_t epoch;
    if (!intel_iso8601_to_epoch(time_raw, &epoch)) return false;
    if (feed->generated_epoch > 0 && epoch > feed->generated_epoch + FEED_MAX_FUTURE_SECONDS) {
        return false;                                        // 来自未来的脏数据
    }
    snprintf(out->time, sizeof out->time, "%s", time_raw);
    intel_hhmm_from_epoch(epoch, out->hhmm);

    parse_tags(item, out);
    out->bias = parse_bias(item);
    parse_quote(item, out);
    return true;
}

static bool parse_feed_impl(const char *json, size_t len, intel_feed_t *out,
                            int *dropped, char *err, size_t err_cap, bool allow_unwrap)
{
    if (dropped) *dropped = 0;
    if (err && err_cap) err[0] = 0;
    if (!json || !out || len == 0) {
        if (err && err_cap) snprintf(err, err_cap, "empty payload");
        return false;
    }

    // 跳过 UTF-8 BOM 与前导空白，便于快速识别 HTML 拦截页或二进制载荷。
    size_t off = 0;
    if (len >= 3 && (unsigned char)json[0] == 0xEF &&
        (unsigned char)json[1] == 0xBB && (unsigned char)json[2] == 0xBF) {
        off = 3;
    }
    while (off < len && (json[off] == ' ' || json[off] == '\t' ||
                         json[off] == '\r' || json[off] == '\n')) {
        off++;
    }
    if (off >= len) {
        if (err && err_cap) snprintf(err, err_cap, "empty payload");
        return false;
    }
    if (json[off] == '<') {
        if (err && err_cap) snprintf(err, err_cap, "非JSON响应(HTML,%uB)", (unsigned)len);
        return false;
    }
    if (json[off] != '{') {
        if (err && err_cap) {
            snprintf(err, err_cap, "非JSON响应(0x%02X,%uB)",
                     (unsigned)(unsigned char)json[off], (unsigned)len);
        }
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(json + off, len - off);
    if (!root) {
        if (err && err_cap) {
            const char last = json[len - 1];
            if (last != '}' && last != '\n' && last != '\r' && last != ' ') {
                snprintf(err, err_cap, "JSON截断(%uB)", (unsigned)len);
            } else {
                snprintf(err, err_cap, "JSON解析失败(%uB)", (unsigned)len);
            }
        }
        return false;
    }

    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(items) && allow_unwrap) {
        const cJSON *enc = cJSON_GetObjectItemCaseSensitive(root, "encoding");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
        if (cJSON_IsString(enc) && enc->valuestring &&
            strcmp(enc->valuestring, "base64") == 0 &&
            cJSON_IsString(content) && content->valuestring) {
            size_t dlen = 0;
            char *decoded = decode_base64_alloc(content->valuestring, &dlen);
            // 先释放外层 JSON 树再解析内层，避免两棵 cJSON 树同时占用堆。
            cJSON_Delete(root);
            if (!decoded) {
                if (err && err_cap) snprintf(err, err_cap, "Base64解包失败");
                return false;
            }
            const bool ok = parse_feed_impl(decoded, dlen, out, dropped, err, err_cap, false);
            free(decoded);
            return ok;
        }
    }

    if (!cJSON_IsArray(items)) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(msg) && msg->valuestring && err && err_cap) {
            snprintf(err, err_cap, "API: %.48s", msg->valuestring);
        } else if (err && err_cap) {
            snprintf(err, err_cap, "missing items[]");
        }
        cJSON_Delete(root);
        return false;
    }

    memset(out, 0, sizeof *out);
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    out->schema_ok = cJSON_IsNumber(schema) && schema->valueint == 1;

    char source[INTEL_SOURCE_BYTES] = {0};
    copy_str(source, sizeof source, root, "source", true);
    snprintf(out->source, sizeof out->source, "%s", source[0] ? source : "005");

    char generated[INTEL_TIME_BYTES] = {0};
    copy_str(generated, sizeof generated, root, "generated_at", false);
    snprintf(out->generated_at, sizeof out->generated_at, "%s", generated);
    if (generated[0]) {
        intel_iso8601_to_epoch(generated, &out->generated_epoch);
        intel_hhmm_from_epoch(out->generated_epoch, out->generated_hhmm);
    }
    if (!out->generated_hhmm[0]) snprintf(out->generated_hhmm, sizeof out->generated_hhmm, "--:--");

    const cJSON *window = cJSON_GetObjectItemCaseSensitive(root, "window_minutes");
    out->window_minutes = cJSON_IsNumber(window) ? (uint16_t)window->valueint : 180;
    if (out->window_minutes == 0) out->window_minutes = 180;

    int drop = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, items) {
        if (out->count >= INTEL_MAX_ITEMS) { drop++; continue; }
        if (parse_item(it, out, &out->items[out->count])) {
            out->count++;
        } else {
            memset(&out->items[out->count], 0, sizeof out->items[0]);
            drop++;
        }
    }

    if (dropped) *dropped = drop;
    cJSON_Delete(root);

    if (out->count == 0) {
        if (err && err_cap) snprintf(err, err_cap, "no usable items (%d dropped)", drop);
        return false;
    }
    return true;
}

bool intel_feed_parse(const char *json, size_t len, intel_feed_t *out,
                      int *dropped, char *err, size_t err_cap)
{
    return parse_feed_impl(json, len, out, dropped, err, err_cap, true);
}
