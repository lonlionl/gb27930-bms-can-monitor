#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
lint_c.py —— 项目自用的轻量 C 静态检查（不需要编译器）

为什么需要它
-----------
  本工程的「编译验证」是在 Ubuntu 虚拟机上用 gcc 做的。但如果虚拟机不在手边
  （或者只是想快速自查），改完代码就没有任何保护。这个脚本用纯 Python 做几件
  最容易被漏掉、又最容易被编译器抓到的事：

    1. 括号配对（() [] {}）—— 多写/少写一个花括号是最常见的手误；
    2. #include "xxx.h" 能否在给定的头文件搜索路径里找到；
    3. 项目内部函数（按前缀识别）有没有「调用了但没人定义/声明」的情况；
       —— 这类问题在小改动里最常出现（改个名字忘了改调用点）；
    4. 定义了却没被任何地方用到的 static 函数（通常是改名后的残留）。

  它**不能**替代真正的编译：类型不匹配、参数个数不对、C89 声明位置这些
  只有编译器才看得准。定位是「动手改完先自检一遍」，正式的验证仍然是
  tools/pack_syntax.py + 虚拟机上的 make -f tools/syntax_check.mk。

用法:
    python tools/lint_c.py User
    python tools/lint_c.py linux_can_monitor
    python tools/lint_c.py User linux_can_monitor
"""

import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 认定属于「本项目内部函数」的前缀（按这些前缀过滤，避免把 libc / 标准库
# 的成千上万个函数都算进来产生噪音）
PROJECT_PREFIXES = (
    "uip_", "UI_", "LCD_", "ILI93xx_", "TP_", "CT_IIC_", "ct_",
    "BMS_", "CAN_", "Sim_", "gb27930_", "GB_",
    "storage_", "rb_", "isotp_", "iso_",
    "can_layer_", "can_set_", "can_query_", "can_state_", "can_err_",
    "gui_", "fb_", "ui_", "uitouch_", "uitest_",
)

C_KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "do", "else",
    "defined", "case", "goto", "break", "continue",
}


def read_text(path):
    raw = open(path, "rb").read()
    for enc in ("utf-8-sig", "gbk", "latin-1"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return ""


def strip_comments_strings(text):
    """把注释和字符串字面量替换成等长空白，保证括号计数不受它们影响"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
        elif c == "/" and nxt == "*":
            out.append("  ")
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
        elif c == '"' or c == "'":
            quote = c
            out.append(" ")
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    out.append(" ")
                    i += 1
                    if i < n:
                        out.append(" ")
                        i += 1
                    continue
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append(" ")
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def project_files(dirs):
    res = []
    for d in dirs:
        base = os.path.join(ROOT, d.replace("/", os.sep))
        for root, _dirs, files in os.walk(base):
            for f in files:
                if f.lower().endswith((".c", ".h")):
                    res.append(os.path.join(root, f))
    return sorted(res)


def strip_prefix(path):
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def check_balance(path, text, problems):
    pairs = {")": "(", "]": "[", "}": "{"}
    stack = []
    line = 1
    for ch in text:
        if ch == "\n":
            line += 1
        elif ch in "([{":
            stack.append((ch, line))
        elif ch in ")]}":
            if not stack:
                problems.append("%s:%d 多出一个 '%s'" % (strip_prefix(path), line, ch))
                return
            top, top_line = stack.pop()
            if top != pairs[ch]:
                problems.append("%s:%d '%s' 与第 %d 行的 '%s' 不匹配"
                                % (strip_prefix(path), line, ch, top_line, top))
                return
    if stack:
        ch, ln = stack[-1]
        problems.append("%s:%d '%s' 没有对应的闭合符号" % (strip_prefix(path), ln, ch))


def check_includes(path, text, search_dirs, problems):
    for m in re.finditer(r'#\s*include\s+"([^"]+)"', text):
        inc = m.group(1)
        found = False
        for d in search_dirs:
            cand = os.path.join(ROOT, d.replace("/", os.sep), inc.replace("/", os.sep))
            if os.path.isfile(cand):
                found = True
                break
        if not found:
            problems.append("%s 找不到 #include \"%s\"（搜索路径：%s）"
                            % (strip_prefix(path), inc, ", ".join(search_dirs)))


DEF_RE = re.compile(
    r"^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*\{", re.M)
DECL_RE = re.compile(
    r"^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*;", re.M)
CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")

# 函数式宏：#define LCD_WR_REG(reg) ...
# 这类名字在调用点上长得和函数一模一样，必须单独收进来，
# 否则会被误报成「调用了但没有定义」。
MACRO_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s*\(", re.M)


def main():
    dirs = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not dirs:
        dirs = ["User", "linux_can_monitor"]

    files = project_files(dirs)
    if not files:
        sys.stderr.write("没找到任何 .c/.h 文件\n")
        return 2

    # 只用于「收集声明」的第三方头文件目录（不参与括号检查）：
    # 标准外设库里的 CAN_Init / GPIO_Init 等都在这里声明。
    lib_dirs = [
        "Libraries/CMSIS",
        "Libraries/STM32F10x_StdPeriph_Driver/inc",
    ]
    lib_files = []
    for d in lib_dirs:
        base = os.path.join(ROOT, d.replace("/", os.sep))
        if os.path.isdir(base):
            for f in sorted(os.listdir(base)):
                if f.lower().endswith(".h"):
                    lib_files.append(os.path.join(base, f))

    # 头文件搜索路径（按项目实际布局给出）
    search_dirs = [
        "User", "User/Can", "User/Bms", "User/Led", "User/Usart",
        "User/Ui", "User/Lcd",
        "linux_can_monitor", "tools/pc_ui",
    ]

    problems = []
    defined = {}        # 名字 -> 文件
    static_defs = []    # (名字, 文件)
    declared = set()
    called = {}         # 名字 -> 文件集合

    # ---- 先收第三方头文件里的声明（CAN_Init / GPIO_Init …） ----
    for path in lib_files:
        text = strip_comments_strings(read_text(path))
        for m in DECL_RE.finditer(text):
            declared.add(m.group(1))
        for m in MACRO_RE.finditer(text):
            declared.add(m.group(1))

    for path in files:
        text = strip_comments_strings(read_text(path))
        check_balance(path, text, problems)
        check_includes(path, text, search_dirs, problems)

        # 函数式宏也算「有定义」
        for m in MACRO_RE.finditer(text):
            defined.setdefault(m.group(1), strip_prefix(path))
            declared.add(m.group(1))

        for m in DEF_RE.finditer(text):
            name = m.group(1)
            if name in C_KEYWORDS:
                continue
            defined.setdefault(name, strip_prefix(path))
            head = text[max(0, m.start() - 12):m.start()]
            if "static" in head:
                static_defs.append((name, strip_prefix(path)))

        for m in DECL_RE.finditer(text):
            declared.add(m.group(1))

        for m in CALL_RE.finditer(text):
            name = m.group(1)
            if name in C_KEYWORDS:
                continue
            if name.startswith(PROJECT_PREFIXES):
                called.setdefault(name, set()).add(strip_prefix(path))

    # ---- 项目内部函数：调用了但没人定义/声明 ----
    missing = []
    for name, where in sorted(called.items()):
        if name in defined or name in declared:
            continue
        missing.append((name, sorted(where)))

    # ---- static 函数定义了却没被调用 ----
    unused = []
    for name, where in static_defs:
        hits = 0
        for path in files:
            text = strip_comments_strings(read_text(path))
            hits += len(CALL_RE.findall(text) and
                        [m for m in CALL_RE.finditer(text) if m.group(1) == name])
        # 定义处本身也算一次，所以 <= 1 才是"从未被调用"
        if hits <= 1:
            unused.append((name, where))

    print("=" * 70)
    print(" 轻量 C 自检：%d 个文件" % len(files))
    print("=" * 70)

    if problems:
        print("\n[括号 / 头文件]")
        for p in problems:
            print("  !! " + p)
    else:
        print("\n[括号 / 头文件]   OK")

    if missing:
        print("\n[项目内部函数：调用了但找不到定义或声明]")
        for name, where in missing:
            print("  !! %s()  被 %s 调用" % (name, ", ".join(where)))
    else:
        print("\n[项目内部函数]     OK（所有 uip_/LCD_/TP_/gui_ … 调用都有定义或声明）")

    if unused:
        print("\n[可能没用的 static 函数（改名后的残留？）]")
        for name, where in unused:
            print("  ?? %s()  定义于 %s" % (name, where))
    else:
        print("\n[static 函数]      OK（没有定义了却没人调用的）")

    bad = len(problems) + len(missing)
    print("\n" + "-" * 70)
    print(" 结果: %s（%d 个问题，%d 个提示）"
          % ("通过" if bad == 0 else "有问题", bad, len(unused)))
    print("=" * 70)
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
