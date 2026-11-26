#!/usr/bin/env python3
"""tests/test_daily_news.py —— 每日新闻抓取的离线测试（不联网）。

用抓下来的两份真实响应结构做夹具，验证：窗口过滤、去重、标签归类、
摘要去重前缀、以及「feed 里绝不出现原始链接」。
"""

from __future__ import annotations

import json
import pathlib
import sys
from datetime import datetime, timedelta, timezone

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import make_feed  # noqa: E402
import fetch_daily  # noqa: E402

TZ8 = timezone(timedelta(hours=8))
NOW = datetime(2026, 11, 27, 0, 30, 0, tzinfo=TZ8)

failures = 0


def check(cond, msg):
    global failures
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        failures += 1


def payloads():
    em = json.loads((ROOT / "tests/fixtures/eastmoney_roll.json").read_text(encoding="utf-8"))
    sina = json.loads((ROOT / "tests/fixtures/sina_roll.json").read_text(encoding="utf-8"))
    return [("eastmoney", em), ("sina", sina)]


def main():
    feed, dropped = fetch_daily.build_feed(payloads(), NOW, 1440)
    items = feed["items"]

    check(len(items) == 4, f"窗口内去重后 4 条（实际 {len(items)}）")
    check(feed["generated_at"] == "2026-11-27T00:30:00+08:00", "generated_at 就是本轮时间")
    check(feed["window_minutes"] == 1440, "window_minutes = 1440")
    check(feed["schema"] == 1, "schema = 1")

    times = [i["time"] for i in items]
    check(times == sorted(times, reverse=True), "按时间倒序")
    check(all(t.endswith("+08:00") for t in times), "时间统一为东八区 ISO 8601")
    check(all(i["time_grade"] == "EXACT" for i in items), "抓取到的发布时间记为 EXACT")

    titles = [i["title"] for i in items]
    check(sum("301关税" in t for t in titles) == 1, "同一标题跨源出现只保留一条")

    by_title = {i["title"]: i for i in items}
    tariff = by_title["商务部：坚决反对美对中国等经济体加征301关税"]
    check(tariff["one_line"].startswith("针对美国贸易代表办公室"),
          "摘要里的【标题】前缀已去掉，不重复标题")
    check(not tariff["one_line"].startswith("【"), "摘要不含【…】前缀")
    check("政策" in tariff["tags"] and "海外" in tariff["tags"], "关税条目归类为 政策/海外")
    check("算力" in by_title["广东：鼓励算力中心优先使用国产算力芯片 提升国产化替代水平"]["tags"],
          "算力条目归类为 算力")

    check(not any("旧闻" in i["one_line"] for i in items), "超过 24 小时的旧闻被丢弃")
    check(not any("未来" in i["one_line"] for i in items), "来自未来的条目被丢弃")

    payload = json.dumps(feed, ensure_ascii=False)
    check("http://" not in payload and "https://" not in payload, "feed 里没有任何 URL")
    check(not any("url" in i for i in items), "条目没有 url 字段")

    # 时间窗收窄后只剩最近一小时的条目
    narrow, _ = fetch_daily.build_feed(payloads(), NOW, 60)
    check(len(narrow["items"]) == 2, f"60 分钟窗口只剩 2 条（实际 {len(narrow['items'])}）")

    # 复用 make_feed 的收窄规则：走一遍 check 路径，确认可以原样回灌
    rebuilt, dropped2 = make_feed.build(feed, 1440)
    check(len(rebuilt["items"]) == len(items), "feed 可以被 make_feed 原样再收窄一次")
    check(not dropped2, "回灌时没有条目被丢弃")

    # 全部源失败时不能写出空 feed（模拟 HTTP 失败）
    original = fetch_daily.http_get
    fetch_daily.http_get = lambda *a, **k: (_ for _ in ()).throw(RuntimeError("boom"))
    try:
        got, errors = fetch_daily.fetch_all(fetch_daily.SOURCES, 1)
        check(got == [] and len(errors) == 2, "所有源失败时返回空 payload 与错误列表")
    finally:
        fetch_daily.http_get = original

    print("test_daily_news: " + ("all checks passed" if not failures else f"{failures} failure(s)"))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
