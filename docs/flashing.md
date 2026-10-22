# 烧录到真机

> **谁来做这一步**：烧录需要设备插在**你自己电脑**的 USB 口上。Arena 的会话沙箱没有
> USB 直通（`/dev/ttyACM*` 不存在），所以这里交付的是**可直接写入的镜像**和一条命令，
> 而不是"我已经帮你刷好了"。

## 镜像从哪来（两条路，任选）

**A. GitHub Actions 编译（推荐，不需要本地装 IDF）**

推送到本分支后，`firmware` 工作流会用官方 `espressif/idf:v5.5.3` 镜像编译并合并镜像：

```
GitHub → Actions → firmware → 最新一次运行 → Artifacts → 005-signal-log-full
```

里面是 `005-signal-log-full.bin` 和 `SHA256SUMS.txt`。下载解压到仓库的 `build/` 目录。

**B. 本地编译**

```bash
. $IDF_PATH/export.sh          # ESP-IDF 5.5.3
idf.py set-target esp32c3
idf.py menuconfig              # 005 Signal Log → Wi-Fi / feed URL / 刷新间隔
idf.py build
idf.py merge-bin -o 005-signal-log-full.bin   # 产物落在 build/ 下
```

## 烧录

```bash
# macOS 的串口是 /dev/cu.usbmodem*，Linux 是 /dev/ttyACM*
./tools/flash.sh --bin build/005-signal-log-full.bin
```

脚本会：自动找串口 → 校验 sha256 → 打印"整机刷新会覆盖 NVS"的提示 → 用 esptool
从 **0x0** 写合并镜像 → 提示怎么看串口日志。

日常迭代只想改应用、保留已存设置时：

```bash
./tools/flash.sh --mode app --bin build/005-signal-log.bin   # 只写 0x10000
```

`--erase` 是先整片擦除再写，只有明确要"从零开始"时才用。

## FoloToy 官方的安装入口

FoloToy 的 [安装社区玩法](https://ai-passport.folotoy.cn/guides/flash-official-firmware/)
是浏览器里点「安装」、选设备、自动写入的流程，但它只服务**玩法社区里托管的作品**，
不接受任意 `.bin`。所以自建固件走上面的 esptool；官方页面留着用来一键恢复默认固件
（[恢复出厂固件](https://ai-passport.folotoy.cn/guides/restore-factory-firmware/)，需要设备二维码链接）。

## 第一次上机按这个顺序验收

1. **能亮**：插电/开机后出现顶栏 `|SIGNAL.LOG`，1 秒内变成启动页，不是黑屏也不是白屏。
2. **能连**：Wi-Fi 连上后顶栏短暂 `SYNC..`，随后出现情报列表（至少 1 条）。
   连不上时应当是 `NO SIGNAL` + 具体原因（`Wi-Fi 未连接` / `HTTP 404` / `JSON syntax error` 等）。
3. **中文不乱**：标题、`AI 一句话` 里没有方块（缺字会显示豆腐块，说明字集要扩）。
4. **按键**：▲▼ 移动选择（到头循环）→ OK 进详情 → ▲▼ 滚动 → OK 翻页 → 长按 OK 回列表；
   列表里长按 OK 触发刷新。
5. **时间可信**：详情页时间是 `generated_at` 推算出来的（设备没有 RTC），顶栏时钟每秒走。

## 出问题时

| 现象 | 先看什么 |
| --- | --- |
| 串口无输出 | 数据线是否只供电；是否 `/dev/cu.usbmodem*`；`idf.py monitor` 用的是 USB-Serial-JTAG |
| `NO SIGNAL` + `Wi-Fi 未连接` | `menuconfig` 里的 SSID 必须是 2.4 GHz，密码别带引号 |
| `HTTP 404` | `CONFIG_APP_FEED_URL` 指到了不存在的路径；先用电脑 `curl` 一下同一个 URL |
| `JSON syntax error` | feed 被 CDN 加了 BOM/HTML 错误页；`make_feed.py --check` 复查 |
| 中文显示为方块 | 字体没有该码点：改 `tools/gen_font.py` 的字符集重新生成，见 `docs/fonts.md` |
| 列表一直是 `STALE` | 拉取失败但旧数据还在（这是有意的）；长按 OK 立即重试 |
