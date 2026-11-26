#!/usr/bin/env python3
"""每天抓一轮公开财经新闻，生成设备要的 feed.json（**不含原链接**）。

设计目标只有一个：让胸前那块小屏每天都有新东西，而不是一份写死的示例数据。

  * 源：东方财富「财经要闻」接口 + 新浪财经滚动接口（都是公开 JSON，无需 key）。
  * 只取最近 `--window` 分钟（默认 1440 = 24 小时）的条目，按时间倒序留 12 条。
  * 每条只有标题 / 摘要 / 来源 / 时间 / 标签，**不写入任何 URL**
    —— 设备没有浏览器，链接只会占满详情页，用户也明确要求不要。
  * 时间来自源站的发布时间，`time_grade` 固定 EXACT；抓取时刻不会被冒充成发布时间。
  * 去重：同一标题在多个源 / 多个频道出现时只留最新的一条。
  * 任一源失败不影响其它源；**全部失败时不写文件**，上一份 feed 继续服务，
    设备顶栏会自然进入 STALE，而不是被一份空 feed 覆盖成 NO SIGNAL。

用法（CI 里由 .github/workflows/daily-feed.yml 调用）：

    python3 tools/fetch_daily.py --out preview/feed.json
    python3 tools/fetch_daily.py --out preview/feed.json --window 1440 --min-items 3

    # 离线复现某一时刻（测试用，不联网）：
    python3 -c "import tools.fetch_daily as f; print(f.build_items(payloads, now))"
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import html
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_feed  # noqa: E402  —— 复用同一套收窄 / 排序 / 截断规则

TZ8 = timezone(timedelta(hours=8))
UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0 Safari/537.36 005-signal-log/1.0")
DEFAULT_WINDOW_MIN = 1440          # 一天
DEFAULT_MIN_ITEMS = 3              # 少于这个数就认为这轮抓取不可用，保留旧 feed
MAX_ITEMS = 12                     # 与屏幕 / 固件一致
FUTURE_TOLERANCE_MIN = 10          # 源站时钟漂移容忍度

# 标签只做关键词归类，不做情绪判断（没有 AI 时不要假装有）。
TOPICS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("政策", ("国常会", "国务院", "政策", "部署", "规划", "商务部", "发改委", "证监会",
              "央行", "财政部", "统计局", "工信部", "监管", "新规")),
    ("宏观", ("CPI", "PPI", "GDP", "PMI", "降准", "降息", "利率", "汇率", "通胀", "出口",
              "进口", "社融", "信贷", "财政", "就业", "经济")),
    ("算力", ("算力", "数据中心", "大模型", "人工智能", "AI", "英伟达", "智算")),
    ("芯片", ("芯片", "半导体", "晶圆", "光刻", "存储", "台积电")),
    ("机器人", ("机器人", "人形", "具身", "减速器", "丝杠")),
    ("新能源", ("锂电", "电池", "储能", "光伏", "风电", "新能源", "充电桩")),
    ("汽车", ("汽车", "车企", "整车", "智驾", "自动驾驶")),
    ("有色", ("稀土", "铜", "铝", "锂", "黄金", "有色", "镍")),
    ("消费", ("白酒", "消费", "食品", "饮料", "零售", "旅游", "免税", "家电")),
    ("医药", ("医药", "创新药", "医疗", "集采", "疫苗", "生物")),
    ("地产", ("房地产", "地产", "楼市", "住房", "物业")),
    ("金融", ("银行", "券商", "保险", "基金", "IPO", "上市", "并购", "重组", "债券",
              "瑞银", "汇丰", "高盛", "摩根", "花旗", "评级", "回购")),
    ("海外", ("美国", "美联储", "欧洲", "日本", "关税", "特朗普", "俄", "乌克兰",
              "中东", "伊朗", "OPEC", "原油")),
)


@dataclass(frozen=True)
class Source:
    kind: str          # "eastmoney" | "sina"
    url: str
    tag: str           # 没有命中关键词时的兜底标签


SOURCES: tuple[Source, ...] = (
    Source(
        kind="eastmoney",
        # column=345 = 财经要闻；page_size 取大一点，方便在本地按窗口过滤。
        url=("https://np-listapi.eastmoney.com/comm/web/getNewsByColumns"
             "?client=web&biz=web_news_col&column=345&order=1&needInteractData=0"
             "&page_index=1&page_size=40&req_trace={trace}"),
        tag="财经",
    ),
    Source(
        kind="sina",
        url=("https://feed.mix.sina.com.cn/api/roll/get"
             "?pageid=153&lid=2516&num=40&page=1"),
        tag="市场",
    ),
)


# --------------------------------------------------------------------------- #
# 抓取
# --------------------------------------------------------------------------- #
def http_get(url: str, timeout: float = 20.0, retries: int = 3) -> bytes:
    """带 UA / gzip / 重试的 GET。失败抛 RuntimeError。"""
    last: Exception | None = None
    for attempt in range(retries):
        req = urllib.request.Request(url, headers={
            "User-Agent": UA,
            "Accept": "application/json,text/plain,*/*",
            "Accept-Encoding": "gzip",
            "Referer": "https://finance.eastmoney.com/",
        })
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                body = resp.read()
                if resp.headers.get("Content-Encoding", "").lower() == "gzip" or body[:2] == b"\x1f\x8b":
                    body = gzip.decompress(body)
                return body
        except (urllib.error.URLError, TimeoutError, OSError, gzip.BadGzipFile) as exc:
            last = exc
            if attempt + 1 < retries:
                time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f"GET failed after {retries} tries: {last}")


def _clean(text) -> str:
    if not isinstance(text, str):
        return ""
    text = html.unescape(text)
    text = re.sub(r"<[^>]+>", " ", text)
    text = re.sub(r"\s+", " ", text).strip()
    return text


def _strip_summary_title(summary: str) -> str:
    """东财的 summary 常以「【标题】正文」开头，标题已经单独显示，去掉重复。"""
    return re.sub(r"^【[^】]{2,80}】\s*", "", summary).strip()


def _tags(title: str, summary: str, fallback: str) -> list[str]:
    """按命中次数挑两个标签；同分时按 TOPICS 的先后顺序。"""
    haystack = f"{title} {summary}"
    scored = []
    for order, (label, words) in enumerate(TOPICS):
        hits = sum(haystack.count(w) for w in words)
        if hits:
            scored.append((-hits, order, label))
    scored.sort()
    return [label for _, _, label in scored[:2]] or [fallback]


def _row(title, summary, source, epoch, fallback_tag) -> dict | None:
    title = _clean(title)
    summary = _strip_summary_title(_clean(summary))
    source = _clean(source)[:24]
    if not title or not isinstance(epoch, (int, float)) or epoch <= 0:
        return None
    return {
        "title": title,
        "summary": summary,
        "source": source,
        "epoch": int(epoch),
        "tags": _tags(title, summary, fallback_tag),
    }


def parse_eastmoney(payload: dict) -> list[dict]:
    data = payload.get("data")
    rows = data.get("list") if isinstance(data, dict) else None
    if not isinstance(rows, list):
        raise ValueError("eastmoney: missing data.list")
    out = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        try:
            dt = datetime.strptime(str(row.get("showTime", "")), "%Y-%m-%d %H:%M:%S")
        except ValueError:
            continue
        dt = dt.replace(tzinfo=TZ8)
        item = _row(row.get("title"), row.get("summary") or row.get("digest"),
                    row.get("mediaName") or "东方财富", dt.timestamp(), "财经")
        if item:
            out.append(item)
    return out


def parse_sina(payload: dict) -> list[dict]:
    result = payload.get("result")
    rows = result.get("data") if isinstance(result, dict) else None
    if not isinstance(rows, list):
        raise ValueError("sina: missing result.data")
    out = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        try:
            epoch = int(row.get("ctime") or 0)
        except (TypeError, ValueError):
            continue
        item = _row(row.get("title"), row.get("intro") or row.get("summary"),
                    row.get("media_name") or "新浪财经", epoch, "市场")
        if item:
            out.append(item)
    return out


PARSERS = {"eastmoney": parse_eastmoney, "sina": parse_sina}


# --------------------------------------------------------------------------- #
# 归一化
# --------------------------------------------------------------------------- #
def _title_key(title: str) -> str:
    return re.sub(r"[\s\u3000·、，,。.:：;；!！?？\"'“”‘’()（）\[\]【】—\-]+", "", title).lower()


def _near_duplicate(a: str, b: str) -> bool:
    """同一条新闻经常被两个源以「多/少一个前缀」的标题各发一次。"""
    if a == b:
        return True
    short, long = (a, b) if len(a) <= len(b) else (b, a)
    return len(short) >= 8 and short in long and len(short) / len(long) >= 0.8


def build_items(entries: list[dict], now: datetime, window_min: int) -> list[dict]:
    """按窗口过滤、去重、排序，输出 feed 条目（无 url 字段）。"""
    cutoff = now - timedelta(minutes=window_min + 5)
    future = now + timedelta(minutes=FUTURE_TOLERANCE_MIN)

    # 新的在前：先去重，保证同一条新闻留下最新（通常也是最完整）的那版。
    ordered = sorted(entries, key=lambda e: e["epoch"], reverse=True)
    kept: list[dict] = []
    keys: list[str] = []
    for entry in ordered:
        dt = datetime.fromtimestamp(entry["epoch"], TZ8)
        if dt < cutoff or dt > future:
            continue
        key = _title_key(entry["title"])
        if not key or any(_near_duplicate(key, seen) for seen in keys):
            continue
        keys.append(key)
        kept.append({
            "id": hashlib.sha1(key.encode("utf-8")).hexdigest()[:8],
            "title": entry["title"],
            "one_line": entry["summary"],
            "source_name": entry["source"],
            "time": dt.isoformat(timespec="seconds"),
            "time_grade": "EXACT",
            "tags": entry["tags"],
            # 明确不携带链接：即便上游给了也不要。
        })

    return kept[:MAX_ITEMS]


def build_feed(payloads: list[tuple[str, dict]], now: datetime, window_min: int) -> tuple[dict, list]:
    entries: list[dict] = []
    for kind, payload in payloads:
        parser = PARSERS.get(kind)
        if parser is None:
            raise ValueError(f"unknown source kind: {kind}")
        entries.extend(parser(payload))

    items = build_items(entries, now, window_min)
    raw = {
        "generated_at": now.isoformat(timespec="seconds"),
        "source": "005 每日新闻",
        "window_minutes": window_min,
        "items": items,
    }
    return make_feed.build(raw, window_min)


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def fetch_all(sources: tuple[Source, ...], timeout: float) -> tuple[list[tuple[str, dict]], list[str]]:
    payloads: list[tuple[str, dict]] = []
    errors: list[str] = []
    for src in sources:
        url = src.url.format(trace=time.strftime("%Y%m%d%H%M%S"))
        try:
            body = http_get(url, timeout=timeout)
            payload = json.loads(body.decode("utf-8", "replace"))
            payloads.append((src.kind, payload))
            print(f"[fetch] {src.kind}: ok ({len(body)} bytes)", file=sys.stderr)
        except Exception as exc:  # noqa: BLE001 —— 单源失败不影响其它源
            errors.append(f"{src.kind}: {exc}")
            print(f"[fetch] {src.kind}: FAILED — {exc}", file=sys.stderr)
    return payloads, errors


def write_feed(feed: dict, out: Path) -> int:
    payload = json.dumps(feed, ensure_ascii=False, separators=(",", ":")) + "\n"
    size = len(payload.encode("utf-8"))
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".tmp")
    tmp.write_text(payload, encoding="utf-8")
    os.replace(tmp, out)
    return size


def main() -> None:
    ap = argparse.ArgumentParser(description="抓取每日财经新闻，生成不含原链接的 feed.json")
    ap.add_argument("--out", default="preview/feed.json")
    ap.add_argument("--window", type=int, default=DEFAULT_WINDOW_MIN,
                    help="保留最近多少分钟的新闻（默认 1440 = 一天）")
    ap.add_argument("--min-items", type=int, default=DEFAULT_MIN_ITEMS,
                    help=f"少于这个条数就判失败、不覆盖旧 feed（默认 {DEFAULT_MIN_ITEMS}）")
    ap.add_argument("--now", default="", help="覆盖当前时间（ISO 8601，测试/复现用）")
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()

    if args.now:
        now = datetime.fromisoformat(args.now.replace("Z", "+00:00"))
        now = now.replace(tzinfo=TZ8) if now.tzinfo is None else now.astimezone(TZ8)
    else:
        now = datetime.now(TZ8)

    payloads, errors = fetch_all(SOURCES, args.timeout)
    if not payloads:
        print("所有新闻源都失败，保留上一份 feed：" + "; ".join(errors), file=sys.stderr)
        raise SystemExit(2)

    feed, dropped = build_feed(payloads, now, args.window)
    for item_id, reason in dropped:
        print(f"drop {item_id}: {reason}", file=sys.stderr)

    if len(feed["items"]) < args.min_items:
        print(f"只拿到 {len(feed['items'])} 条（< {args.min_items}），保留上一份 feed",
              file=sys.stderr)
        raise SystemExit(3)

    out = Path(args.out)
    size = write_feed(feed, out)
    newest = feed["items"][0]["time"] if feed["items"] else "-"
    oldest = feed["items"][-1]["time"] if feed["items"] else "-"
    print(f"wrote {out}: {len(feed['items'])} items, {size} bytes, "
          f"{oldest} .. {newest}, {len(dropped)} dropped")


if __name__ == "__main__":
    main()
