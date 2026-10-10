#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font.py —— 生成嵌入式 UI 用的点阵字库头文件

为什么自己生成点阵字库？
    STM32 端只有 512KB Flash / 64KB RAM，不可能塞 FreeType + TTF；
    I.MX6ULL 端虽然能跑 FreeType，但为了让两端 UI 用同一套字模、
    代码完全一致（便于复用与对照），这里统一用编译期生成的位图字库。

字模格式（统一，ASCII 与汉字同宽）
    · 每个字模固定 CELL 行 x 2 字节 = CELL*2 字节
    · 每行 16 位，最高位对应最左像素
    · ASCII 只画在每行的高 8 位（左侧 8 列），绘制步进 8
    · 汉字用满 16 位，绘制步进 16

用法
    python3 gen_font.py > font16.h          # 生成字库头文件
    python3 gen_font.py --preview a.png     # 额外输出放大预览图
    python3 gen_font.py --art "电压0125."   # 终端里打印 ASCII 艺术字（自检用）

标准输出一律是 CRLF 换行（见 ENDL），这样重定向出来的 font16.h 与 Keil 工程里
其它源码的换行一致，tools/to_gbk.py --check 不会因为纯 LF 行报错。

  注意：这只对「重新生成」出来的文件成立。仓库里现存的 User/Ui/font16.h 是本脚本
  改成 CRLF 输出之前生成的旧产物，仍是纯 LF（552 行），所以 to_gbk.py --check
  现在仍然返回 1。

  行尾与字模内容是两件独立的事：只想把仓库里这一份改成 CRLF、不重新渲染字模，
  直接跑 python tools/to_gbk.py（它覆盖 User/Ui/font16.h，默认方向就是
  UTF-8 -> GBK + CRLF），不需要字体、也不需要 Linux；本脚本解决的是字模内容。
"""

import os
import io
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.stderr.write("需要 Pillow: sudo apt-get install -y python3-pil\n")
    sys.exit(1)

# ---------------------------------------------------------------------------
# 输出行尾
#
#   生成的头文件要落进 Keil 工程，而工程里其它源码都是 CRLF（tools/to_gbk.py
#   的目标状态）。标准输出的默认行尾取决于平台与重定向方式，靠不住，所以这里
#   显式指定：主程序把函数内的 print 换成 _emit()，每一行都以 CRLF 结束。
#
#   为什么不能只靠流层的换行翻译：--out-encoding 分支用
#   TextIOWrapper(..., newline="") 接住原始字节（为了让 \r\n 原样落盘），
#   这种情况下流层不做任何翻译，print 的 \n 会原样写成 LF。
# ---------------------------------------------------------------------------
ENDL = "\r\n"

# 先把内置 print 抓住：main() 里会把本模块的 print 换成 _emit，
# _emit 内部必须用这个原始引用，否则会自己调自己。
_builtin_print = print


def _emit(*parts, **kw):
    """按 CRLF 结束一行（代替内置 print；sep 仍可传）"""
    kw["end"] = ENDL
    return _builtin_print(*parts, **kw)

# ---------------------------------------------------------------------------
# 仓库根目录（本脚本位于 <repo>/tools/gen_font.py）
# ---------------------------------------------------------------------------
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------
CELL   = 16          # 字模方格边长（像素）
CJK_PX = 15          # 汉字渲染字号
ASC_W  = 8           # ASCII 占用宽度
ASC_PX = 13          # ASCII 渲染字号
ROWS   = (CELL + 7) // 8        # 每行字节数（16 像素 -> 2 字节）
GLYPH_BYTES = CELL * ROWS       # 每字模字节数（16*2 = 32）

# 绘制原点相对裁剪窗口的偏移。所有字模共用同一原点，保证基线一致。
ORIGIN_X = 2
ORIGIN_Y = 2

# UI 里会用到的全部汉字（自动去重；缺字会告警）
CJK_TEXT = (
    "国标充电监控终端电池管理系统报文模拟器"
    "电压电流荷电状态温度最高低单体累计量时间实时数据历史曲线"
    "总功率需求允许上限下行条帧速率组号编号探点剩余预计"
    "空闲握手辨识参数配置准备充中结束故障正常异常无错等未接收"
    "已连接断开等待启动停止"
    "秒分时天日期版本型号厂商额定容健康度绝缘"
    "刷新返回上一页下一信息详情记录保存查询"
    "百分比小计平均大值共约"
    # ---- 界面文案补充（uitest 的缺字检查会校验，缺一个都会被抓出来）----
    "会话近满原始解析库击右角看变化采样暂积尚到发"
    "（）「」《》、，。：；！？·—…"
    "当前目标门限报警斜率毫安培"
    "字位节组串并充放电温升冷"
    "成功失败有效无效通过错误"
    "横轴纵轴范围区间坐标刻度"
    "开启关闭暂停继续复位清零"
)


# ---------------------------------------------------------------------------
# 自动扫描：把 UI 源码里出现过的所有非 ASCII 字符并进字库
#
#   手工维护 CJK_TEXT 太容易漏字（改一句界面文案就缺一个字，而缺字在
#   开发板上只会显示成空白，很难发现）。这里直接扫描真正会画到屏上的
#   源文件，凡是出现过的汉字/全角标点全部收进字库，从根上杜绝漏字。
#
#   注意：只扫描「会画到屏上」的文件。ui.c 是终端 ANSI 仪表盘（PC 上跑），
#   它的字符串不需要点阵字模，所以不在列表里。
#
#   【为什么要按平台分档】
#     STM32 那边用 Keil MDK-Lite 编译时，**代码+常量不能超过 32 KB**。
#     汉字字模每个 32 字节，307 个汉字就是 9.8 KB —— 占掉三分之一的额度。
#     两端界面文案并不完全相同（例如历史曲线界面在 STM32 上是另一套措辞），
#     把 Linux 端用到的字也算进 STM32 的字库纯属浪费。
#     所以这里按平台分档：只收该平台真正会画出来的字。
#
#     字模本身仍由同一个脚本、同一种字体渲染，所以两端的字形完全一致，
#     只是「各带各的字表」。缺字检查（uitest / pc_ui）会各自复核。
# ---------------------------------------------------------------------------

# Linux 端：framebuffer 界面 + 离线渲染自检
SCAN_LINUX = [
    "linux_can_monitor/fbdev.c",
    "linux_can_monitor/fbdev.h",
    "linux_can_monitor/gui.c",
    "linux_can_monitor/gui.h",
    "linux_can_monitor/gui_app.c",
    "linux_can_monitor/gui_app.h",
    "linux_can_monitor/uitest.c",
    "linux_can_monitor/uitouch.c",
    # 【必须】协议层的状态名与异常名**会直接画到屏上**
    # （界面「状态」和「异常」两行显示的就是它们）。
    # 之前这个文件不在扫描列表里，导致"握手超时"里的 **握** 和 **超**
    # 不在字库里 —— 屏上就变成了"手 时(BHM 未收到)"，
    # 现场看到的就是"缺字"。Linux 端不受 32 KB 限制，
    # 宁可多收几百个汉字，也不能再漏。
    "linux_can_monitor/gb27930.c",
    "linux_can_monitor/gb27930.h",
    "linux_can_monitor/ui.c",
    "linux_can_monitor/can_layer.c",
    "linux_can_monitor/storage.c",
]

# STM32 端：
#   · ui_app.c   —— 所有画到屏上的文案都在这里
#   · bms_protocol.c —— 状态名/异常名（BMS_StateStr/BMS_ErrorStr）会直接
#                        显示到屏上，所以它们的字符串必须进字库。
#                        这个文件里还有大段中文 printf 日志（走串口，不占字模），
#                        脚本只取字符串字面量、跳过注释，多收几个字可以接受，
#                        换来的是「绝不会漏字」。
SCAN_STM32 = [
    "User/Ui/ui_app.c",
    "User/Ui/ui_app.h",
]

# STM32 界面上还会直接显示协议层给出的**动态**文案（状态名 / 异常短名）。
#
# 这些字在 bms_protocol.c 里，但那个文件里 90% 的中文只走串口日志 ——
# 整文件扫下来会多出 150 多个汉字（实测：当前 77 字会涨到 205 字），
# 按 32 字节/字模 + 4 字节/GBK 映射算就是 5 KB 以上的 Flash，
# 而 Keil MDK-Lite 只给 32 KB 总额度，这一下就能把工程顶爆。
#
# 所以这里只把「真的会画到屏幕上」的那几条列出来：
#   · BMS_StateStr()  —— 界面「状态」一行显示的就是它
#   · BMS_ErrorStr()  —— 界面「异常」一行显示的就是它
# 其余（BMS 串口诊断、LecStr 的 LEC 名字等）都不进字库。
UI_DYNAMIC_TEXT_STM32 = (
    # ---- BMS_StateStr()：故意**不收集** ----
    # 界面「状态」那一行现在画的是充电阶段（待机/充电中/充满/放电中），
    # 协议状态名（空闲/握手/辨识/参数配置/充电准备/结束/故障）不再上屏，
    # 只走串口日志。以前收它们是为了那一行，现在不收：
    # 省下 19 个汉字，把 MDK-Lite 的 32 KB 额度让出来。
    #
    # ---- BMS_ErrorStr()：全部文案都收 ----
    # 「异常」一行显示的就是这些字符串。以前只收了前三条，其余文案里的字
    # 没有字模，ui_port 遇到查不到的字会"推进一格但不画" —— 于是屏上出现
    # 「等待  超时」这种中间空一块的样子，看着像显示 bug。
    #
    # 实测补全它们只差 8 个汉字（主 传 多 失 止 等 败 输），
    # 成本 8 x 36 = 288 字节，而当时 ROM 离上限还有 6 KB 余量，加得起。
    # 如果以后又逼近 32 KB，**先量再删**：把下面这组文案减到只留实际会
    # 出现的那几条，然后重新生成并用 Keil 看 ROM 占用。
    "无"
    "等待 CRM 超时"
    "等待 CML 超时"
    "等待 CRO 超时"
    "充电阶段报文超时"
    "J1939 多帧传输失败"
    "充电机主动中止"
    "CAN 总线 Bus-Off"
)
PROFILES = {
    "all":   SCAN_LINUX + SCAN_STM32,
    "linux": SCAN_LINUX,
    "stm32": SCAN_STM32,
}

# 缺省 = 全都要（生成一份两端通用的字库）
SCAN_FILES = PROFILES["all"]


def resolve_source(rel):
    """把仓库相对路径解析成真实路径。

    同一份源码在本工程里有两种放法：
      · Windows 工作副本:  <repo>/linux_can_monitor/fbdev.c
      · 虚拟机上展平的副本: ~/CAN/fbdev.c
    这里两种都试一遍，脚本在哪儿都能直接跑。
    """
    rel_os = rel.replace("/", os.sep)
    candidates = [
        os.path.join(REPO_ROOT, rel_os),
        os.path.join(REPO_ROOT, os.path.basename(rel_os)),
        os.path.join(os.path.dirname(REPO_ROOT), rel_os),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path
    return None


def extract_string_chars(text):
    """从一份 C 源码里提取「字符串字面量中出现的非 ASCII 字符」。

    只取双引号字符串里的字符，注释里的汉字一律忽略 —— 本工程的注释是
    全中文的，如果把注释也算进去，字库会从 200 多字暴涨到 700 多字，
    白白吃掉十几 KB Flash。

    处理：行注释 //、块注释 /* */、字符串转义 \\" 三种情况。
    """
    out = set()
    i = 0
    n = len(text)
    in_block = in_line = in_str = False

    while i < n:
        c = text[i]
        nxt = text[i + 1] if (i + 1) < n else ""

        if in_line:
            if c == "\n":
                in_line = False
            i += 1
            continue

        if in_block:
            if c == "*" and nxt == "/":
                in_block = False
                i += 2
                continue
            i += 1
            continue

        if in_str:
            if c == "\\":            # 转义序列整体跳过
                i += 2
                continue
            if c == '"':
                in_str = False
                i += 1
                continue
            if ord(c) > 0x7F:
                out.add(c)
            i += 1
            continue

        if c == "/" and nxt == "/":
            in_line = True
            i += 2
            continue
        if c == "/" and nxt == "*":
            in_block = True
            i += 2
            continue
        if c == '"':
            in_str = True
            i += 1
            continue

        i += 1

    return out


def _plausibility(text):
    """给一段解码结果打分：合法的 C 源码应该是「ASCII 为主 + 少量常用汉字」。

    GBK 的字节流往往也能被当成 UTF-8 解出来（只是解成一堆生僻字和怪符号），
    反过来 UTF-8 也能被当成 GBK 解出乱码。所以不能只看「解码成不成功」，
    要看「解出来像不像正常源码」：
        · ASCII（含注释、代码）记 +1
        · CJK 基本区汉字（0x4E00~0x9FFF）记 +1
        · 其它（生僻符号、方块、emoji 区）记 -3
    得分高的那个编码就是正确编码。
    """
    score = 0
    for ch in text:
        o = ord(ch)
        if o < 0x80:
            score += 1
        elif 0x4E00 <= o <= 0x9FFF:
            score += 1
        elif o in (0x3002, 0x3001, 0xFF08, 0xFF09, 0xFF0C, 0xFF1A,
                   0x201C, 0x201D, 0x2018, 0x2019, 0x2014, 0x2026):
            score += 1              # 中文标点
        elif o in (0x2103,):        # ℃
            score += 1
        else:
            score -= 3
    return score


def read_source(path):
    """读取源码文本，自动判断 UTF-8 / GBK。

    本工程里 Linux 端源码是 UTF-8，STM32（Keil）端是 GBK，
    同一个脚本要能同时读两边，所以这里两种编码都试一遍再择优。
    """
    try:
        with open(path, "rb") as fp:
            raw = fp.read()
    except OSError as exc:
        sys.stderr.write("警告: 无法读取 %s (%s)\n" % (path, exc))
        return ""

    # 【重要】先按路径定编码，再靠"择优"兜底。
    #
    # 为什么不能只靠择优：GBK 的双字节序列**经常也能被 UTF-8 成功解码**
    # （解成一堆生僻字和怪符号），于是 raw.decode("utf-8") 不报错，
    # 而"生僻字"在打分函数看来恰恰像是有效内容 —— 结果就是把一整份
    # GBK 源码当 UTF-8 收了，字库凭空多出两百多个乱码汉字。
    # 实测过一次：ui_app.c（GBK）被当成 UTF-8 解码，汉字数会暴涨（旧版实测 90 -> 307）。
    #
    # 本工程的布局是确定的：
    #   User/                -> STM32 / Keil，GBK
    #   linux_can_monitor/   -> Linux / gcc，UTF-8
    #   tools/               -> 生成脚本，UTF-8
    # 所以按路径直接指定，判错了也还有下面的择优兜底。
    p_norm = path.replace("\\", "/")
    if "/User/" in p_norm or p_norm.startswith("User/"):
        order = ("gbk", "utf-8")
    else:
        order = ("utf-8", "gbk")

    best = None
    for enc in order:
        try:
            text = raw.decode(enc)
        except (UnicodeDecodeError, LookupError):
            continue
        score = _plausibility(text)
        if best is None or score > best[0]:
            best = (score, text, enc)

    if best is None:
        sys.stderr.write("警告: %s 既不是 UTF-8 也不是 GBK，已跳过\n" % path)
        return ""

    return best[1]


def scan_cjk():
    """扫描 SCAN_FILES，返回按 Unicode 排序的非 ASCII 字符列表"""
    found = set()
    used = []
    for rel in SCAN_FILES:
        path = resolve_source(rel)
        if path is None:
            continue
        used.append(os.path.basename(path))
        found |= extract_string_chars(read_source(path))
    sys.stderr.write("已扫描 %d 个源文件，字符串中用到 %d 个非 ASCII 字符\n"
                     % (len(used), len(found)))
    return sorted(found)


# ---------------------------------------------------------------------------
# 字体候选
#   汉字和 ASCII 必须分开选字体！
#   DroidSansFallbackFull 这类「CJK fallback」字体只含汉字，
#   用它渲染 '0'/'V' 会得到「缺字方框」，实测确实踩过这个坑。
# ---------------------------------------------------------------------------
CJK_FONT_CANDIDATES = [
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2),   # 2 = SC
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc", 2),
    ("/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", 0),
    ("/usr/share/fonts/truetype/arphic/uming.ttc", 0),
    ("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", 0),
]

ASC_FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]


def open_cjk_font():
    for path, idx in CJK_FONT_CANDIDATES:
        try:
            ImageFont.truetype(path, CJK_PX, index=idx)
            sys.stderr.write("汉字字体: %s (index=%d)\n" % (path, idx))
            return path, idx
        except Exception:
            continue
    sys.stderr.write("找不到可用中文字体，请安装 fonts-noto-cjk 或 fonts-droid-fallback\n")
    sys.exit(1)


def measure(font, ch, pad=10):
    """把单字画到固定原点的大画布上，返回 (画布, 墨迹包围盒)。

    所有字形共用同一个绘制原点，因此 bb[1] 直接反映各自的垂直位置，
    据此可以还原出一致的基线。
    """
    canvas = Image.new("L", (64, 64), 0)
    d = ImageDraw.Draw(canvas)
    d.text((pad, pad), ch, font=font, fill=255)
    return canvas, canvas.getbbox()


def ascii_metrics(font, cw, chh):
    """检查整张 ASCII 表能否塞进 cw x chh，返回 (是否可行, '0' 的墨迹顶端)"""
    ref_top = None
    max_w = max_h = 0
    for code in range(0x20, 0x7F):
        _, bb = measure(font, chr(code))
        if bb is None:
            continue
        max_w = max(max_w, bb[2] - bb[0])
        max_h = max(max_h, bb[3] - bb[1])
        if code == ord("0"):
            ref_top = bb[1]
    if ref_top is None or max_w > cw or max_h > chh:
        return False, None, max_w, max_h
    return True, ref_top, max_w, max_h


def pick_ascii_font(cw, chh):
    """在候选等宽字体中挑一个「字号尽可能大且能塞进单元格」的组合"""
    best = None
    for path in ASC_FONT_CANDIDATES:
        for px in range(chh, 8, -1):
            try:
                f = ImageFont.truetype(path, px, index=0)
            except Exception:
                break
            ok, ref_top, mw, mh = ascii_metrics(f, cw, chh)
            if ok:
                sys.stderr.write("ASCII 字体: %s @%dpx (最大墨迹 %dx%d)\n"
                                 % (path, px, mw, mh))
                return f, ref_top
            if best is None:
                best = (path, px, mw, mh)
    raise SystemExit("找不到能塞进 %dx%d 的 ASCII 字体（最后一次尝试: %s）"
                     % (cw, chh, best))


def render_ascii(ch, font, cw, chh, ref_top):
    """渲染 ASCII：水平左对齐到 x=0，垂直以 '0' 的墨迹顶端为基准（保证基线一致）"""
    canvas, bb = measure(font, ch)
    out = Image.new("L", (cw, chh), 0)
    if bb is not None:
        g = canvas.crop(bb)
        y = bb[1] - ref_top
        if y < 0:
            y = 0
        if y + g.height > chh:
            g = g.crop((0, 0, g.width, chh - y))
        out.paste(g, (0, y))
    return out


def render_cjk(ch, font):
    """渲染汉字到 CELL x CELL，共用绘制原点保证基线一致"""
    canvas, bb = measure(font, ch)
    out = Image.new("L", (CELL, CELL), 0)
    if bb is not None:
        g = canvas.crop(bb)
        if g.width > CELL:
            g = g.crop((0, 0, CELL, g.height))
        if g.height > CELL:
            g = g.crop((0, 0, g.width, CELL))
        out.paste(g, ((CELL - g.width) // 2, (CELL - g.height) // 2))
    return out


def pack(img, cw, chh):
    """按行阈值化成位图字节，MSB 在左，每行 ROWS 字节"""
    px = img.load()
    out = []
    for y in range(chh):
        bits = 0
        for x in range(cw):
            bits = (bits << 1) | (1 if px[x, y] >= 100 else 0)
        bits <<= (ROWS * 8 - cw)          # 右侧补 0，保证 MSB 对齐
        for b in range(ROWS):
            out.append((bits >> (8 * (ROWS - 1 - b))) & 0xFF)
    return out


def pack_ascii_packed(img, cw=ASC_W, chh=CELL):
    """ASCII 专用压缩打包：每行 1 字节（8 像素宽，MSB 在左）

    为什么单独做一份
    ----------------
    汉字是 16x16，每行要 2 字节；ASCII 只有 8 像素宽，但原来也按
    「每行 2 字节」存 —— 每行高 8 位恒为 0。95 个 ASCII 字模因此白占
    95 * 16 = 1520 字节 Flash，而 Keil MDK-Lite 的总额度只有 32 KB。

    压缩后 ASCII 字模 = 16 字节，汉字仍是 32 字节。消费端统一用 font16.h 里的
    FONT_ASC_ROWBYTES / FONT_ASC_GLYPH() / FONT_CJK_GLYPH() 访问，
    不需要各自判断格式。
    """
    px = img.load()
    out = []
    for y in range(chh):
        bits = 0
        for x in range(cw):
            bits = (bits << 1) | (1 if px[x, y] >= 100 else 0)
        out.append(bits & 0xFF)
    return out


def art(glyph, cw, rowbytes=None):
    """把一个字模画成 ASCII 艺术字，用于终端自检

    rowbytes: 每行字节数。None = 传统 2 字节/行；1 = ASCII 压缩格式。
    """
    rb = ROWS if rowbytes is None else rowbytes
    lines = []
    for y in range(CELL):
        row = 0
        for b in range(rb):
            row = (row << 8) | glyph[y * rb + b]
        if rb == 1:
            row = row << 8          # 8 位左移到 16 位的 MSB，后面统一按 15-x 取
        s = ""
        for x in range(cw):
            s += "#" if (row >> (15 - x)) & 1 else "."
        lines.append(s)
    return lines


PACK_ASCII_ART = False      # 由 main() 按档位设置，供 dump_art(--art) 使用


def dump_art(text, cjk_font, asc_font, asc_ref_top):
    blocks = []
    for ch in text:
        if ord(ch) < 0x80:
            img = render_ascii(ch, asc_font, ASC_W, CELL, asc_ref_top)
            if PACK_ASCII_ART:
                blocks.append((ch, art(pack_ascii_packed(img), ASC_W, 1)))
            else:
                blocks.append((ch, art(pack(img, ASC_W, CELL), ASC_W)))
        else:
            g = pack(render_cjk(ch, cjk_font), CELL, CELL)
            blocks.append((ch, art(g, CELL)))

    # 并排打印，每行 4 个字
    for i in range(0, len(blocks), 4):
        group = blocks[i:i + 4]
        print("  ".join("%-16s" % (b[0]) for b in group))
        for y in range(CELL):
            print("  ".join(b[1][y] for b in group))
        print()


def emit_preview(path_out, cjk_font, asc_font, asc_ref_top, samples):
    scale = 3
    pad = 4
    line_h = CELL + 2
    w = 260
    h = line_h * len(samples) + pad * 2

    img = Image.new("RGB", (w * scale, h * scale), (16, 20, 28))
    for row, text in enumerate(samples):
        x = pad
        y = pad + row * line_h
        for ch in text:
            if ord(ch) < 0x80:
                g = render_ascii(ch, asc_font, ASC_W, CELL, asc_ref_top)
                adv = ASC_W
            else:
                g = render_cjk(ch, cjk_font)
                adv = CELL
            big = g.resize((adv * scale, CELL * scale), Image.NEAREST)
            img.paste(Image.merge("RGB", (big, big, big)), (x * scale, y * scale))
            x += adv
    img.save(path_out)
    sys.stderr.write("预览图: %s (%dx%d)\n" % (path_out, img.width, img.height))


def emit_c_array(name, data, per_line=12):
    print("static const unsigned char %s[] = {" % name)
    for i in range(0, len(data), per_line):
        print("    " + " ".join("0x%02X," % v for v in data[i:i + per_line]))
    print("};")
    print()


def c_escape(ch):
    return {"\\": "\\\\", '"': '\\"'}.get(ch, ch)


def main():
    argv = sys.argv[1:]

    # ---- 输出编码 ----
    # 生成的字库要回写到 Keil 工程里，而 STM32 那边的源码是 GBK。
    # 用 --out-encoding gbk 就能直接落盘成 Keil 认的编码，
    # 不用再在 PC 上手工转一道 UTF-8 -> GBK。
    #   python3 tools/gen_font.py --profile stm32 --out-encoding gbk > User/Ui/font16.h
    saved_stdout = None
    if "--out-encoding" in argv:
        enc = argv[argv.index("--out-encoding") + 1]
        # 注意：不能用 sys.stdout.reconfigure() —— 那是 Python 3.7 才有的，
        # 而虚拟机（Ubuntu 18.04）上是 Python 3.6.9。用 TextIOWrapper 包一层，
        # 3.6 / 3.7+ 都能跑。newline="" 关掉流层的换行翻译，
        # 落盘的行尾完全由 _emit() 的 ENDL 决定。
        try:
            # 先把原有 TextIOWrapper 摘掉再包，并留一个引用到 finally 里还回去：
            # 被摘掉的那个包装器一旦被回收就会关掉底层 buffer，
            # 新的包装器（以及调用方如 tee 对旧 sys.stdout 的引用）就写不进去了。
            saved_stdout = sys.stdout
            sys.stdout = io.TextIOWrapper(saved_stdout.detach(),
                                          encoding=enc, newline="")
        except (LookupError, AttributeError, ValueError) as exc:
            sys.stderr.write("无法把输出切到 %s: %s\n" % (enc, exc))
            return 2
        sys.stderr.write("输出编码: %s\n" % enc)

    try:
        # 本函数（含它调用的 emit_c_array）是唯一向标准输出写头文件正文的地方，
        # 这里把 print 换成按 CRLF 结束的 _emit，行尾就不会随平台或重定向方式变化。
        # dump_art() 是终端自检、不进头文件，仍用内置 print。
        global print
        print = _emit

        path, fidx = open_cjk_font()
        cjk_font = ImageFont.truetype(path, CJK_PX, index=fidx)
        asc_font, asc_ref_top = pick_ascii_font(ASC_W, CELL)

        # ---- 按平台选择要扫描的文件集 ----
        #   --profile stm32  → 只收 STM32 界面真正会画出来的字（省 Flash）
        #   --profile linux  → 只收 Linux framebuffer 界面的字
        #   不给就全都要（两端通用的字库）
        global SCAN_FILES
        profile = "all"
        if "--profile" in argv:
            profile = argv[argv.index("--profile") + 1]
            if profile not in PROFILES:
                sys.stderr.write("未知的 profile: %s（可选: %s）\n"
                                 % (profile, ", ".join(PROFILES.keys())))
                return 2
            SCAN_FILES = PROFILES[profile]
        sys.stderr.write("字库档位: %s（扫描 %d 个文件）\n" % (profile, len(SCAN_FILES)))

        # ---- 终端自检：打印 ASCII 艺术字 ----
        if "--art" in argv:
            dump_art(argv[argv.index("--art") + 1], cjk_font, asc_font, asc_ref_top)
            return

        # ---- 预览图 ----
        if "--preview" in argv:
            emit_preview(argv[argv.index("--preview") + 1], cjk_font, asc_font, asc_ref_top, [
                "国标充电监控终端",
                "电压 512.3 V  电流 98.7 A",
                "荷电状态 62.5 %  温度 38 C",
                "历史曲线  返回  实时监控",
                "空闲 握手 辨识 参数配置",
                "准备 充电中 结束 故障 异常",
                "0123456789 ABCDEFabcdef .:-/%#",
            ])

        # ---- 字符集：以「源码扫描」为准 ----
        #   早期版本用一张手工维护的 CJK_TEXT 表兜底，但它会把大量**用不到**的字
        #   也塞进字库。STM32 那边一个字模 32 字节，而 Keil MDK-Lite 只给 32 KB
        #   总额度，浪费不起 —— 实测把 CJK_TEXT 去掉后字库能小掉三分之一。
        #   扫描已经足够可靠，而且还有 uitest / pc_ui 两道缺字检查兜着，
        #   所以默认只用扫描结果；万一扫描没扫到任何字（路径不对之类），
        #   再退回 CJK_TEXT，避免生成一个空字库把屏画成一片空白。
        scanned_all = scan_cjk()
        # STM32 档位补上「协议层提供、但会显示在屏上」的那些字（见上方说明）
        if profile == "stm32":
            have = set(scanned_all)
            added = 0
            for ch in UI_DYNAMIC_TEXT_STM32:
                # ASCII 必须跳过！95 个 ASCII 字模本来就一直在字库里，
                # 早期这里没判断，把 "IDLE" 里的 I/D/L/E 也当汉字塞进了
                # 16x16 表 —— 每个白占 32 字节字模 + 4 字节映射表。
                if ord(ch) < 0x80:
                    continue
                if ch not in have:
                    have.add(ch)
                    scanned_all.append(ch)
                    added += 1
            sys.stderr.write("补充界面动态文案字符: %d 个（状态名/异常短名）\n" % added)

        if scanned_all:
            source_chars = scanned_all
            sys.stderr.write("字符集来源: 源码扫描（%d 个汉字/全角符号）\n"
                             % len(scanned_all))
        else:
            source_chars = sorted(set(CJK_TEXT))
            sys.stderr.write("警告: 源码扫描没有任何结果，退回手工 CJK_TEXT 表（%d 个字）\n"
                             % len(source_chars))
            sys.stderr.write("      请检查 SCAN_FILES 里的路径是否能解析到（在仓库根目录跑本脚本）\n")

        seen = set()
        cjk = []
        for ch in source_chars:
            if ch not in seen:
                seen.add(ch)
                cjk.append(ch)

        # ---- 组装字模 ----
        #   STM32 档位：ASCII 压缩成 16 字节/字；其余档位保持 32 字节/字不变。
        pack_ascii = (profile == "stm32")
        asc_rowbytes = 1 if pack_ascii else ROWS
        global PACK_ASCII_ART
        PACK_ASCII_ART = pack_ascii

        glyphs = []
        for code in range(0x20, 0x7F):
            img = render_ascii(chr(code), asc_font, ASC_W, CELL, asc_ref_top)
            if pack_ascii:
                glyphs.extend(pack_ascii_packed(img))
            else:
                glyphs.extend(pack(img, ASC_W, CELL))
        ascii_count = 0x7F - 0x20
        #   cjk_base  = 汉字字模在 g_font_data 里的起始字节偏移
        #   cjk_start = 写进映射表的 index（压缩格式下相对汉字块，传统格式下是绝对下标）
        # cjk_base = ASCII 字模块**实际占用的字节数**。
        #
        #   千万不能写成 ascii_count * asc_rowbytes —— 那是"每行字节数"不是"每字字节数"。
        #   压缩格式下 asc_rowbytes=1、CELL=16，那么算出来只有 95，
        #   而 ASCII 块实际占 95*16 = 1520 字节。偏移少了 1425 字节，
        #   每个汉字都从错误的位置取字模，屏幕上就是一片"被打碎的乱码"。
        #   这里直接用已生成数组的长度，从根上杜绝算错。
        cjk_base  = len(glyphs) if pack_ascii else 0
        cjk_start = 0 if pack_ascii else ascii_count

        # 再兜一道：ASCII 块大小必须和宏定义一致
        assert cjk_base == (ascii_count * asc_rowbytes * CELL if pack_ascii else 0), \
            "ASCII 字模块大小自检失败"

        missing = []
        for ch in cjk:
            img = render_cjk(ch, cjk_font)
            if img.getbbox() is None:
                missing.append(ch)
            glyphs.extend(pack(img, CELL, CELL))
        if missing:
            sys.stderr.write("警告: 这些字符渲染为空，字体可能缺字: %s\n" % "".join(missing))

        # ---- 输出头文件 ----
        print("/**")
        print("  ******************************************************************************")
        print("  * @file    font16.h")
        print("  * @brief   %dx%d 点阵字库（由 tools/gen_font.py 自动生成，请勿手工修改）" % (CELL, CELL))
        print("  *")
        print("  * 生成字体: %s" % path)
        print("  *")
        print("  * 字模格式:")
        print("  *   · 每字模 %d 行，每行 %d 字节，共 %d 字节，MSB 对应最左像素"
              % (CELL, ROWS, GLYPH_BYTES))
        print("  *   · ASCII 0x20~0x7E 共 %d 个：宽 %d 像素，每行 %d 字节，共 %d 字节"
              % (ascii_count, ASC_W, asc_rowbytes, asc_rowbytes * CELL))
        print("  *   · 汉字共 %d 个：用满 %d 位，绘制步进 %d" % (len(cjk), CELL, CELL))
        print("  ******************************************************************************")
        print("  */")
        print()
        print("#ifndef __FONT16_H")
        print("#define __FONT16_H")
        print()
        print("#include <stdint.h>")
        print()
        print("#define FONT_ASCII_FIRST    0x20u")
        print("#define FONT_ASCII_LAST     0x7Eu")
        print("#define FONT_ASCII_COUNT    %du" % ascii_count)
        print("#define FONT_CJK_COUNT      %du" % len(cjk))
        print("#define FONT_CELL           %du" % CELL)
        print("#define FONT_ASC_WIDTH      %du" % ASC_W)
        print("#define FONT_ROWS           %du" % ROWS)
        print("#define FONT_GLYPH_BYTES    %du" % GLYPH_BYTES)
        print("#define FONT_ASC_BYTES      %du" % (asc_rowbytes * CELL))
        print("#define FONT_ASC_ROWBYTES   %du" % asc_rowbytes)
        print("#define FONT_CJK_BASE       %du" % cjk_base)
        print()
        print("/* 取字模的统一切入口：消费端不要自己算字节偏移，")
        print("   这样 ASCII 压缩与否对上层是透明的。 */")
        print("#define FONT_ASC_GLYPH(n)   (&g_font_data[(uint32_t)(n) * FONT_ASC_BYTES])")
        print("#define FONT_CJK_GLYPH(i)   (&g_font_data[FONT_CJK_BASE + (uint32_t)(i) * FONT_GLYPH_BYTES])")
        print()
        print("/** 汉字 -> 字模下标 映射项 */")
        print("typedef struct")
        print("{")
        print("    uint32_t    codepoint;   /* Unicode 码点 */")
        print("    uint16_t    index;       /* 字模数组下标 */")
        print("} font_map_t;")
        print()
        print("/** 全部字模（ASCII 在前，汉字在后，每字 FONT_GLYPH_BYTES 字节） */")
        emit_c_array("g_font_data", glyphs)
        print("/** 汉字映射表 */")
        print("static const font_map_t g_font_cjk_map[FONT_CJK_COUNT] = {")
        for i, ch in enumerate(cjk):
            print("    { 0x%04X, %d },   /* %s */" % (ord(ch), cjk_start + i, c_escape(ch)))
        print("};")
        print()

        # -------------------------------------------------------------------
        # GBK 映射表（STM32 端专用，可选）
        #
        #   为什么需要它：Linux 端源码是 UTF-8，直接按 Unicode 码点查表即可；
        #   但 STM32 的 Keil 工程历史上一直是 GBK 编码，字符串字面量里躺的是
        #   GBK 双字节码。与其在单片机上加一张巨大的「GBK -> Unicode」转换表，
        #   不如反过来——由 PC 在生成字库时就把每个字的 GBK 码算出来，
        #   单片机只做「读两个字节 -> 查表 -> 得到字模下标」，零额外开销。
        #
        #   用 #ifdef 包起来，Linux 端不定义 FONT_WITH_GBK_MAP 就不会占用空间。
        # -------------------------------------------------------------------
        gbk_items = []
        for i, ch in enumerate(cjk):
            try:
                b = ch.encode("gbk")
            except UnicodeEncodeError:
                continue
            if len(b) != 2:
                continue
            gbk_items.append(((b[0] << 8) | b[1], cjk_start + i))
        gbk_items.sort(key=lambda kv: kv[0])

        print("#ifdef FONT_WITH_GBK_MAP")
        print("/** GBK 双字节码 -> 字模下标 映射项（STM32 端用，按 gbk 升序可二分查找） */")
        print("typedef struct")
        print("{")
        print("    uint16_t    gbk;         /* GBK 双字节码，高字节在前 */")
        print("    uint16_t    index;       /* 字模数组下标 */")
        print("} font_gbk_map_t;")
        print()
        print("#define FONT_GBK_COUNT  %du" % len(gbk_items))
        print()
        print("static const font_gbk_map_t g_font_gbk_map[FONT_GBK_COUNT] = {")
        for code, idx in gbk_items:
            print("    { 0x%04X, %d }," % (code, idx))
        print("};")
        print("#endif /* FONT_WITH_GBK_MAP */")
        print()
        print("#endif /* __FONT16_H */")

        sys.stderr.write("完成: ASCII %d 个 + 汉字 %d 个 = %d 个字模, 共 %d 字节\n"
                         % (ascii_count, len(cjk), ascii_count + len(cjk), len(glyphs)))
        sys.stderr.write("GBK 映射表: %d 项\n" % len(gbk_items))
    finally:
        # 把标准输出还回去：先 flush 新包装器（它写的是 saved_stdout 的 buffer），
        # 恢复后再由解释器退出时统一 flush，避免缓冲顺序问题。
        sys.stdout.flush()
        if saved_stdout is not None:
            sys.stdout = saved_stdout


if __name__ == "__main__":
    main()

if __name__ == "__main__":
    main()
