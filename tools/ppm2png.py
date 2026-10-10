#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ppm2png.py —— 把界面导出的 PPM 帧批量转成 PNG，便于目视检查

为什么分两步：
    C 侧导出 PPM 不需要任何压缩库（PNG 依赖 zlib），代码只有几十行；
    而 PPM 是无损位图，转 PNG 在 PC 上用 Pillow 一行就能搞定。
    这样开发板上既不用装图形库，也不用引入压缩依赖。

用法:
    python3 tools/ppm2png.py <目录或文件> [更多...]
    python3 tools/ppm2png.py /tmp/gui_frames --scale 2     # 放大 2 倍输出
    python3 tools/ppm2png.py /tmp/ui_dashboard.ppm

依赖:
    pip3 install pillow     （Ubuntu: sudo apt-get install -y python3-pil）
"""

import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("需要 Pillow: sudo apt-get install -y python3-pil\n")
    sys.exit(1)


def collect(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for name in sorted(os.listdir(p)):
                if name.lower().endswith((".ppm", ".pnm")):
                    out.append(os.path.join(p, name))
        elif os.path.isfile(p):
            out.append(p)
        else:
            sys.stderr.write("跳过不存在的路径: %s\n" % p)
    return out


def main():
    argv = sys.argv[1:]
    scale = 1
    if "--scale" in argv:
        i = argv.index("--scale")
        scale = int(argv[i + 1])
        del argv[i:i + 2]

    if not argv:
        sys.stderr.write(__doc__)
        return 2

    files = collect(argv)
    if not files:
        sys.stderr.write("没有找到任何 PPM 文件\n")
        return 1

    ok = 0
    for src in files:
        dst = os.path.splitext(src)[0] + ".png"
        try:
            img = Image.open(src).convert("RGB")
            if scale > 1:
                img = img.resize((img.width * scale, img.height * scale),
                                 Image.NEAREST)
            img.save(dst, "PNG")
            print("%-46s -> %-46s %dx%d" % (
                os.path.basename(src), os.path.basename(dst),
                img.width, img.height))
            ok += 1
        except Exception as exc:                      # noqa: BLE001
            sys.stderr.write("转换失败 %s: %s\n" % (src, exc))

    print("\n共转换 %d 个文件" % ok)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
