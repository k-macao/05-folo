#!/usr/bin/env bash
# 主机侧校验：不需要 ESP-IDF，gcc 即可。CI 和本地都用这一条。
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=tests/build
mkdir -p "$OUT"

echo "== host tests =="
gcc -std=c11 -Wall -Wextra -Werror -Imain \
    -o "$OUT/test_logic" tests/test_logic.c main/intel_logic.c
"$OUT/test_logic"

gcc -std=c11 -Wall -Wextra -Werror -Imain -Itests/third_party/cJSON \
    -o "$OUT/test_feed" tests/test_feed.c main/intel_feed.c main/intel_logic.c \
    tests/third_party/cJSON/cJSON.c -lm
"$OUT/test_feed" preview/feed.json

echo "== rounding safe-area check =="
gcc -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
    -x c - components/bsp/src/bsp_display_rounding.c -o "$OUT/test_rounding" <<'EOF'
#include "bsp_display_rounding.h"
#include <stdio.h>
int main(void) {
    /* 顶栏 y=7..21 与底栏 y=299..313 在左右各留 18 px 时不得被 R=30 圆角遮罩裁切 */
    const int ys[] = { 7, 21, 299, 313 };
    for (int i = 0; i < 4; i++) {
        for (int x = 18; x <= 240 - 18 - 1; x++) {
            if (bsp_display_pixel_outside_rounded_rect(x, ys[i], 240, 320, 30)) {
                printf("FAIL clipped at (%d, %d)\n", x, ys[i]);
                return 1;
            }
        }
    }
    printf("test_rounding: top/bottom safe area clear of R=30 mask\n");
    return 0;
}
EOF
"$OUT/test_rounding"

echo "== feed tool =="
python3 tools/make_feed.py --in preview/feed.json --check

echo "== font inventory & LVGL cmap 1:1 lookup =="
python3 - <<'PY'
import pathlib, re, sys
sys.path.insert(0, "tools")
import gen_font

chars_text = pathlib.Path("main/fonts/crt_font_12.chars").read_text(encoding="utf-8")
c_text = pathlib.Path("main/fonts/crt_font_12.c").read_text(encoding="utf-8")
cps = [ord(c) for c in chars_text]
chars = set(chars_text)

gen_font.verify_c_font(c_text, cps)

missing = [c for c in gen_font.EXTRA
           if c not in chars and c not in gen_font.FALLBACK_MAPPED]
if missing:
    raise SystemExit(f"font lacks code points used by the UI: {missing}")
if not all(chr(c) in chars for c in range(0x20, 0x7f)):
    raise SystemExit("font lacks part of ASCII")

# 扫描 main/*.c 中的 UI 字符串字面量（排除日志宏 ESP_LOG*），确保每个非 ASCII 字符都在字库中。
ui_missing = set()
for src in sorted(pathlib.Path("main").glob("*.c")):
    for line in src.read_text(encoding="utf-8").splitlines():
        if "ESP_LOG" in line:
            continue
        for lit in re.findall(r'"((?:\\.|[^"\\])*)"', line):
            for ch in lit:
                if ord(ch) >= 0x80 and ch not in chars and ch not in gen_font.FALLBACK_MAPPED:
                    ui_missing.add((src.name, ch, f"U+{ord(ch):04X}"))
if ui_missing:
    raise SystemExit(f"UI strings contain glyphs absent from crt_font_12: {sorted(ui_missing)}")

print(f"fallback-mapped (normalize to ASCII): {''.join(sorted(gen_font.FALLBACK_MAPPED))}")
print(f"font: {len(chars)} code points, LVGL cmaps 1:1 verified, all UI strings covered")
PY

echo "all host checks passed"
