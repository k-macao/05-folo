# 位图字体资产（中文上屏）

设备上没有 PSRAM，8 MB Flash 也不可能装下一整套 CJK 字体，所以本应用只嵌入**用得到的
码点**：`main/fonts/crt_font_12.c`（4 bpp，4346 个码点，约 309 KiB 位图）。

## 字面与许可

| 项 | 值 |
| --- | --- |
| 字体 | Noto Sans Mono CJK SC Regular |
| 授权 | SIL Open Font License 1.1（允许子集化与嵌入，保留署名） |
| 下载 | <https://github.com/notofonts/noto-cjk>（`Sans/Mono/NotoSansMonoCJKsc-Regular.otf`） |
| 仓库策略 | **不提交源字体文件**（16 MB，且无必要），只提交生成物与本文档 |

选等宽 CJK 字面而不是普通 Noto Sans SC，是为了让"半角 6 px / 全角 12 px"成立：
所有排版都按格计算（列表每行 38 格、详情每行 36 格），UI、预览、固件三处才能用同一套换行规则。

## 码点清单

- ASCII `U+0020`–`U+007E`（95 个，连续的 `FORMAT0_TINY` cmap）
- GB2312 一级汉字（3755 个，rows `B0`–`D7`）
- GB2312 符号区（rows `A1`–`A9` 中的标点/全角字符）
- 手工补充：箭头/几何符号（`▲▼◀▶●○■□★☆`）、`①②③`、`℃ ％ ￥`、`… —` 等

GB2312 的希腊字母、拼音声调字母、Latin-1 字母被排除：它们在等宽字面里不是 6/12 px，
会破坏格宽假设；`intel_normalize()` 会把运行时出现的这类字符替换成 ASCII 或 `?`。

字体里没有、但内容可能出现的四个字符（`— ° ′ ✓`）在 `main/intel_logic.c:intel_normalize()`
里被映射成 `- o ' v`，所以永远不会显示成方块。`tools/check.sh` 会校验这一点。

## 重新生成

```bash
# 1) 取源字体（不提交）
curl -L -o /tmp/NotoSansMonoCJKsc-Regular.otf \
  https://github.com/notofonts/noto-cjk/raw/main/Sans/Mono/NotoSansMonoCJKsc-Regular.otf   # 16 MB

# 2) 生成 + 自校验
python3 tools/gen_font.py --font /tmp/NotoSansMonoCJKsc-Regular.otf \
    --size 12 --name crt_font_12 --mono --out main/fonts/crt_font_12.c
```

输出三个文件：`crt_font_12.c`（数据 + 字体描述符）、`crt_font_12.h`、`crt_font_12.chars`
（实际嵌入的码点清单，用来 diff 与检查）。

## 为什么自己写生成器

`lv_font_conv`（LVGL 官方工具）最后一次发布是 2021 年，在 Node 24 下对同一个 OTF、同一组参数
产出的位图与 LVGL 自带的 Montserrat 解码结果不一致（可用 PLAIN 解码器复现乱码）。因此
`tools/gen_font.py`：

1. 用 Pillow/FreeType 栅格化，4 bpp 量化；
2. 直接输出 LVGL 9 的 `lv_font_fmt_txt_glyph_dsc_t` / `lv_font_fmt_txt_cmap_t` / `lv_font_fmt_txt_dsc_t`；
3. **重新解析刚写出的 C 文件**，按 `lv_font_get_bitmap_fmt_txt()` 的 PLAIN 4 bpp 规则解码每个字形，
   与栅格化结果逐像素比对，不一致直接失败退出。

约定（改生成器时必须保持）：

- **位图打包**：PLAIN、4 bpp、高 nibble 在左，行间连续不补齐（`stride = 0`），每个字形从字节边界开始。
- **cmap**：ASCII 用一条 `FORMAT0_TINY`；其余码点用一条 `SPARSE_TINY`（`unicode_list` 存
  `cp - range_start`，`glyph_id = glyph_id_start + index`）。
- **行高**：12 px 字号的行高被裁到 15 px、`base_line = 4`，与 UI 的 15 px 行网格严格对齐；
  生成器会拒绝超出该行高的字形。
- **字宽**：`--mono` 断言每个字形 advance 只能是 96（6 px）或 192（12 px），否则丢弃并打印。

## Flash 账

| 内容 | 大小 |
| --- | --- |
| 字体位图 | ~309 KiB |
| 字形描述符 + cmap | ~36 KiB |
| 生成的 `.c` 源码 | 2.4 MiB（编译进 rodata，运行时零 RAM） |

app 分区 0x7f0000（约 8 MB），当前应用连同 BSP + LVGL 约占 2 MB，余量充足。
如果要扩字集，优先按 005 的实际语料挑选码点，而不是直接塞进 GB2312 二级字（6763 字，位图翻倍）。
