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

echo "== feed tool =="
python3 tools/make_feed.py --in preview/feed.json --check

echo "== font inventory =="
python3 - <<'PY'
import pathlib, sys
sys.path.insert(0, "tools")
import gen_font

chars = set(pathlib.Path("main/fonts/crt_font_12.chars").read_text(encoding="utf-8"))
missing = [c for c in gen_font.EXTRA
           if c not in chars and c not in gen_font.FALLBACK_MAPPED]
if missing:
    raise SystemExit(f"font lacks code points used by the UI: {missing}")
print(f"fallback-mapped (normalize to ASCII): {''.join(sorted(gen_font.FALLBACK_MAPPED))}")
if not all(chr(c) in chars for c in range(0x20, 0x7f)):
    raise SystemExit("font lacks part of ASCII")
print(f"font: {len(chars)} code points, UI symbols all covered")
PY

echo "all host checks passed"
