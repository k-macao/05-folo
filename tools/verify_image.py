#!/usr/bin/env python3
"""校验合并镜像（build/005-signal-log-full.bin）的结构，打印可读报告。

只读本地文件，不需要 esptool / 设备：

  * 0x0 处的引导程序镜像头（magic 0xE9、芯片型号、段数、是否附 SHA256）
  * 0x8000 处的分区表：magic、每条分区（label/type/subtype/offset/size）+ MD5
  * app 分区开头的固件描述符（项目名、版本、编译时间、IDF 版本）
  * 文件大小是否落在 8 MB Flash 内、镜像区间是否重叠

CI 用它给出"这个 bin 到底是什么"的证据；本地下载后也可以跑：

    python3 tools/verify_image.py build/005-signal-log-full.bin
"""

from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

FLASH_SIZE = 8 * 1024 * 1024
PART_TABLE_OFFSET = 0x8000
PART_MAGIC = 0x50AA
PART_MAGIC_MD5 = 0xEBEB
APP_DESC_MAGIC = 0xABCD5432
IMAGE_MAGIC = 0xE9
CHIPS = {0: "ESP32", 2: "ESP32-S2", 5: "ESP32-C3", 9: "ESP32-S3", 12: "ESP32-C2",
         13: "ESP32-C6", 16: "ESP32-H2", 18: "ESP32-P4"}
TYPES = {0: "app", 1: "data"}
APP_SUBTYPES = {0: "factory", 0x10: "ota_0", 0x11: "ota_1"}
DATA_SUBTYPES = {1: "phy", 2: "nvs", 3: "coredump", 4: "nvs_keys", 0x81: "fat", 0x82: "spiffs"}

errors: list[str] = []


def fail(msg: str) -> None:
    errors.append(msg)


def parse_image_header(blob: bytes, offset: int) -> dict:
    if len(blob) < offset + 24:
        fail(f"0x{offset:X}: 文件比镜像头还短")
        return {}
    magic, segments, spi_mode, spi_conf = blob[offset], blob[offset + 1], blob[offset + 2], blob[offset + 3]
    chip_id = struct.unpack_from("<H", blob, offset + 12)[0]
    hash_appended = blob[offset + 23]
    return {
        "magic": magic,
        "segments": segments,
        "spi_mode": spi_mode,
        "spi_speed_size": spi_conf,
        "chip_id": chip_id,
        "hash_appended": hash_appended,
    }


def parse_partitions(blob: bytes) -> tuple[list[dict], bool | None]:
    table = blob[PART_TABLE_OFFSET:PART_TABLE_OFFSET + 95 * 32 + 16]
    entries = []
    md5_offset = None
    for i in range(95):
        chunk = table[i * 32:(i + 1) * 32]
        if len(chunk) < 32:
            break
        magic = struct.unpack_from("<H", chunk, 0)[0]
        if magic == PART_MAGIC_MD5:
            md5_offset = i * 32
            break
        if magic != PART_MAGIC:
            if any(chunk):
                fail(f"分区表第 {i} 项 magic=0x{magic:04X}，既不是 0x{PART_MAGIC:04X} 也不是结束标记")
            md5_offset = i * 32
            break
        ptype, subtype, offset, size = struct.unpack_from("<BBII", chunk, 2)
        label = chunk[12:28].split(b"\x00")[0].decode("ascii", "replace")
        entries.append({
            "label": label, "type": ptype, "subtype": subtype,
            "offset": offset, "size": size,
        })

    if md5_offset is None:
        fail("分区表没有结束标记")
        return entries, None

    # 结束块 = 16 字节 magic（EB EB + FF*14）+ 16 字节 MD5；MD5 覆盖它之前的所有条目。
    stored = table[md5_offset + 16:md5_offset + 32]
    computed = hashlib.md5(table[:md5_offset]).digest()
    md5_ok = stored == computed
    if not md5_ok:
        fail("分区表 MD5 不匹配（分区表被改写或镜像拼接错位）")
    return entries, md5_ok


def parse_app_desc(blob: bytes, app_offset: int) -> dict | None:
    desc_offset = app_offset + 0x20
    if len(blob) < desc_offset + 0x100:
        fail("app 描述符超出文件范围")
        return None
    magic = struct.unpack_from("<I", blob, desc_offset)[0]
    if magic != APP_DESC_MAGIC:
        fail(f"0x{desc_offset:X} 处没有 app 描述符（magic=0x{magic:08X}）")
        return None

    def text(at: int, n: int) -> str:
        return blob[desc_offset + at: desc_offset + at + n].split(b"\x00")[0].decode("utf-8", "replace")

    return {
        "version": text(16, 32),
        "project": text(48, 32),
        "time": text(80, 16),
        "date": text(96, 16),
        "idf_ver": text(112, 32),
    }


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "build/005-signal-log-full.bin")
    if not path.is_file():
        print(f"找不到 {path}")
        return 2
    blob = path.read_bytes()
    print(f"file       : {path} ({len(blob)} bytes / 0x{len(blob):X})")

    if len(blob) > FLASH_SIZE:
        fail(f"文件 {len(blob)} 字节超过 8 MB Flash")

    # --- bootloader @ 0x0
    boot = parse_image_header(blob, 0)
    if boot:
        chip = CHIPS.get(boot["chip_id"], f"unknown({boot['chip_id']})")
        ok = boot["magic"] == IMAGE_MAGIC
        print(f"bootloader : magic=0x{boot['magic']:02X} {'OK' if ok else 'BAD'} "
              f"chip={chip} segments={boot['segments']} "
              f"sha256_appended={'yes' if boot['hash_appended'] else 'no'}")
        if not ok:
            fail("0x0 处不是 ESP 镜像（magic 应为 0xE9）")
        if boot["chip_id"] != 5:
            fail(f"镜像芯片为 {chip}，AI Passport 是 ESP32-C3")

    # --- partition table @ 0x8000
    parts, md5_ok = parse_partitions(blob)
    print(f"partitions : @0x{PART_TABLE_OFFSET:X} "
          f"{'MD5 OK' if md5_ok else 'MD5 BAD'} {len(parts)} entries")
    for p in parts:
        kind = TYPES.get(p["type"], str(p["type"]))
        if p["type"] == 0:
            sub = APP_SUBTYPES.get(p["subtype"], f"0x{p['subtype']:02X}")
        else:
            sub = DATA_SUBTYPES.get(p["subtype"], f"0x{p['subtype']:02X}")
        end = p["offset"] + p["size"]
        in_flash = end <= FLASH_SIZE
        print(f"  {p['label']:<10} {kind:>4}/{sub:<8} 0x{p['offset']:06X} "
              f"size 0x{p['size']:06X} end 0x{end:06X}{'' if in_flash else '  <-- 超出 Flash'}")
        if not in_flash:
            fail(f"分区 {p['label']} 超出 8 MB Flash")

    # 分区不重叠
    ordered = sorted(parts, key=lambda p: p["offset"])
    for a, b in zip(ordered, ordered[1:]):
        if a["offset"] + a["size"] > b["offset"]:
            fail(f"分区 {a['label']} 与 {b['label']} 重叠")

    # --- app @ 它的分区偏移
    apps = [p for p in parts if p["type"] == 0]
    for app in apps:
        hdr = parse_image_header(blob, app["offset"])
        if not hdr:
            continue
        print(f"app        : @0x{app['offset']:X} magic=0x{hdr['magic']:02X} "
              f"chip={CHIPS.get(hdr['chip_id'], hdr['chip_id'])} "
              f"segments={hdr['segments']}")
        if hdr["magic"] != IMAGE_MAGIC:
            fail(f"0x{app['offset']:X} 处不是 ESP 镜像")
        desc = parse_app_desc(blob, app["offset"])
        if desc:
            print(f"  project  : {desc['project']}")
            print(f"  version  : {desc['version']}")
            print(f"  built    : {desc['date']} {desc['time']}")
            print(f"  idf      : {desc['idf_ver']}")

    if not apps:
        fail("分区表里没有 app 分区")

    print()
    if errors:
        print("RESULT: FAIL")
        for e in errors:
            print(f"  - {e}")
        return 1
    print("RESULT: PASS  (镜像结构正确，可以按 docs/flashing.md 从 0x0 写入)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
