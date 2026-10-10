#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
to_gbk.py —— STM32 侧源码的编码/换行统一工具

为什么要这么做：
  Keil MDK 工程里原有的 .c/.h 一直是 GBK 编码（中文注释在 Keil 里显示正常）。
  新加的界面文件最初是按 UTF-8 写的，如果不转换，在 Keil 里中文注释会是乱码，
  严重时还会因为多字节序列被误解析而报语法错误。

  同时统一成 CRLF 换行，避免工程里出现「一部分 LF、一部分 CRLF」的混乱。

用法:
    python tools/to_gbk.py              UTF-8 -> GBK/CRLF（交付给 Keil 前的最后一步）
    python tools/to_gbk.py --check      只检查当前编码，不改动
    python tools/to_gbk.py --to-utf8    GBK -> UTF-8，便于用普通编辑器改中文

  典型工作流：要改这些文件时先 --to-utf8，改完再跑一次默认方向转回 GBK。

  注意：这些文件一旦是 GBK，普通的 UTF-8 文本工具（包括本项目的 edit/read 工具）
  就会读不了；所以「改完必须转回来」这一步不要忘。
"""

import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TARGETS = [
    "User/Ui/ui_app.c",
    "User/Ui/ui_app.h",
    "User/Ui/ui_port.c",
    "User/Ui/ui_port.h",
    "User/Ui/font16.h",
    # LCD / 触摸驱动（Keil 工程同样是 GBK）
    "User/Lcd/lcd.c",
    "User/Lcd/lcd.h",
    "User/Lcd/ili93xx.c",
    "User/Lcd/touch.c",
    "User/Lcd/touch.h",
    "User/Lcd/ctiic.c",
    "User/Lcd/ctiic.h",
]


def decode(raw, rel):
    """返回 (文本, 编码名) 或 (None, None)"""
    if raw.startswith(b"\xef\xbb\xbf"):
        raw_nobom = raw[3:]
    else:
        raw_nobom = raw
    for enc in ("gbk", "utf-8"):
        try:
            return raw_nobom.decode(enc), ("GBK" if enc == "gbk" else "UTF-8")
        except UnicodeDecodeError:
            continue
    return None, None


def main():
    check_only = "--check" in sys.argv
    to_utf8 = "--to-utf8" in sys.argv
    rc = 0

    print("方向: %s" % ("GBK -> UTF-8" if to_utf8 else "UTF-8 -> GBK"))

    for rel in TARGETS:
        path = os.path.join(ROOT, rel.replace("/", os.sep))
        if not os.path.isfile(path):
            print("%-24s 缺失，跳过" % rel)
            continue

        raw = open(path, "rb").read()
        bom = raw.startswith(b"\xef\xbb\xbf")
        if bom:
            print("%-24s 有 UTF-8 BOM（Keil 会报错，必须去掉）" % rel)
            rc = 1

        text, enc = decode(raw, rel)
        if text is None:
            print("%-24s 编码未知，跳过" % rel)
            rc = 1
            continue

        crlf = text.count("\r\n")
        lf_only = text.count("\n") - crlf
        note = "" if lf_only == 0 else "  <-- 含 %d 个纯 LF" % lf_only

        if check_only:
            print("%-24s %-6s CRLF=%-4d LF=%d%s" % (rel, enc, crlf, lf_only, note))
            if enc != "GBK" or lf_only:
                rc = 1
            continue

        want_enc = "UTF-8" if to_utf8 else "GBK"
        if enc == want_enc and lf_only == 0 and not bom:
            print("%-24s 已是 %s/CRLF，无需改动" % (rel, want_enc))
            continue

        # 统一成 CRLF 后再按目标编码写回
        text = text.replace("\r\n", "\n").replace("\n", "\r\n")
        try:
            data = text.encode("utf-8" if to_utf8 else "gbk")
        except UnicodeEncodeError as exc:
            # 注意：这条提示本身只能用 ASCII —— 否则在 GBK 控制台上
            # 打印它自己就会再抛一次 UnicodeEncodeError（踩过一次）。
            print("%-24s 有字符无法用 %s 表示: %s" % (rel, want_enc, exc))
            print("     ^ 这个字符必须换掉（Keil 的 GBK 源码装不下它）。")
            print("       常见元凶: 双向箭头 U+2194 、对勾 U+2713 、星号 U+2605 、")
            print("       带圈数字 U+2460.. 、数学符号等。换成 ASCII 或常用汉字即可。")
            rc = 1
            continue

        open(path, "wb").write(data)
        print("%-24s %s -> %s/CRLF  %d -> %d 字节%s"
              % (rel, enc, want_enc, len(raw), len(data), note))

    return rc


if __name__ == "__main__":
    sys.exit(main())
