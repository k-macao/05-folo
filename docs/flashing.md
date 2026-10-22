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

### 方式一：Windows 图形界面（Espressif Flash Download Tool）

1. **先解压 `.zip`**：从 Actions → Artifacts 下载的 `005-signal-log-full.zip` **必须先右键解压**，
   取出里面的 **`005-signal-log-full.bin`**（约 1.9 MB，千万别选 `.zip` 本身）。
2. 打开 Flash Download Tool，芯片选 **`ESP32-C3`**，工作模式选 `Develop`，连接方式选 `UART`（或 `USB`）。
3. 在 `SPIDownload` 页第一行：
   - 点 `...` 选中解压出来的 **`005-signal-log-full.bin`**
   - 右侧地址填 **`0x0`**
   - **最关键一步：勾选该行最左边的方框 `☑`** —— 勾上后整行（路径框与 `0x0` 框）会变成**亮绿色**！
     **如果没勾 `☑`（框是灰色），点 `START` 只会读一次芯片 MAC / Flash ID 就秒显 `FINISH 完成`，实际上 1 个字节都没写进去！**
4. 下方参数确认：
   - `SPI SPEED`: `40MHz`
   - `SPI MODE`: `DIO`
   - **`☑ DoNotChgBin` 保持勾选**（直接使用 ESP-IDF 生成的 8 MB 镜像头，不要让工具改 bin）
   - `COM`: 设备对应的串口（如 `COM3`）
   - `BAUD`: 建议改选 **`921600`** 或 **`460800`**（`115200` 写 1.9 MB 要两分半钟，`921600` 只需几秒；真正写入时进度条会一格格推进，后台黑框会打印 `Writing at 0x00000000... (10 %) ... (100 %)`）
5. 点 **`START`**，等待进度条走完变成 `FINISH 完成`，拔线重插或按电源键重开机即可。

### 方式二：命令行（macOS / Linux / Windows esptool）

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
| 开机屏幕白灯一闪一闪 / 黑屏不进界面 | ① Flash Download Tool 第一行最左侧 **`☑` 没勾上**（整行灰色而非亮绿）就点了 `START`（若之前点过 `ERASE`，空 Flash 会让 ESP32-C3 ROM 引导程序不断往 UART0 TX = `GPIO21` 背光脚吐报错并每隔约 3 秒看门狗重启，表现为屏幕背光周期性闪烁）；② 误把未解压的 `.zip` 当 `.bin` 烧进了 `0x0`；③ 未勾 `DoNotChgBin` 改坏了镜像头 |
| 串口无输出 | 数据线是否只供电；是否 `/dev/cu.usbmodem*`；`idf.py monitor` 用的是 USB-Serial-JTAG |
| `NO SIGNAL` + `Wi-Fi 未连接` | `menuconfig` 里的 SSID 必须是 2.4 GHz，密码别带引号 |
| `HTTP 404` | `CONFIG_APP_FEED_URL` 指到了不存在的路径；先用电脑 `curl` 一下同一个 URL |
| `JSON syntax error` | feed 被 CDN 加了 BOM/HTML 错误页；`make_feed.py --check` 复查 |
| 中文显示为方块 | 字体没有该码点：改 `tools/gen_font.py` 的字符集重新生成，见 `docs/fonts.md` |
| 列表一直是 `STALE` | 拉取失败但旧数据还在（这是有意的）；长按 OK 立即重试 |
