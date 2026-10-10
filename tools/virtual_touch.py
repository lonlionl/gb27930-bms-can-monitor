#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
virtual_touch.py —— 用 uinput 造一个「虚拟触摸屏」，在 PC 上验证真机触摸链路

为什么需要它
-----------
  「点了没反应 / 坐标偏了 / 左右反了」这类问题只有在真机上才暴露，改一次代码
  就得烧一次板子，一个来回好几分钟。而 Linux 的 uinput 允许用户态**创建一个
  看起来完全像真触摸屏的输入设备**，再往里灌事件 —— 对 can_monitor 来说，
  它和真的 GT1151 没有任何区别。

  于是「事件送达 → 坐标映射 → 命中测试 → 切页」这整条链路可以在 PC 上跑通，
  而且送进去的坐标完全可控：给屏幕坐标（--tap）就走一遍和真机一样的换算，
  给原始坐标（--raw-tap）则直接验证映射与命中区域。

为什么要有「命令 FIFO」
---------------------
  uinput 的事件必须写在**创建设备时那个 fd** 上；另外 open("/dev/uinput")
  一次就是一台新设备。所以「一个进程创建、另一个进程点击」是行不通的。
  解决办法：常驻进程持有 fd，并从一个命名管道读「点击指令」。

坐标约定
--------
  两个方向分开，避免"送进去的到底是屏幕值还是原始值"混淆：

    · --tap X Y      送**屏幕坐标**（0..479 / 0..271）：写管道之前就按 --screen
                     与 --raw-x-max / --raw-y-max 换算成原始值，常驻进程原样发出。
    · --raw-tap X Y  送**原始坐标**，不换算 —— 用来复现"内核上报的量程与实际值
                     不符"（野火板上那块 Goodix 内核声称 34799 x 13064，
                     设备却按屏幕像素上报）。
    · --raw-mode     配合 --create 用，把管道里的数值一律当原始值。管道是唯一
                     的输入通道，常驻进程无法知道写入方用的是哪一组参数，
                     所以默认按屏幕坐标解释；确实要整条管道都灌原始值
                     （比如手工 echo 数值进去）时加这个开关。

  屏幕 -> 原始（与 linux_can_monitor/uitouch.c 的 map_axis() 同一套线性映射）：
    raw_x = round(sx * raw_x_max / (screen_w - 1))
    raw_y = round(sy * raw_y_max / (screen_h - 1))
  默认量程 34799 x 13064、默认屏幕 480x272 时，标题栏四个按钮中心的取值：
    数据曲线（主界面，x 314..401，y 4..23，中心 357,14）  -> 原始 25936 675
    充电    （主界面，x 408..471，y 4..23，中心 439,14）  -> 原始 31893 675
    历史    （数据曲线页，x 332..401，y 4..23，中心 366,14）-> 原始 26590 675
    返回    （数据曲线页，x 408..471，y 4..23，中心 439,14）-> 原始 31893 675
  按钮矩形取自 linux_can_monitor/gui.c 的 gui_button_rect()。

用法
----
    # 终端 A：常驻，创建虚拟触摸屏（会打印出 event 节点名）
    sudo python3 tools/virtual_touch.py --create

    # 终端 B：用它跑界面
    sudo ./can_monitor -i can0 --gui --no-touch-grab
    #  （自动探测会优先选中带 BTN_TOUCH 的虚拟触摸屏；也可以 --touch 显式指定）

    # 终端 C：点主界面右上角的「数据曲线」按钮（屏幕坐标）
    sudo python3 tools/virtual_touch.py --tap 357 14

    # 进数据曲线页后点「历史」，看历史九宫格
    sudo python3 tools/virtual_touch.py --tap 366 14

    # 不换算、直接灌设备原始值：原始 (357,14) 映射到屏幕 (5,0)，
    # 用来对照"内核上报量程与实际值不符"时界面会怎么动
    sudo python3 tools/virtual_touch.py --raw-tap 357 14

    --touch-debug 可以把每次命中的屏幕坐标打在终端 B 的日志里，用来核对换算。
"""

import argparse
import fcntl
import os
import struct
import sys
import time

# ---------------------------------------------------------------- 常量
UINPUT_MAX_NAME_SIZE = 80
ABS_CNT = 0x40

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03

ABS_X, ABS_Y = 0x00, 0x01
ABS_MT_SLOT = 0x2F
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36
ABS_MT_TRACKING_ID = 0x39

BTN_TOUCH = 0x14A
SYN_REPORT = 0x00

DEV = "/dev/uinput"
NODE_FILE = "/tmp/virtual_touch.node"      # 常驻进程把 event 节点名写这里
FIFO = "/tmp/virtual_touch.fifo"           # 点击指令管道


def _IO(t, nr):
    return (t << 8) | nr


def _IOW(t, nr, size):
    return (1 << 30) | (size << 16) | (t << 8) | nr


UI_DEV_CREATE = _IO(ord('U'), 1)
UI_DEV_DESTROY = _IO(ord('U'), 2)
UI_SET_EVBIT = _IOW(ord('U'), 100, 4)
UI_SET_KEYBIT = _IOW(ord('U'), 101, 4)
UI_SET_ABSBIT = _IOW(ord('U'), 103, 4)


def create_device(raw_x_max, raw_y_max, name):
    fd = os.open(DEV, os.O_WRONLY | os.O_NONBLOCK)

    fcntl.ioctl(fd, UI_SET_EVBIT, EV_SYN)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
    fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH)
    for code in (ABS_X, ABS_Y, ABS_MT_SLOT,
                 ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID):
        fcntl.ioctl(fd, UI_SET_ABSBIT, code)

    # struct uinput_user_dev —— 只有一个 ff_effects_max 字段！
    #   写成 80s+HHHH+II 会多出 4 字节，内核 write() 直接返回 EINVAL。
    #   Ubuntu 18.04（kernel 5.x）实测 sizeof = 1116 =
    #       80(name) + 8(input_id) + 4(ff_effects_max) + 4*64*4(四个 abs 数组)
    absmax = [0] * ABS_CNT
    absmin = [0] * ABS_CNT
    absfuzz = [0] * ABS_CNT
    absflat = [0] * ABS_CNT
    absmax[ABS_X] = raw_x_max
    absmax[ABS_Y] = raw_y_max
    absmax[ABS_MT_POSITION_X] = raw_x_max
    absmax[ABS_MT_POSITION_Y] = raw_y_max

    buf = struct.pack("80sHHHHI", name.encode()[:79], 0x03, 0x1234, 0x5678, 1, 0)
    buf += struct.pack("i" * (ABS_CNT * 4), *(absmax + absmin + absfuzz + absflat))
    if len(buf) != 1116:
        sys.stderr.write("警告: uinput_user_dev 组装成 %d 字节，期望 1116\n" % len(buf))
    os.write(fd, buf)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd


def emit(fd, etype, code, value):
    # struct input_event { struct timeval time; __u16 type, code; __s32 value; }
    os.write(fd, struct.pack("llHHi", 0, 0, etype, code, value))


def tap_raw(fd, raw_x, raw_y):
    """一次完整的按下 + 抬起（Type B：TRACKING_ID + 坐标 + BTN_TOUCH）"""
    emit(fd, EV_ABS, ABS_MT_SLOT, 0)
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 1)
    emit(fd, EV_ABS, ABS_MT_POSITION_X, raw_x)
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, raw_y)
    emit(fd, EV_KEY, BTN_TOUCH, 1)
    emit(fd, EV_SYN, SYN_REPORT, 0)
    time.sleep(0.12)
    emit(fd, EV_ABS, ABS_MT_SLOT, 0)
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1)
    emit(fd, EV_KEY, BTN_TOUCH, 0)
    emit(fd, EV_SYN, SYN_REPORT, 0)
    time.sleep(0.12)


def find_event_node(name, timeout=4.0):
    t_end = time.time() + timeout
    while time.time() < t_end:
        try:
            txt = open("/proc/bus/input/devices").read()
        except OSError:
            return None
        for block in txt.split("\n\n"):
            if name in block:
                for line in block.splitlines():
                    if line.startswith("H: Handlers"):
                        for tok in line.split():
                            if tok.startswith("event"):
                                return "/dev/input/" + tok
        time.sleep(0.2)
    return None


def cmd_create(args):
    fd = create_device(args.raw_x_max, args.raw_y_max, args.name)
    node = find_event_node(args.name)
    if node is None:
        sys.stderr.write("设备创建了但没找到 event 节点\n")
        return 1

    with open(NODE_FILE, "w") as f:
        f.write("%s\n" % node)
    if os.path.exists(FIFO):
        os.unlink(FIFO)
    os.mkfifo(FIFO)

    print("虚拟触摸屏已就绪")
    print("  event 节点 : %s" % node)
    print("  原始量程   : X[0..%d] Y[0..%d]" % (args.raw_x_max, args.raw_y_max))
    print("  逻辑屏幕   : %dx%d" % (args.screen_w, args.screen_h))
    print("  指令管道   : %s" % FIFO)
    print("")
    print("另一个终端里跑：sudo ./can_monitor -i can0 --gui --no-touch-grab")
    print("再另一个终端点按钮：sudo python3 %s --tap 357 14" % sys.argv[0])
    print("（--tap 送屏幕坐标、--raw-tap 送原始坐标，两者都由写管道的那一侧算好）")
    print("Ctrl+C 结束。")
    sys.stdout.flush()

    try:
        while True:
            # 阻塞读管道；每读到一行就发一次触摸
            with open(FIFO, "r") as fp:
                for line in fp:
                    parts = line.split()
                    if len(parts) != 2:
                        continue
                    sx, sy = int(parts[0]), int(parts[1])
                    if args.raw_mode:
                        # 直接当原始值发：用来复现「内核上报量程与实际值不符」
                        raw_x, raw_y = sx, sy
                    else:
                        # 管道里是屏幕坐标，按 --screen 与量程换算成原始值
                        raw_x, raw_y = screen_to_raw(sx, sy, args)
                    print("  -> 发送屏幕(%d,%d) 原始(%d,%d)" % (sx, sy, raw_x, raw_y))
                    sys.stdout.flush()
                    tap_raw(fd, raw_x, raw_y)
    except KeyboardInterrupt:
        pass
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)
        for p in (FIFO, NODE_FILE):
            if os.path.exists(p):
                os.unlink(p)
        print("\n虚拟触摸屏已销毁")
    return 0


def screen_to_raw(sx, sy, args):
    """屏幕坐标 -> 设备原始坐标（与 uitouch.c 的 map_axis() 同一套线性映射）"""
    raw_x = int(round(sx * args.raw_x_max / max(args.screen_w - 1, 1)))
    raw_y = int(round(sy * args.raw_y_max / max(args.screen_h - 1, 1)))
    return raw_x, raw_y


def cmd_tap(args):
    if not os.path.exists(FIFO):
        sys.stderr.write("找不到 %s —— 请先在另一个终端跑 --create\n" % FIFO)
        return 1

    if args.raw_tap:
        # 直接按原始值发：常驻进程原样送给设备，不做换算。
        # 用来复现野火板上那块 Goodix 的情况：内核上报量程 34799x13064，
        # 但设备实际按屏幕像素(0..480/0..272)上报。
        x, y = args.raw_tap
        print("已发送原始坐标 (%d,%d)（不经换算，直接发往设备）" % (x, y))
    else:
        # 按屏幕坐标发：常驻进程读的是 --screen 指定的逻辑屏幕
        sx, sy = args.tap
        x, y = screen_to_raw(sx, sy, args)
        print("已发送屏幕坐标 (%d,%d) -> 原始 (%d,%d)" % (sx, sy, x, y))

    with open(FIFO, "w") as fp:
        fp.write("%d %d\n" % (x, y))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--create", action="store_true", help="创建虚拟触摸屏并常驻")
    ap.add_argument("--tap", nargs=2, type=int, metavar=("X", "Y"),
                    help="点一下（逻辑屏幕坐标，按 --screen 与量程换算成原始值）")
    ap.add_argument("--raw-tap", nargs=2, type=int, metavar=("RAWX", "RAWY"),
                    help="按原始值点一下（不经换算，复现内核量程与实际值不符的情况）")
    ap.add_argument("--raw-mode", action="store_true",
                    help="--create 时常驻进程把管道里的数值一律当原始值"
                         "（默认按屏幕坐标解释）")
    ap.add_argument("--name", default="gb27930 virtual touch")
    ap.add_argument("--raw-x-max", type=int, default=34799,
                    help="原始 X 量程上限（默认照抄野火板上的 Goodix）")
    ap.add_argument("--raw-y-max", type=int, default=13064)
    ap.add_argument("--screen", default="480x272",
                    help="逻辑屏幕尺寸 WxH，用于屏幕坐标 -> 原始坐标的换算")
    args = ap.parse_args()

    w, h = args.screen.lower().split("x")
    args.screen_w, args.screen_h = int(w), int(h)

    if os.geteuid() != 0:
        sys.stderr.write("需要 root（uinput 只有 root 能写）: sudo python3 %s ...\n"
                         % sys.argv[0])
        return 1
    if not os.path.exists(DEV):
        sys.stderr.write("没有 %s，先 sudo modprobe uinput\n" % DEV)
        return 1

    if args.create:
        return cmd_create(args)
    if args.tap:
        return cmd_tap(args)
    if args.raw_tap:
        return cmd_tap(args)

    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
