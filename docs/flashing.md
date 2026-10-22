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

每次构建成功还会把同一个镜像发布成固定链接（不需要登录、不会过期）：

```
https://github.com/k-macao/05-folo/releases/download/firmware-latest/005-signal-log-full.bin
https://github.com/k-macao/05-folo/releases/download/firmware-latest/SHA256SUMS.txt
```

**A2. 带自己 Wi-Fi 的私有镜像（推荐日常使用）**

Actions → firmware → **Run workflow**，填 `wifi_ssid` / `feed_url`（可选 `feed_url_fallback`），
密码从仓库 Secret `WIFI_PASSWORD` 自动注入。产物只在本次运行的 **Artifacts** 里，
**不会**发到公开 release——镜像里含 Wi-Fi 密码，别转发。

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

# 想先确认下载到的镜像到底是什么，可以单独跑结构校验
python3 tools/verify_image.py build/005-signal-log-full.bin
```

脚本会：自动找串口 → 校验 sha256 → 打印"整机刷新会覆盖 NVS"的提示 → 用 esptool
从 **0x0** 写合并镜像 → 提示怎么看串口日志。

日常迭代只想改应用、保留已存设置时：

```bash
./tools/flash.sh --mode app --bin build/005-signal-log.bin   # 只写 0x10000
```

`--erase` 是先整片擦除再写，只有明确要"从零开始"时才用。

### Windows 图形工具（Flash Download Tool）

没有 esptool 时也可以用乐鑫官方 `flash_download_tool`，但必须按下面设，
否则最常见的结果就是**烧完黑屏**：

| 设置 | 值 | 设错的后果 |
| --- | --- | --- |
| ChipType / WorkMode | `ESP32-C3` / `Develop` | — |
| SPIDownload 文件行 | **只留一行**、左边复选框**打勾**、`005-signal-log-full.bin` | 不打勾 = 根本没写入 |
| 地址 | **`0x0`**（这是合并镜像，不是单独的 app） | 写 `0x10000` → bootloader 落错位 → 黑屏 |
| SPI MODE / SPEED / FLASH SIZE | `DIO` / `80MHz` / `64Mbit (8MB)`，**或直接勾 DoNotChgBin** | 工具会按界面值改写镜像头；和编译期不一致可能起不来 |
| BAUD | `460800`（115200 也行，只是慢） | — |

点 START 后要等到主窗口变**绿色 FINISH** 才算写完。

⚠ **写完必须整机断电重启**：拔掉 USB、关机，等几秒再开机。这个工具结束时
只打印 `is stub and send flash finish`，不会把 ESP32-C3（原生 USB-Serial-JTAG）
可靠地复位进应用——芯片还停在 ROM 下载模式（日志里的 `waiting for download`
就是它），此时屏幕漆黑是正常的，光按复位键可能又回到下载模式。

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
| 烧完黑屏、无任何显示 | 先**整机断电重启**（见上节）；还黑就抓串口日志（下一行）。注意：开机页在连 Wi-Fi 之前就渲染，所以黑屏和 Wi-Fi/feed 无关，是"固件没跑起来" |
| 黑屏抓日志的方法 | 同一 COM 口开串口监视（115200，Arduino IDE 串口监视器 / PuTTY / `python -m serial.tools.miniterm COMx 115200` 都行），设备重新上电，看第一屏：反复 `waiting for download` = 还在下载模式；`invalid header` 类 = 地址写错或镜像头被工具改过（按 `0x0` + DoNotChgBin 重烧）；`Guru Meditation` / 反复重启 = 固件崩溃，把整段日志贴回来；日志正常出现 `005 signal log 启动` 但屏不亮 = 显示初始化问题，同样贴日志 |
| 串口无输出 | 数据线是否只供电；是否 `/dev/cu.usbmodem*`；`idf.py monitor` 用的是 USB-Serial-JTAG |
| `NO SIGNAL` + `Wi-Fi 未连接` | `menuconfig` 里的 SSID 必须是 2.4 GHz，密码别带引号 |
| `HTTP 404` | `CONFIG_APP_FEED_URL` 指到了不存在的路径；先用电脑 `curl` 一下同一个 URL |
| `JSON syntax error` | feed 被 CDN 加了 BOM/HTML 错误页；`make_feed.py --check` 复查 |
| 中文显示为方块 | 字体没有该码点：改 `tools/gen_font.py` 的字符集重新生成，见 `docs/fonts.md` |
| 列表一直是 `STALE` | 拉取失败但旧数据还在（这是有意的）；长按 OK 立即重试 |
