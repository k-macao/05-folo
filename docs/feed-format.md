# feed.json 契约（设备 ↔ 005）

设备只认一个 URL 上的一个 JSON。005 负责抓取与判断，`tools/make_feed.py` 负责收窄，
固件只负责**校验、排序、显示**——不做业务推断。

```jsonc
{
  "schema": 1,                                  // 必须为 1；其它值设备仍会尝试解析，但会记日志
  "generated_at": "2026-10-23T09:20:00+08:00",  // 本轮生成时间，设备用它当"现在"
  "source": "005 章鱼AI",                        // 顶栏 / 日志用
  "window_minutes": 180,                        // 只做展示，不参与过滤
  "items": [                                    // 最多 12 条，按时间倒序
    {
      "id": "8f3a1c02",                         // 去重指纹，缺省由 make_feed 用 sha1 补
      "title": "国常会：部署加快培育人形机器人产业",   // 必填，>72 格会被截断
      "one_line": "政策从“鼓励”转向“部署”，减速器确定性最高",
      "source_name": "东方财富",
      "time": "2026-10-23T09:12:00+08:00",       // 必填，ISO 8601，必须带时区（或缺省按东八区）
      "time_grade": "EXACT",                     // 必填：EXACT | DATE | DERIVED
      "tags": ["政策", "机器人"],                  // 最多 3 个，每个 ≤18 格
      "bias": "bull",                            // bull | bear | neutral（缺省不显示）
      "security": {                              // 可缺省；只有识别到标的时才给
        "code": "300124",
        "name": "汇川技术",
        "price": 68.42,
        "change_pct": 3.21
      },
      "url": "https://example.com/005/robot-policy"   // ≤108 格，详情页"原文"展示
    }
  ]
}
```

## 设备端的硬规则（`main/intel_feed.c`）

解析不是"尽量多收"，而是**宁可丢弃，不可臆造**——与 005 的时间校验原则一致：

| 情况 | 处理 |
| --- | --- |
| 没有 `title`，或规范化后为空 | 丢弃 |
| 没有 `time`，或不是合法 ISO 8601 | 丢弃 |
| `time_grade` 缺失 / 不是 `EXACT` / `DATE` / `DERIVED`（即 `MISSING`） | 丢弃 |
| `time` 比 `generated_at` 晚 10 分钟以上（源站时钟漂移容忍度） | 丢弃 |
| 条目数超过 12 | 丢弃多余项 |
| 全部条目都被丢弃 | 整轮失败，屏幕保持上一轮数据并标 `STALE`；无旧数据则 `NO SIGNAL` |
| 响应 > 24576 字节 | 整轮失败（防止把 RAM 拉爆） |

丢弃数量会写进日志，也会记在预览右侧面板上，便于确认"今天为什么只有 6 条"。

## 字段在屏幕上的位置

| 字段 | 列表 | 详情 |
| --- | --- | --- |
| `time_grade` + `time` | 每行 meta 行（`[EXACT] 09:12`，颜色按等级） | `10-23 08:42 EXACT · 8分前` |
| `source_name` | meta 行右侧 | 标题栏右侧 + `来源` 行 |
| `title` | 2 行 × 38 格 | 不显示（详情页聚焦结论） |
| `one_line` | 不显示 | `AI 一句话`（反白标题 + 白字正文，最多 5 行） |
| `security` | 右上角涨跌幅（涨红跌青） | 代码/名称一行，价格 + 涨跌幅一行 |
| `tags` | 不显示 | `标签 锂电 / 业绩` |
| `bias` | 不显示 | `倾向 偏多` |
| `url` | 不显示 | `原文`，最多 3 行 |

## 时间分级为什么必须传

设备不会去猜发布时间：`time_grade` 是 005 对这条内容时间可信度的判断。
传错比不传更糟——`EXACT` 会被当成"精确到秒"，`DATE` 显示琥珀色，`DERIVED` 显示橙色并带
"推算"的语义，`MISSING` 直接丢弃。**不要用抓取时刻冒充发布时间。**

## 托管

任何静态托管都行（GitHub Pages / 对象存储 / 自己的服务器）：

```bash
python3 tools/make_feed.py --in items.json --out public/feed.json
python3 tools/make_feed.py --in items.json --check     # CI 里校验体积与丢弃原因
```

- 体积：8 条中文情报约 3.3 KiB，12 条约 4.5 KiB，远低于 24 KiB 上限。
- 缓存：设备请求带 `Cache-Control: no-cache`；如果前面有 CDN，请给 `feed.json` 设很短的 TTL。
- HTTPS：固件内置 ESP-IDF 证书 bundle，GitHub Pages / 常见对象存储都不需要额外配置证书。

## 从 005 导出 items.json

005 现在的推送正文是 HTML，条目对象在 `main.py` 的抓取结果里。导出时只需要把这些条目
映射成本文的字段，`title` / `time` / `time_grade` 三个必须真实存在：

```python
# 005 侧示意（放在推送前，和渲染 HTML 并列）
items = [{
    "id": it.fingerprint,
    "title": it.title,
    "one_line": it.ai_one_liner,
    "source_name": it.source,
    "time": it.published_at.isoformat(),     # 带时区
    "time_grade": it.time_quality.name,      # EXACT / DATE / DERIVED
    "tags": it.tags[:3],
    "bias": it.bias,                         # 可缺省
    "security": {"code": it.code, "name": it.name,
                 "price": it.price, "change_pct": it.change_pct},
    "url": it.url,
} for it in pushed_items]
json.dump({"generated_at": now.isoformat(), "source": "005 章鱼AI",
           "window_minutes": 180, "items": items}, open("items.json", "w"),
          ensure_ascii=False)
```

导出后立刻跑一次 `make_feed.py --check`，被丢弃的条目会逐条打印原因——这是最省事的
"字段映射是否正确"的验证方式。
