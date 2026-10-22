# 005 · SIGNAL.LOG — FoloToy AI Passport 显示端

把仓库 `005`（章鱼AI · A股情报 / 事件雷达）的推送内容，显示在 **FoloToy AI Passport**
这块 240×320 的小屏上：黑底、磷光绿、反白选中、等宽字格——像一台挂在胸前的 DOS 终端。

```
005 抓取 + AI 分析 ──► items.json ──► tools/make_feed.py ──► feed.json（GitHub Pages）
                                                                    │ HTTPS
                                    ESP32-C3 拉取 ──► 校验/排序 ──► LVGL 渲染
```

## 现在就能看的两样东西

| 想看什么 | 去哪里 |
| --- | --- |
| **屏幕长什么样**（1:1 逻辑像素、可按键、可模拟断网） | [`preview/index.html`](preview/index.html) |
| **固件源码**（ESP-IDF 工程，基于 FoloToy 官方 BSP） | [`main/`](main/) |

本地起预览：

```bash
cd preview && python3 -m http.server 8000
# 打开 http://localhost:8000
```

预览不是"效果图"：它和固件共用同一套排版常量（半格 6 px、全角 12 px、列表 38 格换行、
6 行 × 46 px、详情 17 行 × 15 px），换行、时间分级、丢弃规则也逐条对齐，改固件时先在这里看。

## 硬件

| 项目 | 规格 | 本应用怎么用 |
| --- | --- | --- |
| SoC | ESP32-C3，8 MB Flash，无 PSRAM | 单 app 分区，rodata 放位图字体 |
| 屏幕 | ST7789P3，240×320 竖屏，RGB565，SPI2 40 MHz | LVGL 9.5，双 label 布局，无图片资源 |
| 按键 | UP / DOWN / OK，GPIO0 ADC 电阻梯 | 全部交互只用这三颗键 |
| 电量 | CW2017（I2C） | 顶栏右侧显示百分比 |
| 无线 | Wi-Fi 2.4 GHz | 只做 STA，拉 HTTPS feed |
| 音频 / BLE | ES8311 / NimBLE | 本应用未使用（预留） |

BSP 直接取自 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 的 `main`
（`components/bsp`，MIT，见 `LICENSE`），没有改动任何一行：改的是 `main/` 里的应用层。

## 界面

**列表（SIGNAL.LOG）** — 首屏

```
|SIGNAL.LOG                       09:12 87%     <- 顶栏：同步状态 / 时钟 / 电量
──────────────────────────────────────────────
[EXACT] 09:12 东方财富              +3.21%    <- 时间等级+时间+来源，右角涨跌
国常会：部署加快培育人形机器人产业
政策从"鼓励"转向"部署"…                      <- 标题 2 行，选中行整条反白
──────────────────────────────────────────────
… 共 6 行可见，▲▼ 循环移动
▲▼ 选择  OK 详情  3/8
```

**详情** — 17 行，多于一屏时 ▲▼ 滚动、OK 翻页

```
| 3/8 巨潮资讯                                  <- 反白标题栏
   AI 一句话                                    <- 反白小标题
   政策定调，上游减速器与丝杠确定性提升
   ─────────────────────────────────────
   300750 宁德时代
   231.50  +2.31%                               <- 涨=亮红，跌=亮青（CGA）
   ─────────────────────────────────────
   10-23 08:42 EXACT · 8分前
   来源 巨潮资讯
   标签 锂电 / 业绩
   倾向 偏多
   ─────────────────────────────────────
   原文
   https://…
```

**状态页**：开机 `SIGNAL.LOG`、拉取失败 `NO SIGNAL` + 原因 + `OK 重试`；
拉取中顶栏显示 `SYNC..`，失败但旧数据还在时显示 `STALE`（不白屏）。

## 按键

| 按键 | 列表 | 详情 | 状态页 |
| --- | --- | --- | --- |
| ▲ / ▼ | 移动选中（到头循环） | 逐行滚动 | — |
| OK 短按 | 打开详情 | 翻一页（到底回顶） | 重新拉取 |
| OK 长按 | 立即刷新 | 返回列表 | 重新拉取 |

## 烧录到真机（一步）

编译在 GitHub runner 上完成——这个开发沙箱没有 USB 直通，也没有 ESP-IDF 工具链，
所以产出的是可直接写入的镜像：

```
https://github.com/k-macao/05-folo/releases/download/firmware-latest/005-signal-log-full.bin
https://github.com/k-macao/05-folo/releases/download/firmware-latest/SHA256SUMS.txt
```

（同一份东西也在 Actions → firmware → Artifacts 里，30 天有效）

下载解压到 `build/`，然后把设备插到你自己的电脑上（能传数据的 Type-C 线）：

```bash
./tools/flash.sh                      # 从 0x0 写合并镜像（整机刷新）
./tools/flash.sh --mode app           # 只写 app 到 0x10000，保留 NVS
```

脚本自动找串口、校验 sha256、按 FoloToy 的约定给出提示（合并镜像会覆盖 NVS/PHY）。
FoloToy 官方的[安装社区玩法](https://ai-passport.folotoy.cn/guides/flash-official-firmware/)
页面只接受社区托管的作品，不接受任意 `.bin`，所以自建固件用上面的 esptool 路径。

细节与上机验收清单：[`docs/flashing.md`](docs/flashing.md)。

## 固件：编译与烧录

要求 **ESP-IDF 5.5.3**（ESP32-C3）。组件依赖由 `components/bsp/idf_component.yml` 声明
（LVGL 9.5.0、esp_lvgl_port 2.9.0、button 4.2.0、esp_codec_dev 1.6.2），首次 `build` 自动拉取。

```bash
idf.py set-target esp32c3
idf.py menuconfig          # → 005 Signal Log：Wi-Fi SSID / 密码 / feed URL / 备用 URL / 刷新间隔
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

合并镜像（按 AI Passport 的烧录约定，从 `0x0` 烧）：

```bash
idf.py merge-bin -o 005-signal-log-full.bin   # 产物落在 build/ 下
```

刷新间隔默认 10 分钟（`CONFIG_APP_REFRESH_MINUTES`），与 005 的 180 分钟窗口配合，
列表始终只显示三小时内的情报。

## 005 侧：把推送变成 feed.json

1. 005 每轮把要推送的条目按 [`docs/feed-format.md`](docs/feed-format.md) 的字段 dump 成
   `items.json`（`title`、`time`、`time_grade` 三个字段是硬要求）。
2. 收窄成设备要的 12 条：

   ```bash
   python3 tools/make_feed.py --in items.json --out preview/feed.json
   python3 tools/make_feed.py --in items.json --check   # 只校验，CI 用
   ```

   `make_feed.py` 会丢掉没有标题、没有时间、`time_grade` 不可信或时间无法解析的条目，
   丢掉超出窗口 / 来自未来的条目，按时间倒序取前 12 条，并按屏幕格数截断长文本。
3. 托管：GitHub Pages、对象存储、任意静态站点都行，把 URL 填进 `CONFIG_APP_FEED_URL`，
   再给一个 `CONFIG_APP_FEED_URL_FALLBACK`（可选，主地址失败时自动重试；CDN 有缓存，
   镜像只适合当备用）。
   feed 体积上限 24 KiB（12 条中文情报约 3–4 KiB）。

## 目录结构

```
main/
  main.c            应用状态机：三个屏幕、按键分发、拉取线程
  ui_crt.c          DOS/CRT 渲染（列表 / 详情 / 状态页），全部是 label + 矩形
  intel_logic.c     纯逻辑：UTF-8 字宽、换行、时间解析、详情页行模型（可主机测试）
  intel_feed.c      feed JSON → 内存模型，字段收窄与丢弃规则（cJSON）
  app_wifi.c        Wi-Fi STA（凭证来自 Kconfig）
  app_feed.c        HTTPS 拉取 + 解析
  fonts/crt_font_12.c  4 bpp 位图字体，4346 个码点（ASCII + GB2312 一级字 + 标点符号）
components/bsp/     FoloToy 官方 BSP（MIT，原样引入）
preview/            浏览器 1:1 预览（和固件同一套排版规则）
tools/
  gen_font.py       LVGL 9 位图字体生成器（Pillow 栅格化 + 自校验）
  make_feed.py      005 条目 → feed.json
  check.sh          主机侧全量校验
tests/              主机测试（gcc，不需要 ESP-IDF）
docs/               feed 契约、字体资产说明
```

## 验证状态

| 项目 | 结果 | 说明 |
| --- | --- | --- |
| Build | **PASS** | GitHub Actions + `espressif/idf:v5.5.3` 编译通过；合并镜像 1,903,008 B，镜像结构校验（引导头/ESP32-C3/分区表 MD5/app 描述符）全绿 |
| 语法检查 | **PASS** | `main/*.c` 与字体文件用最小桩头文件做过 `gcc -fsyntax-only -Wall -Wextra`；只验证 C 语法与类型，不代表真实 LVGL/IDF API 能链接通过 |
| Host tests | **PASS** | `./tools/check.sh`：逻辑/换行/时间/解析共 40+ 断言 + 字体码点覆盖 |
| Device tests | **NOT RUN** | 没有连接设备；未执行任何真机验证 |
| Unverified | 屏幕上的中文渲染、Wi-Fi/TLS 实际连通、按键手感、电量读数、刷新时的闪烁 |

第一次上真机时建议按这个顺序看：开机是否进入列表 → 顶栏时钟是否来自 `generated_at` →
Wi-Fi 失败时是否出现 `NO SIGNAL` 而不是黑屏 → 详情页长文本滚动是否完整 → 字体是否有方块缺字。

## 已知限制

- **没有离线持久化**：上一轮 feed 只存在 RAM，断电后必须重新联网拉取（NVS blob 单值上限
  4000 B，装不下 8 KB 的解析结果；要做需要单独的数据分区）。
- **配网是编译期的**：Wi-Fi 凭证写在 Kconfig 里，没有配网界面，也没有 BLE 配网。
- **每轮最多 12 条**：超出的在 `make_feed.py` 里裁掉，不占用设备内存。
- **字体是固定 12 px 位图**：缺字会显示方块；`intel_normalize()` 已把中文字体常见缺失的
  `— ° ′ ✓` 映射成 ASCII。要扩字集就改 `tools/gen_font.py` 重新生成。
- **没有 deep sleep / 自动息屏**：屏幕常亮，关机靠拔电。
