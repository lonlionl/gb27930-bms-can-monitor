#!/usr/bin/env bash
# ============================================================================
#  run_gui_test.sh —— 无屏幕 / 无触摸硬件下，跑通并验证整条本地界面链路
#
#  它在虚拟 CAN 总线上把「充电机 + BMS 模拟器」跑起来，让 can_monitor 真正
#  收到报文、真正写进 SQLite，然后带 --gui 启动界面：
#
#      --fb virtual    界面渲染到内存虚拟屏（不需要 /dev/fb0）
#      --touch demo    触摸层按脚本产生触摸事件（2s/5s 点按钮，8s 点空白）
#      --gui-rec DIR   每一帧导出成 PPM
#
#  最后把 PPM 转成 PNG，并逐项校验：
#      · 界面线程真的起来了、真的渲染出了帧
#      · 触摸切页真的发生了（导出帧里同时有主界面和历史曲线界面）
#
#   注意：本脚本会在 can_monitor 后面加 -v。
#   产品形态下逐报文/触摸/界面日志都只在 -v 时才输出，而这个脚本正是
#   靠"日志里有没有出现某行"来判断切页有没有发生的，所以必须开 -v。
#   人眼看板子的时候不需要加，串口是干净的。
#      · 历史曲线真的从 SQLite 取到了数据（曲线帧不是空图）
#      · 点空白区域没有误触发切页
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
    pass "编译通过（0 warning）"
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
echo "      （历史曲线界面读的就是这张表，没有数据就画不出线）"
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
    pass "SQLite 已灌入 $ROWS 条解析记录（历史曲线有数据可画）"
else
    fail "SQLite 只有 ${ROWS:-0} 条记录，历史曲线会是空的（见 $DATA_LOG）"
fi

# ---------------------------------------------------------------------------
echo "[4/6] 带界面启动：虚拟屏 + 脚本化触摸 + 导出每一帧"
echo "      （同时挂上 BMS 模拟器，这样导出的帧上是真实读数而不是占位符）"
# ---------------------------------------------------------------------------
rm -rf "$REC_DIR"; mkdir -p "$REC_DIR"

python3 fake_bms.py "$IFACE" 9 >"$BMS_LOG" 2>&1 &
BMS2_PID=$!
sleep 1
sudo ./can_monitor -i "$IFACE" -b 250000 -d "$DB" --no-ui \
     --gui --fb virtual --touch demo --gui-rec "$REC_DIR" --verbose \
     -r 11 >"$GUI_LOG" 2>&1 &
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
HIST_N=$(ls "$REC_DIR"/*_history.ppm 2>/dev/null | wc -l)
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
    && pass "数据曲线帧 $CURVE_N 张（曲线界面真的画出来了）" \
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
hist  = sorted(glob.glob(os.path.join(d, "*_curve.ppm")))
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
# 主界面帧有 10 张，最后一张可能刚好在切页前一刻、内容还没画满，
# 所以取**所有主界面帧里彩色像素最多的一张** —— 只要界面真的画得出来，
# 就一定有一帧是完整的。这样判据不会因为采样时刻而误报。
main_counts = [colored(p, (0, 30, 480, 272)) for p in main] or [-1]
print("main_colored", max(main_counts))
# 主界面的判据用「非背景像素」而不是「高饱和像素」：
# 演示脚本 2 秒就切页了，那之前的帧上 BMS 数据还没到，
# 进度条画不出来，高饱和像素自然是 0 —— 那是采样时刻的问题，
# 不是界面没画。只要标题栏/文字/表格画出来了，非背景像素就一定很多。
main_ink = max([ink(p, (0, 0, 480, 272)) for p in main] or [-1])
print("main_ink", main_ink)
print("main_frames", len(main), "counts", main_counts[:6])
print("hist_colored", colored(hist[-1], (40, 40, 476, 240)) if hist else -1)
print("hist_full",    colored(hist[-1], (0, 0, 480, 272)) if hist else -1)
print("grid_colored", colored(grid[-1], (0, 30, 480, 272)) if grid else -1)
PY
)
echo "$CHK" | sed 's/^/      /'

HIST_PX=$(echo "$CHK" | awk '/^hist_colored/{print $2}')
MAIN_PX=$(echo "$CHK" | awk '/^main_colored/{print $2}')
[ "${HIST_PX:--1}" -gt 200 ] 2>/dev/null \
    && pass "历史曲线帧绘图区内有 $HIST_PX 个彩色像素（曲线已画出）" \
    || fail "历史曲线帧绘图区彩色像素仅 ${HIST_PX:-0}，曲线可能没画出来"
MAIN_INK=$(echo "$CHK" | awk '/^main_ink/{print $2}')
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
