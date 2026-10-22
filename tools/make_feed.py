#!/usr/bin/env python3
"""把 005「章鱼AI · 事件雷达」的推送条目整理成设备要的 feed.json。

005 侧只需要把这一轮要推送的条目按下面的结构 dump 出来（字段见
docs/feed-format.md），剩下的收窄工作由本脚本完成：

  * 丢弃没有标题 / 没有时间 / 时间不可信（MISSING）/ 时间无法解析的条目
    —— 与固件、005 相同的"宁可丢弃，不可臆造"。
  * 丢弃超出窗口（默认 180 分钟）或比 generated_at 晚 10 分钟以上的条目。
  * 按时间倒序，最多 12 条；标题/一句话/URL 按屏幕格数截断。
  * 缺 id 时用 title+time 的 sha1 前 8 位补齐，方便设备端去重。
  * 输出紧凑 JSON（约 4 KB / 12 条），并打印体积与丢弃原因。

用法：
    python3 tools/make_feed.py --in items.json --out preview/feed.json
    python3 tools/make_feed.py --in items.json --check        # 只校验，不写文件
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

MAX_ITEMS = 12
GRADES = {"EXACT", "DATE", "DERIVED"}
TZ8 = timezone(timedelta(hours=8))

# 屏幕格数上限：列表两行 38 格、详情 5 行 36 格 / URL 3 行 36 格。
LIMITS = {"title": 72, "one_line": 180, "source_name": 24, "url": 108, "tag": 18}


def cells(text: str) -> int:
    return sum(1 if ord(ch) < 0x80 else 2 for ch in text)


def fit_cells(text: str, limit: int) -> str:
    if cells(text) <= limit:
        return text
    out = []
    used = 0
    for ch in text:
        w = 1 if ord(ch) < 0x80 else 2
        if used + w + 2 > limit:
            break
        out.append(ch)
        used += w
    return "".join(out) + "…"


def parse_time(value) -> datetime | None:
    if not isinstance(value, str):
        return None
    s = value.strip()
    if not s:
        return None
    try:
        dt = datetime.fromisoformat(s.replace("Z", "+00:00"))
    except ValueError:
        return None
    if dt.tzinfo is None:          # 没写时区：按东八区理解，并记录在案
        dt = dt.replace(tzinfo=TZ8)
    return dt.astimezone(TZ8)


def clean_text(value, limit: int) -> str:
    if not isinstance(value, str):
        return ""
    text = re.sub(r"\s+", " ", value).strip()
    return fit_cells(text, limit)


def normalise_item(raw: dict, dropped: list[tuple[str, str]]) -> dict | None:
    title = clean_text(raw.get("title"), LIMITS["title"])
    if not title:
        dropped.append((raw.get("id", "?"), "no title"))
        return None

    dt = parse_time(raw.get("time"))
    if dt is None:
        dropped.append((raw.get("id", "?"), "bad time"))
        return None

    grade = str(raw.get("time_grade", "")).upper()
    if grade not in GRADES:
        dropped.append((raw.get("id", "?"), f"untrusted time ({grade or 'empty'})"))
        return None

    item = {
        "id": str(raw.get("id") or hashlib.sha1(
            (title + dt.isoformat()).encode()).hexdigest()[:8]),
        "title": title,
        "time": dt.isoformat(timespec="seconds"),
        "time_grade": grade,
        "source_name": clean_text(raw.get("source_name"), LIMITS["source_name"]),
    }

    one_line = clean_text(raw.get("one_line"), LIMITS["one_line"])
    if one_line:
        item["one_line"] = one_line

    tags = raw.get("tags")
    if isinstance(tags, list):
        cleaned = [clean_text(t, LIMITS["tag"]) for t in tags]
        cleaned = [t for t in cleaned if t][:3]
        if cleaned:
            item["tags"] = cleaned

    bias = raw.get("bias")
    if bias in {"bull", "bear", "neutral"}:
        item["bias"] = bias

    sec = raw.get("security")
    if isinstance(sec, dict):
        code = clean_text(sec.get("code"), 12)
        name = clean_text(sec.get("name"), 24)
        price = sec.get("price")
        change = sec.get("change_pct")
        if (code or name) and isinstance(price, (int, float)) and isinstance(change, (int, float)):
            item["security"] = {
                "code": code,
                "name": name,
                "price": round(float(price), 3),
                "change_pct": round(float(change), 3),
            }
        elif code or name:
            item["security"] = {"code": code, "name": name}

    url = raw.get("url")
    if isinstance(url, str) and url.startswith(("http://", "https://")):
        item["url"] = fit_cells(url, LIMITS["url"])

    return item


def build(raw: dict, window: int) -> tuple[dict, list[tuple[str, str]]]:
    dropped: list[tuple[str, str]] = []
    generated = parse_time(raw.get("generated_at")) or datetime.now(TZ8)

    items = raw.get("items")
    if not isinstance(items, list):
        raise SystemExit("input must be an object with an items[] array")

    kept = []
    for entry in items:
        if not isinstance(entry, dict):
            dropped.append(("?", "not an object"))
            continue
        item = normalise_item(entry, dropped)
        if item is None:
            continue

        dt = parse_time(item["time"])
        age_min = (generated - dt).total_seconds() / 60
        if age_min > window + 5:
            dropped.append((item["id"], f"older than window ({age_min:.0f} min)"))
            continue
        if age_min < -10:
            dropped.append((item["id"], "from the future"))
            continue
        kept.append(item)

    kept.sort(key=lambda i: i["time"], reverse=True)
    if len(kept) > MAX_ITEMS:
        for extra in kept[MAX_ITEMS:]:
            dropped.append((extra["id"], "over the 12-item screen budget"))
        kept = kept[:MAX_ITEMS]

    feed = {
        "schema": 1,
        "generated_at": generated.isoformat(timespec="seconds"),
        "source": clean_text(raw.get("source") or "005 章鱼AI", 24),
        "window_minutes": window,
        "items": kept,
    }
    return feed, dropped


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True)
    ap.add_argument("--out", dest="dst")
    ap.add_argument("--window", type=int, default=180)
    ap.add_argument("--check", action="store_true", help="只校验，不写文件")
    args = ap.parse_args()

    raw = json.loads(Path(args.src).read_text(encoding="utf-8"))
    feed, dropped = build(raw, args.window)

    for item_id, reason in dropped:
        print(f"drop {item_id}: {reason}", file=sys.stderr)

    if not feed["items"]:
        print("no usable items — the device would show NO SIGNAL", file=sys.stderr)
        if not args.check:
            raise SystemExit(1)

    payload = json.dumps(feed, ensure_ascii=False, separators=(",", ":"))
    if cells(payload) and len(payload.encode()) > 24000:
        raise SystemExit(f"feed is {len(payload.encode())} bytes, device buffer is 24576")

    if args.check:
        print(f"ok: {len(feed['items'])} items, {len(payload.encode())} bytes, "
              f"{len(dropped)} dropped")
        return

    dst = Path(args.dst or "feed.json")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(payload + "\n", encoding="utf-8")
    print(f"wrote {dst}: {len(feed['items'])} items, {len(payload.encode())} bytes, "
          f"{len(dropped)} dropped")


if __name__ == "__main__":
    main()
