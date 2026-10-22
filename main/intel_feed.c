// main/intel_feed.c —— 用 IDF 自带的 cJSON 组件（REQUIRES json）解析 005 推送。
#include "intel_feed.h"
#include "intel_logic.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

#define FEED_MAX_FUTURE_SECONDS 600  // 源站时钟漂移容忍度，与 005 一致

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

bool intel_feed_parse(const char *json, size_t len, intel_feed_t *out,
                      int *dropped, char *err, size_t err_cap)
{
    if (dropped) *dropped = 0;
    if (err && err_cap) err[0] = 0;
    if (!json || !out) {
        if (err && err_cap) snprintf(err, err_cap, "empty payload");
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        if (err && err_cap) snprintf(err, err_cap, "JSON syntax error");
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

    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(items)) {
        if (err && err_cap) snprintf(err, err_cap, "missing items[]");
        cJSON_Delete(root);
        return false;
    }

    int drop = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, items) {
        if (out->count >= INTEL_MAX_ITEMS) { drop++; continue; }
        intel_item_t parsed;
        if (parse_item(it, out, &parsed)) out->items[out->count++] = parsed;
        else drop++;
    }

    if (dropped) *dropped = drop;
    cJSON_Delete(root);

    if (out->count == 0) {
        if (err && err_cap) snprintf(err, err_cap, "no usable items (%d dropped)", drop);
        return false;
    }
    return true;
}
