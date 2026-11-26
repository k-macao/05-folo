#!/usr/bin/env python3
"""把 feed.json 摘要成 GitHub Actions 的 Step Summary（Markdown）。

    python3 tools/feed_summary.py preview/feed.json >> "$GITHUB_STEP_SUMMARY"
"""

from __future__ import annotations

import json
import pathlib
import sys


def main() -> int:
    path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "preview/feed.json")
    print("## 005 每日新闻")
    print()
    if not path.exists():
        print("_没有 feed 文件_")
        return 0

    feed = json.loads(path.read_text(encoding="utf-8"))
    items = feed.get("items") or []
    print(f"- 生成时间：`{feed.get('generated_at', '—')}`")
    print(f"- 条数：{len(items)} / 窗口 {feed.get('window_minutes', '—')} 分钟")
    print(f"- 体积：{path.stat().st_size} B（固件上限 24576 B）")
    print("- 原链接：无（条目里不携带 url）")
    print()
    if not items:
        print("_本轮没有可用条目，工作流不应提交_")
        return 0
    for item in items[:6]:
        stamp = str(item.get("time", ""))[11:16]
        print(f"- `{stamp}` **{item.get('source_name', '—')}** · {item.get('title', '—')}")
    if len(items) > 6:
        print(f"- … 另外 {len(items) - 6} 条")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
