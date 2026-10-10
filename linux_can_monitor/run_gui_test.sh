#!/usr/bin/env bash
# ============================================================================
#  run_gui_test.sh —— 无屏幕 / 无触摸硬件下，跑通并验证整条本地界面链路
#
#  它在虚拟 CAN 总线上把「充电机 + BMS 模拟器」跑起来，让 can_monitor 真正
#  收到报文、真正写进 SQLite，然后带 --gui 启动界面：
#
#      --fb virtual    界面渲染到内存虚拟屏（不需要 /dev/fb0）
#      --touch demo    触摸层按脚本产生触摸事件
#                      （标称 2s/5s 点按钮、8s 点空白；计时自 uitouch_open()
#                        起算，实际触发时刻受 30 ms 触摸轮询粒度影响）
#      --gui-rec DIR   每一帧导出成 PPM
#
#  最后把 PPM 转成 PNG，并逐项校验：
#      · 界面线程真的起来了、真的渲染出了帧
#      · 触摸切页真的发生了（导出帧里同时有主界面和数据曲线页）
#      · 数据曲线页的绘图区里真的画出了本次充电的曲线
#        （判据：绘图区 x 58..472 / y 70..200 内出现「曲线颜色」的像素。
#          指标按钮行 y 48..65 不在判据区内 —— 选中按钮的蓝色填充与
#          「电压 V」曲线同色，把它算进来会让这条断言恒真，详见第 6 步）
#      · 点空白区域没有误触发切页
#
#   注意：本脚本会在 can_monitor 后面加 -v。
#   产品形态下逐报文/触摸/界面日志都只在 -v 时才输出，而这个脚本正是
#   靠"日志里有没有出现某行"来判断切页有没有发生的，所以必须开 -v。
#   人眼看板子的时候不需要加，串口是干净的。
#
#   第 3、4 步都加 --auto-start。程序不会自己上线：只有收到「开始充电」请求
#   才发 CHM，而 fake_bms.py 要等到 CHM 才开始握手。第 4 步少了这个开关，
#   这一轮从头到尾停在待机，数据曲线页走空态分支（gui.c 画「尚未开始充电，
#   点主界面『充电』」），绘图区一个采样点都没有。
#
#  用法:
#      ./run_gui_test.sh                 # 默认 vcan0
#      sudo ./run_gui_test.sh can0       # 指定接口
# ============================================================================

set -u

IFACE="${1:-vcan0}"
WORK="$(cd "$(dirname "$0")" && pwd)"
REC_DIR="/tmp/gui_frames"
rm -rf "$REC_DIR"
OUT_DIR="/tmp/gui_frames_png"
DB="/tmp/gui_test.db"
# 日志一律用 mktemp 生成。
# 【为什么不用固定的 /tmp/xxx.log】这些脚本常常一会儿用 sudo 跑、一会儿用普通
# 用户跑。固定路径的日志文件如果上一次是 root 建的，这一次的重定向就会
# "权限不够" 而失败 —— 表现出来是莫名其妙的"编译失败"，其实 make 根本没跑。
GUI_LOG="$(mktemp /tmp/gui_mon.XXXXXX.log)"
DATA_LOG="$(mktemp /tmp/gui_data.XXXXXX.log)"
BMS_LOG="$(mktemp /tmp/gui_bms.XXXXXX.log)"
MAKE_LOG="$(mktemp /tmp/gui_make.XXXXXX.log)"
PNG_LOG="$(mktemp /tmp/gui_png.XXXXXX.log)"

PASS=0
FAIL=0

pass() { echo "  [PASS] $*"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }

# ppm2png.py 的位置：本目录（虚拟机展平布局）或仓库的 tools/ 下（原始布局）
find_ppm2png() {
    for c in "$WORK/ppm2png.py" \
             "$WORK/tools/ppm2png.py" \
             "$WORK/../tools/ppm2png.py" \
             "$WORK/../../tools/ppm2png.py"; do
        [ -f "$c" ] && { echo "$c"; return 0; }
    done
    return 1
}

echo "======================================================================"
echo " GB/T 27930-2015  本地界面链路验证（虚拟屏 + 脚本化触摸）"
echo " 接口=$IFACE  工作目录=$WORK"
echo "======================================================================"

cd "$WORK" || exit 1

# ---------------------------------------------------------------------------
echo "[1/6] 准备虚拟 CAN 接口"
# ---------------------------------------------------------------------------
if ! ip link show "$IFACE" >/dev/null 2>&1; then
    case "$IFACE" in
        vcan*)
            sudo modprobe vcan 2>/dev/null || true
            sudo ip link add dev "$IFACE" type vcan 2>/dev/null || true
            ;;
    esac
fi
if ip link show "$IFACE" >/dev/null 2>&1; then
    sudo ip link set "$IFACE" up 2>/dev/null || true
    pass "接口 $IFACE 就绪"
else
    fail "接口 $IFACE 不存在（vcan 需要 sudo modprobe vcan）"
    exit 1
fi

# ---------------------------------------------------------------------------
echo "[2/6] 编译（含界面模块）"
# ---------------------------------------------------------------------------
if make >"$MAKE_LOG" 2>&1; then
    pass "编译通过（make 退出码 0；warning 由下面一条单独判）"
else
    fail "编译失败，见 $MAKE_LOG"
    tail -20 "$MAKE_LOG"
    exit 1
fi
if grep -qi 'warning' "$MAKE_LOG"; then
    fail "编译有 warning，见 $MAKE_LOG"
fi

# ---------------------------------------------------------------------------
echo "[3/6] 跑一轮真实数据采集，把 SQLite 灌上充电曲线"
echo "      （本步只校验解析记录确实写进了 charge_data：充电阶段每秒一条。"
echo "        数据曲线页画的是 GUI 线程内存里的 g_gui.curve，不读这张表）"
# ---------------------------------------------------------------------------
rm -f "$DB" "$DB"-wal "$DB"-shm
# 注意：can_monitor 主动发 CHM，fake_bms.py 收到后才开始回握手报文，
#       所以监控端要比 BMS 端先起、且跑得更久一点。
sudo ./can_monitor -i "$IFACE" -b 250000 -d "$DB" --no-ui --auto-start -r 14 >"$DATA_LOG" 2>&1 &
MON_PID=$!
sleep 1
python3 fake_bms.py "$IFACE" 10 >"$BMS_LOG" 2>&1 &
BMS_PID=$!

wait "$BMS_PID" 2>/dev/null
wait "$MON_PID" 2>/dev/null

ROWS=$(python3 - "$DB" <<'PY'
import sqlite3, sys
try:
    c = sqlite3.connect(sys.argv[1])
    print(c.execute("SELECT COUNT(*) FROM charge_data").fetchone()[0])
except Exception:
    print(0)
PY
)
if [ "${ROWS:-0}" -gt 3 ] 2>/dev/null; then
    pass "SQLite 已写入 $ROWS 条解析记录（charge_data，充电阶段每秒一条）"
else
    fail "SQLite 只有 ${ROWS:-0} 条解析记录（期望 > 3），解析或存储链路没跑通（见 $DATA_LOG）"
fi

# ---------------------------------------------------------------------------
echo "[4/6] 带界面启动：虚拟屏 + 脚本化触摸 + 导出每一帧"
echo "      （同时挂上 BMS 模拟器，并加 --auto-start：本轮真的充电，数据曲线页"
echo "        才有本次充电的采样点；主界面帧跨越握手前后，前几帧仍是占位符）"
# ---------------------------------------------------------------------------
rm -rf "$REC_DIR"; mkdir -p "$REC_DIR"

python3 fake_bms.py "$IFACE" 9 >"$BMS_LOG" 2>&1 &
BMS2_PID=$!
sleep 1
# 【为什么必须带 --auto-start】程序不会自己上线：只有收到「开始充电」请求才发
# CHM，而 fake_bms.py 要等到 CHM 才开始握手。少了这个开关，这一轮从头到尾停在
# 待机，数据曲线页走空态分支（gui.c 画「尚未开始充电，点主界面『充电』」），
# 绘图区一个采样点都没有 —— 曲线断言就成了在验一条根本不存在的曲线。
sudo ./can_monitor -i "$IFACE" -b 250000 -d "$DB" --no-ui \
     --gui --fb virtual --touch demo --gui-rec "$REC_DIR" --verbose \
     --auto-start -r 11 >"$GUI_LOG" 2>&1 &
GUI_PID=$!
wait "$GUI_PID" 2>/dev/null
wait "$BMS2_PID" 2>/dev/null

if grep -q -- "界面线程启动" "$GUI_LOG"; then
    pass "界面线程已启动"
else
    fail "界面线程没有起来，见 $GUI_LOG"
fi
if grep -q -- "虚拟 framebuffer" "$GUI_LOG"; then
    pass "使用虚拟 framebuffer 渲染（无需 /dev/fb0）"
else
    fail "没有走虚拟 framebuffer 分支"
fi

FRAME_N=$(ls "$REC_DIR"/*.ppm 2>/dev/null | wc -l)
if [ "${FRAME_N:-0}" -ge 5 ] 2>/dev/null; then
    pass "已导出 $FRAME_N 帧渲染结果"
else
    fail "只导出了 ${FRAME_N:-0} 帧（期望 >= 5）"
fi

MAIN_N=$(ls "$REC_DIR"/*_main.ppm 2>/dev/null | wc -l)
CURVE_N=$(ls "$REC_DIR"/*_curve.ppm 2>/dev/null | wc -l)
GRID_N=$(ls "$REC_DIR"/*_grid.ppm 2>/dev/null | wc -l)
[ "${MAIN_N:-0}" -ge 1 ] 2>/dev/null && pass "主界面帧 $MAIN_N 张" \
                                      || fail "没有主界面帧"

# ---------------------------------------------------------------------------
echo "[5/6] 校验触摸切页真的发生了，且空白区域不误触发"
# ---------------------------------------------------------------------------
IN_N=$(grep -c -- "-> 数据曲线" "$GUI_LOG" 2>/dev/null || true)
BACK_N=$(grep -c -- "-> 历史记录" "$GUI_LOG" 2>/dev/null || true)
IN_N=${IN_N:-0}; BACK_N=${BACK_N:-0}

[ "$IN_N" -eq 1 ]   && pass "触摸按钮触发了「主界面 -> 数据曲线」切页（1 次）" \
                    || fail "进入数据曲线的次数为 $IN_N（期望恰好 1）"
[ "$BACK_N" -eq 1 ] && pass "再次触摸触发了「数据曲线 -> 历史九宫格」切页（1 次）" \
                    || fail "进入历史九宫格的次数为 $BACK_N（期望恰好 1）"
[ "${CURVE_N:-0}" -ge 1 ] 2>/dev/null \
    && pass "数据曲线帧 $CURVE_N 张（数据曲线页确实渲染了）" \
    || fail "没有数据曲线帧"

[ "${GRID_N:-0}" -ge 1 ] \
    && pass "历史九宫格帧 $GRID_N 张（九宫格真的画出来了）" \
    || fail "没有历史九宫格帧"

if grep -q -- "演示脚本第 3 步" "$GUI_LOG"; then
    pass "点空白区域的第 3 步已执行，且没有产生额外切页（未误触发）"
else
    fail "演示脚本第 3 步（点空白）没有执行"
fi

# ---------------------------------------------------------------------------
echo "[6/6] 转 PNG，并对渲染结果做像素级校验"
# ---------------------------------------------------------------------------
PPM2PNG="$(find_ppm2png || true)"
if [ -n "$PPM2PNG" ] && python3 "$PPM2PNG" "$REC_DIR" >"$PNG_LOG" 2>&1; then
    cp -f "$REC_DIR"/*.png "$OUT_DIR" 2>/dev/null || {
        mkdir -p "$OUT_DIR"; cp -f "$REC_DIR"/*.png "$OUT_DIR"; }
    pass "渲染帧已转成 PNG（$OUT_DIR/*.png）"
else
    fail "PNG 转换失败（脚本: ${PPM2PNG:-未找到}，见 $PNG_LOG）"
fi

CHK=$(python3 - "$REC_DIR" <<'PY'
import glob, os, sys
d = sys.argv[1]
curve = sorted(glob.glob(os.path.join(d, "*_curve.ppm")))
grid  = sorted(glob.glob(os.path.join(d, "*_grid.ppm")))
main  = sorted(glob.glob(os.path.join(d, "*_main.ppm")))
def load(p):
    f = open(p, "rb"); assert f.readline().strip() == b"P6"
    w, h = map(int, f.readline().split()); f.readline()
    return w, h, f.read()
def ink(path, box):
    """统计「非背景」像素：用来判断界面到底画没画东西。
    背景色 FB_COL_BG = (0x14,0x18,0x20)。"""
    w, h, px = load(path)
    x0, y0, x1, y1 = box
    n = 0
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w)):
            i = (y * w + x) * 3
            if (px[i], px[i+1], px[i+2]) != (0x14, 0x18, 0x20):
                n += 1
    return n

def colored(path, box):
    """统计「高饱和」像素（max-min > 40 且 max > 90）。
    只做诊断用：它分不清「曲线」和「选中按钮的纯色填充」，两者都算。"""
    w, h, px = load(path)
    x0, y0, x1, y1 = box
    n = 0
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w)):
            i = (y * w + x) * 3
            r, g, b = px[i], px[i+1], px[i+2]
            # 只统计「明显有色」的像素：排除背景/面板/边框这些低饱和灰蓝
            mx, mn = max(r, g, b), min(r, g, b)
            if mx - mn > 40 and mx > 90:
                n += 1
    return n

# ---- 数据曲线页的几何（与 gui.c 的宏逐一对齐；设计基准 480x272，g_ox=g_oy=0）----
#   MBTN_X0=8  MBTN_Y=48  MBTN_W=110  MBTN_H=18
#       → 四个指标切换按钮占 y 48..65（含 1 像素边框），x 8..462
#   CURVE_X0=58  CURVE_Y0=70  CURVE_X1=472  CURVE_Y1=200
#       → 曲线绘图区占 x 58..472、y 70..200（左侧刻度画在 x 4..28，不在区内）
# 判据区**只取绘图区**：指标按钮行绝不能进判据区 —— 选中按钮的填充色就是
# FB_COL_BLUE，而 FB_COL_BLUE 同时是「电压 V」那条曲线的颜色，把按钮行算进来
# 会让断言恒真（旧判据区的读数几乎全部来自按钮填充，见下面的 diag_* 诊断行）。
CURVE_BOX  = (58, 70, 473, 201)   # 上界写成开区间，把 x=472 / y=200 的端点包进来
METRIC_ROW = (8, 48, 462, 66)     # 指标按钮行，诊断用
OLD_BOX    = (40, 40, 476, 240)   # 旧判据区（含按钮行），诊断用

# 四条曲线各自的颜色，取自 gui.c 的 g_metric[]，取值同 fbdev.h：
#   电压 V = BLUE(4C 9A FF)  电流 A = ORANGE(FF 9A 3D)
#   温度 C = YELLOW(F2 C1 4E)  电量 % = GREEN(3D D6 8C)
# 判据 = 绘图区里出现这四种颜色中的任意一种，也就是「画出了一条曲线」：
#   · 曲线为空时（gui.c 的 n == 0 分支）绘图区只画网格线 FB_COL_BORDER、
#     左侧刻度与提示文字 FB_COL_DIM，没有一个像素是曲线颜色 → 读数恒为 0；
#   · 有采样点时 draw_series_metric() 至少画一个点（1 个点走 fb_pixel，
#     >= 2 个点走 fb_polyline），用的就是 g_metric[m].color → 读数 >= 1。
# 绘图原语都是直接写 RGB、不混色也不抗锯齿（fbdev.c 的 fb_pixel / fb_line），
# 所以屏幕上的颜色与上面的常量精确相等，不存在"近似色"判不出来。
CURVE_COLORS = {(0x4C, 0x9A, 0xFF), (0xFF, 0x9A, 0x3D),
                (0xF2, 0xC1, 0x4E), (0x3D, 0xD6, 0x8C)}

def curve_pixels(path, box):
    """统计 box 内属于四条曲线颜色的像素数（判据读数）。"""
    w, h, px = load(path)
    x0, y0, x1, y1 = box
    n = 0
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w)):
            i = (y * w + x) * 3
            if (px[i], px[i+1], px[i+2]) in CURVE_COLORS:
                n += 1
    return n

# 主界面的判据用「非背景像素」而不是「高饱和像素」：主界面帧都取在触摸切页
# 之前，画面主体是标题栏、表格文字和数字，都不是高饱和色；只要界面真的画出来
# 了，非背景像素就一定上万。帧有 10 张，末帧可能刚好在切页前一刻、内容还没画
# 满，所以取所有主界面帧里读数最大的一张，判据不因采样时刻而误报。
main_ink = max([ink(p, (0, 0, 480, 272)) for p in main] or [-1])
print("main_ink", main_ink)

# 数据曲线帧同样取「曲线像素最多的一张」：曲线只向右生长、采样点只增不减，
# 取 max 与取最后一张等价，且不会因为采样时刻而误判。
curve_counts = [curve_pixels(p, CURVE_BOX) for p in curve] or [-1]
print("curve_px", max(curve_counts))

# ---- 以下为诊断输出，不参与任何判据（[6/6] 的两条判据只读上面的 ----
# main_ink 与 curve_px）。它们的用途是把「旧判据区为什么会让断言恒真」
# 摊开：指标按钮行里的彩色像素是千级（diag_metric_row），旧判据区几乎等于它
# （diag_old_box），而同一张帧里真正的曲线只有个位数到十几（curve_px）。
main_counts = [colored(p, (0, 30, 480, 272)) for p in main] or [-1]
print("diag_metric_row", colored(curve[-1], METRIC_ROW) if curve else -1)
print("diag_old_box",    colored(curve[-1], OLD_BOX)    if curve else -1)
print("diag_main_colored", max(main_counts))
print("diag_grid_colored", colored(grid[-1], (0, 30, 480, 272)) if grid else -1)
print("diag_frames", "main", len(main), "curve", len(curve), "grid", len(grid),
      "main_colored", main_counts[:6])
PY
)
echo "$CHK" | sed 's/^/      /'

CURVE_PX=$(echo "$CHK" | awk '/^curve_px/{print $2}')
MAIN_INK=$(echo "$CHK" | awk '/^main_ink/{print $2}')
# 判据 >= 1：绘图区里只要出现 1 个曲线颜色的像素，就说明本次充电至少采到 1 个
# 采样点并且被画了出来（原理见上面 CURVE_COLORS 的注释）；曲线为空时恒为 0，
# 所以这一条不是恒真断言。
# 【为什么不写 > 200 这类大数字】本轮 -r 11，数据曲线页只在触摸演示的 2 s ~ 5 s
# 之间显示，切页窗口内通常只采到 2 ~ 4 个点；而横轴是**电量增量**（fake_bms 每秒
# +1.0 %，换算到横轴上每点只往右走 414 x 10 / 1000 ≈ 4 像素），按这个速率折算，
# 光栅像素数在 4n-3 这个量级（n=2 → 5、n=3 → 9、n=4 → 13）。
# 用大阈值会把"曲线真的画出来了"判成失败。
[ "${CURVE_PX:--1}" -ge 1 ] 2>/dev/null \
    && pass "数据曲线帧绘图区内有 $CURVE_PX 个曲线颜色像素（本次充电曲线已画出）" \
    || fail "数据曲线帧绘图区内没有曲线颜色像素（读数 ${CURVE_PX:-0}）：曲线页只在触摸演示的 2 s ~ 5 s 之间显示，这段窗口里没有采样点，说明握手还没完成或第 4 步少了 --auto-start"
[ "${MAIN_INK:--1}" -gt 2000 ] 2>/dev/null \
    && pass "主界面帧有 $MAIN_INK 个非背景像素（界面确实画出来了）" \
    || fail "主界面帧非背景像素仅 ${MAIN_INK:-0}，界面可能没画出来"

# ---------------------------------------------------------------------------
echo "----------------------------------------------------------------------"
echo " 测试结果:  PASS $PASS / FAIL $FAIL"
echo " 渲染帧:    $REC_DIR  （PNG 在 $OUT_DIR）"
echo " 运行日志:  $GUI_LOG   $DATA_LOG   $BMS_LOG"
echo "======================================================================"

[ "$FAIL" -eq 0 ]
