// tests/test_feed.c —— intel_feed 解析规则（时间缺失/来自未来一律丢弃）。
#include "intel_feed.h"
#include "intel_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static const char *kValid =
    "{"
    "  \"schema\": 1,"
    "  \"generated_at\": \"2026-10-23T09:00:00+08:00\","
    "  \"source\": \"005 章鱼AI\","
    "  \"window_minutes\": 180,"
    "  \"items\": ["
    "    {\"id\":\"a1\",\"title\":\"国常会：加快培育人形机器人产业\","
    "     \"one_line\":\"政策定调\",\"source_name\":\"东方财富\","
    "     \"time\":\"2026-10-23T08:42:00+08:00\",\"time_grade\":\"EXACT\","
    "     \"tags\":[\"政策\",\"机器人\"],\"bias\":\"bull\","
    "     \"security\":{\"code\":\"300750\",\"name\":\"宁德时代\",\"price\":231.5,\"change_pct\":2.31},"
    "     \"url\":\"https://example.com/a\"},"
    "    {\"id\":\"a2\",\"title\":\"北向资金净流入\",\"time\":\"2026-10-23T07:10:00+08:00\","
    "     \"time_grade\":\"DERIVED\",\"source_name\":\"同花顺\"}"
    "  ]"
    "}";

static void test_valid(void)
{
    intel_feed_t feed;
    int dropped = -1;
    char err[96] = { 0 };
    CHECK(intel_feed_parse(kValid, strlen(kValid), &feed, &dropped, err, sizeof err));
    CHECK(feed.count == 2);
    CHECK(dropped == 0);
    CHECK(feed.schema_ok);
    CHECK(strcmp(feed.generated_hhmm, "09:00") == 0);
    CHECK(feed.window_minutes == 180);
    CHECK(strcmp(feed.items[0].hhmm, "08:42") == 0);
    CHECK(feed.items[0].grade == INTEL_TIME_EXACT);
    CHECK(feed.items[0].quote.has_quote);
    CHECK(feed.items[0].quote.price > 231.4f && feed.items[0].quote.price < 231.6f);
    CHECK(feed.items[0].tag_count == 2);
    CHECK(feed.items[0].bias == INTEL_BIAS_BULL);
    CHECK(strcmp(feed.items[1].hhmm, "07:10") == 0);
    CHECK(feed.items[1].grade == INTEL_TIME_DERIVED);
    CHECK(!feed.items[1].quote.has_quote);
}

static void test_drops(void)
{
    const char *json =
        "{\"generated_at\":\"2026-10-23T09:00:00+08:00\",\"items\":["
        " {\"title\":\"无时间\",\"time_grade\":\"EXACT\"},"
        " {\"title\":\"时间不可信\",\"time\":\"2026-10-23T08:00:00+08:00\",\"time_grade\":\"MISSING\"},"
        " {\"title\":\"来自未来\",\"time\":\"2026-10-24T08:00:00+08:00\",\"time_grade\":\"EXACT\"},"
        " {\"title\":\"坏时间\",\"time\":\"昨天\",\"time_grade\":\"EXACT\"},"
        " {\"title\":\"\",\"time\":\"2026-10-23T08:00:00+08:00\",\"time_grade\":\"EXACT\"},"
        " {\"title\":\"这条可用\",\"time\":\"2026-10-23T08:00:00+08:00\",\"time_grade\":\"DATE\"}"
        "]}";
    intel_feed_t feed;
    int dropped = 0;
    CHECK(intel_feed_parse(json, strlen(json), &feed, &dropped, NULL, 0));
    CHECK(feed.count == 1);
    CHECK(dropped == 5);
    CHECK(strcmp(feed.items[0].title, "这条可用") == 0);
    CHECK(strcmp(feed.source, "005") == 0);   // 缺省来源
}

static void test_limits(void)
{
    char json[4096];
    size_t off = (size_t)snprintf(json, sizeof json, "{\"items\":[");
    for (int i = 0; i < INTEL_MAX_ITEMS + 3; i++) {
        off += (size_t)snprintf(json + off, sizeof json - off,
                                "%s{\"title\":\"T%d\",\"time\":\"2026-10-23T08:00:00+08:00\","
                                "\"time_grade\":\"EXACT\"}",
                                i ? "," : "", i);
    }
    snprintf(json + off, sizeof json - off, "]}");

    intel_feed_t feed;
    int dropped = 0;
    CHECK(intel_feed_parse(json, strlen(json), &feed, &dropped, NULL, 0));
    CHECK(feed.count == INTEL_MAX_ITEMS);   // 超出的条目被截断，不覆盖内存
    CHECK(dropped == 3);
}

static void test_errors(void)
{
    intel_feed_t feed;
    char err[96];
    const char *bad = "{\"items\":[}";
    CHECK(!intel_feed_parse(bad, strlen(bad), &feed, NULL, err, sizeof err));
    CHECK(strstr(err, "JSON") != NULL);

    const char *empty = "{\"items\":[]}";
    CHECK(!intel_feed_parse(empty, strlen(empty), &feed, NULL, err, sizeof err));
    CHECK(strstr(err, "no usable items") != NULL);

    const char *noitems = "{}";
    CHECK(!intel_feed_parse(noitems, strlen(noitems), &feed, NULL, err, sizeof err));
    CHECK(!intel_feed_parse(NULL, 0, &feed, NULL, err, sizeof err));
}

static void test_sample_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("SKIP sample feed %s (not found)\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    CHECK(buf != NULL);
    if (!buf) { fclose(f); return; }
    const size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = 0;
    fclose(f);

    intel_feed_t feed;
    int dropped = 0;
    char err[96] = { 0 };
    const bool ok = intel_feed_parse(buf, got, &feed, &dropped, err, sizeof err);
    if (!ok) printf("sample parse error: %s\n", err);
    CHECK(ok);
    CHECK(feed.count > 0);
    CHECK(dropped == 0);
    if (ok) printf("sample feed: %d items, %d bytes\n", feed.count, (int)got);
    free(buf);
}

int main(int argc, char **argv)
{
    test_valid();
    test_drops();
    test_limits();
    test_errors();
    test_sample_file(argc > 1 ? argv[1] : "../preview/feed.json");

    if (failures) {
        printf("test_feed: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_feed: all checks passed\n");
    return 0;
}
