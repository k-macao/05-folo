# 每日新闻：每天怎么更新、为什么不放链接

设备上的内容不是一次性写死的示例。`.github/workflows/daily-feed.yml` 会在每天
**07:10 / 11:10 / 15:10 / 19:10（北京时间）** 各跑一轮，把最近 24 小时的新闻写进
`preview/feed.json`；固件默认每 10 分钟拉一次，所以最迟 10 分钟就换到新内容。

```
东方财富「财经要闻」──┐
                     ├─ fetch_daily.py ─→ items（title / summary / source / time / tags）
新浪财经「财经滚动」──┘                        │
                                             ├─ make_feed.build：窗口 1440 分钟、去重、倒序、留 12 条
                                             └─ preview/feed.json（**没有任何 URL**）
                                                        │
                                              ESP32-C3 拉取 → LVGL 渲染
```

## 为什么不用手写 feed

手写的示例数据有两个问题：过几天就成了「旧闻」，而且详情页里会留一条
`https://example.com/...` 的假链接。设备没有浏览器，点不开也不需要点，
所以现在**从数据源头就不写 url**：

- `tools/fetch_daily.py` 只保留标题、导语摘要、媒体名、发布时间、关键词标签；
- `tools/make_feed.py` 会把上游带来的 `url` 字段直接忽略（不再写进 feed）；
- `main/intel_feed.c` 不再解析 `url`，`main/intel_logic.c` 的详情页没有「原文」一栏。

## 抓取规则

| 规则 | 说明 |
| --- | --- |
| 时间窗口 | 默认最近 1440 分钟（一天），未来 10 分钟以外的条目视为脏数据丢弃 |
| 发布时间 | 用源站给的 `showTime` / `ctime`，`time_grade = EXACT`；**不拿抓取时刻冒充发布时间** |
| 去重 | 标题去掉标点与空白后做 key；同一条在多个源出现只留最新的一条 |
| 条数 | 最多 12 条（屏幕只放得下 6 行，翻页上限 12 条） |
| 摘要 | 源站导语，去掉开头的「【标题】」避免和标题重复，按 180 格（约 90 个汉字）截断 |
| 标签 | `TOPICS` 词表命中次数排序取前 2 个；没有命中就用频道兜底标签 |
| 倾向 / 行情 | 不生成：没有 AI 判断就不假装有 |

## 失败时会发生什么

- **某一个源失败**：另一个源照常出 feed（日志里能看到 `[fetch] ... FAILED`）。
- **所有源失败，或可用条目少于 3 条**：脚本以非零码退出，工作流不会提交，
  `preview/feed.json` 保持上一轮内容；设备继续显示，顶栏在拉取失败时显示 `STALE`。
- **feed 体积**：12 条摘要约 4–5 KiB，远低于固件 24 KiB 的缓冲上限。

## 换源 / 加源

编辑 `tools/fetch_daily.py`：

```python
SOURCES = (
    Source(kind="eastmoney", url="...column=345...", tag="财经"),
    Source(kind="sina",      url="...lid=2516...",    tag="市场"),
)
```

- `kind` 决定用哪个解析器（`parse_eastmoney` / `parse_sina`），新源需要加一个解析器并注册到 `PARSERS`。
- 解析器只需要产出 `title` / `summary` / `source` / `epoch`；过滤、去重、标签、排序都在
  `build_items()` 里统一做。
- 标签词表在 `TOPICS`，按 `(标签, 关键词元组)` 排列，命中次数多的优先。

## 本地验证（不联网）

```bash
python3 tests/test_daily_news.py          # 用 tests/fixtures/ 里的真实响应结构
python3 tools/make_feed.py --in preview/feed.json --check --window 1440
python3 -m http.server -d preview 8000    # 打开 http://localhost:8000 看屏幕
```

测试覆盖：窗口过滤、未来数据丢弃、跨源去重、摘要前缀清理、标签归类、
`feed` 中不出现任何 `http://` / `https://`、以及全部源失败时返回空 payload。

## 手动刷新

Actions → **daily-feed** → *Run workflow*，或在本地：

```bash
python3 tools/fetch_daily.py --out preview/feed.json --window 1440
```

脚本只在成功拿到 ≥3 条时写文件（先写 `.tmp` 再原子替换），所以中途失败也不会留下半份 feed。
