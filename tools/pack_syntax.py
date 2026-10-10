#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pack_syntax.py —— 打包 STM32 端「语法检查包」

STM32 的代码在 PC 上没法真正编译（没有 ARM 工具链），但可以用
x86 上的 gcc 做一次「-fsyntax-only」检查：类型、函数原型、变量作用域、
C89 声明位置这些问题都能提前抓出来，比在 Keil 里反复 Build 快得多。

本脚本把 CMSIS 头、标准外设库头、User 下的源码，以及 tools/stub 里的
lcd.h/touch.h 桩文件一起打成 tar.gz；虚拟机拉过去解包后执行
make -f tools/syntax_check.mk 即可。

用法:
    python tools/pack_syntax.py
"""

import io
import os
import sys
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "stm32_syntax.tar.gz")

INCLUDE = [
    "Libraries/CMSIS",
    "Libraries/STM32F10x_StdPeriph_Driver/inc",
    "User",
    "tools/syntax_check.mk",
]

# User/Ui/font16.h 在仓库里是 GBK（给 Keil 用）；语法检查时换成
# linux_can_monitor/font16.h 这份 UTF-8 的同一份字库，
# 避免 GBK 双字节里出现 0x5C（反斜杠）时把 gcc 的注释行吃掉。
EXCLUDE = {"User/Ui/font16.h"}
ALIASES = [("linux_can_monitor/font16.h", "User/Ui/font16.h")]

# 排除编译产物与数据库之类
SKIP_EXT = (".o", ".d", ".crf", ".lst", ".htm", ".map", ".dep", ".lnp",
            ".db", ".bak")


def add_tree(tar, rel):
    path = os.path.join(ROOT, rel.replace("/", os.sep))
    if os.path.isfile(path):
        tar.add(path, arcname=rel)
        return 1
    n = 0
    for base, dirs, files in os.walk(path):
        dirs[:] = [d for d in dirs if d not in (".git", "Objects", "Listings")]
        for f in files:
            if f.lower().endswith(SKIP_EXT):
                continue
            full = os.path.join(base, f)
            arc = os.path.relpath(full, ROOT).replace(os.sep, "/")
            if arc in EXCLUDE:
                continue
            tar.add(full, arcname=arc)
            n += 1
    return n


def main():
    if os.path.exists(OUT):
        os.remove(OUT)

    total = 0
    with tarfile.open(OUT, "w:gz") as tar:
        for rel in INCLUDE:
            total += add_tree(tar, rel)
        for src, dst in ALIASES:
            full = os.path.join(ROOT, src.replace("/", os.sep))
            if os.path.isfile(full):
                tar.add(full, arcname=dst)
                total += 1

    sys.stderr.write("已生成 %s（%d 个文件，%d 字节）\n"
                     % (OUT, total, os.path.getsize(OUT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
