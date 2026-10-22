#!/usr/bin/env bash
# tools/flash.sh —— 把镜像写进 AI Passport（ESP32-C3 原生 USB，/dev/ttyACM* 或 /dev/cu.usbmodem*）。
#
#   ./tools/flash.sh                       # 默认：合并镜像从 0x0 写（整机刷新）
#   ./tools/flash.sh --mode app            # 只写 app 到 0x10000，保留 NVS（正常迭代）
#   ./tools/flash.sh --bin build/xxx.bin --port /dev/ttyACM1 --yes
#   ./tools/flash.sh --erase               # 先整片擦除再写（数据全丢，需显式要求）
#
# FoloToy 的约定（docs/development/engineering/firmware-layout.md）：
#   * 烧录前不需要备份出厂固件，也不要求先读出整片 Flash；
#   * 合并镜像从 0x0 写会连 NVS/PHY 一起重写，可能清掉已存数据；
#   * 日常增量开发用分段写（--mode app）或 idf.py flash，避免动数据区。
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="build/005-signal-log-full.bin"
PORT=""
MODE="full"
ASSUME_YES=0
ERASE=0
BAUD="${BAUD:-460800}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bin) BIN="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --mode) MODE="$2"; shift 2 ;;
        --baud) BAUD="$2"; shift 2 ;;
        --yes|-y) ASSUME_YES=1; shift ;;
        --erase) ERASE=1; shift ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$MODE" in
    full|app) ;;
    *) echo "--mode must be full or app" >&2; exit 2 ;;
esac

# ---------------------------------------------------------------- 找设备
detect_port() {
    local candidates=()
    shopt -s nullglob
    candidates+=(/dev/ttyACM* /dev/cu.usbmodem*)
    shopt -u nullglob
    if [[ ${#candidates[@]} -eq 1 ]]; then echo "${candidates[0]}"; return 0; fi
    if [[ ${#candidates[@]} -gt 1 ]]; then
        echo "multiple serial devices found: ${candidates[*]}" >&2
        return 1
    fi
    return 1
}

if [[ -z "$PORT" ]]; then
    if ! PORT="$(detect_port)"; then
        cat >&2 <<'EOF'
没有找到设备串口（/dev/ttyACM* 或 /dev/cu.usbmodem*）。

  1. 把 AI Passport 开机；
  2. 用【能传数据】的 Type-C 线接到电脑（有些线只供电）；
  3. macOS 用 /dev/cu.usbmodem*，Linux 用 /dev/ttyACM*（可能需要 dialout 组权限）；
  4. 找不到也可以用 --port 指定。
EOF
        exit 1
    fi
fi

[[ -f "$BIN" ]] || { echo "找不到镜像 $BIN（先跑 GitHub Actions 的 firmware 工作流并下载产物）" >&2; exit 1; }

# ---------------------------------------------------------------- 校验
SUMS="$(dirname "$BIN")/SHA256SUMS.txt"
if [[ -f "$SUMS" ]]; then
    echo "== 校验 sha256"
    expected=$(awk -v want="$(basename "$BIN")" 'index($0, want) { print $1; exit }' "$SUMS" 2>/dev/null || true)
    if [[ -n "$expected" ]]; then
        if command -v sha256sum >/dev/null 2>&1; then
            actual=$(sha256sum "$BIN" | awk '{print $1}')
        else
            actual=$(shasum -a 256 "$BIN" | awk '{print $1}')
        fi
        if [[ "$expected" != "$actual" ]]; then
            echo "sha256 不匹配：期望 $expected，实际 $actual" >&2
            echo "镜像可能没下载完整，重新下载再试。" >&2
            exit 1
        fi
        echo "sha256 OK ($actual)"
    else
        echo "SHA256SUMS.txt 里没有 $(basename "$BIN")，跳过校验"
    fi
fi

# ---------------------------------------------------------------- 提示
SIZE=$(wc -c <"$BIN" | tr -d ' ')
echo "镜像 : $BIN  ($((SIZE / 1024)) KiB)"
echo "端口 : $PORT"
if [[ "$MODE" == "full" ]]; then
    echo "方式 : 合并镜像 → 0x0（整机刷新，会覆盖 NVS/PHY，已存数据可能丢失）"
else
    echo "方式 : app → 0x10000（保留 NVS 数据区）"
fi
[[ $ERASE -eq 1 ]] && echo "注意 : --erase 会先整片擦除，设备数据全部丢失"

if [[ $ASSUME_YES -eq 0 ]]; then
    read -r -p "继续？[y/N] " reply
    [[ "$reply" =~ ^[Yy]$ ]] || { echo "已取消"; exit 1; }
fi

# ---------------------------------------------------------------- 烧录
ESPTOOL="${ESPTOOL:-esptool.py}"
if ! command -v "$ESPTOOL" >/dev/null 2>&1; then
    if command -v esptool >/dev/null 2>&1; then ESPTOOL=esptool
    elif [[ -n "${IDF_PATH:-}" && -x "$IDF_PATH/components/esptool_py/esptool/esptool.py" ]]; then
        ESPTOOL="$IDF_PATH/components/esptool_py/esptool/esptool.py"
    else
        echo "没找到 esptool。装一个：python3 -m pip install esptool" >&2
        exit 1
    fi
fi

if [[ $ERASE -eq 1 ]]; then
    "$ESPTOOL" --chip esp32c3 -p "$PORT" erase_flash
fi

if [[ "$MODE" == "full" ]]; then
    "$ESPTOOL" --chip esp32c3 -p "$PORT" -b "$BAUD" \
        --before default_reset --after hard_reset \
        write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
        0x0 "$BIN"
else
    "$ESPTOOL" --chip esp32c3 -p "$PORT" -b "$BAUD" \
        --before default_reset --after hard_reset \
        write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
        0x10000 "$BIN"
fi

cat <<EOF

写入完成，设备已重启。
看日志（可选）：  idf.py -p $PORT monitor      # 或  python3 -m serial.tools.miniterm $PORT 115200
预期：屏幕出现 |SIGNAL.LOG → Wi-Fi 连上后变成情报列表；连不上会显示 NO SIGNAL + 原因。
EOF
