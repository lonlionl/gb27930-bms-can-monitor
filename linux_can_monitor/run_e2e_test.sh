#!/bin/bash
# ============================================================================
#  run_e2e_test.sh —— 无硬件端到端联调自动测试
#
#  作用：在没有 STM32 板子、也没有真实 CAN 收发器的情况下，
#        用内核自带的 vcan 虚拟 CAN 接口 + Python 版 BMS 模拟器，
#        把「can_monitor（充电机侧）」的完整链路跑一遍并校验数据库。
#
#  用法：
#      ./run_e2e_test.sh                  # 默认 vcan0，自动尝试免 sudo（用户命名空间）
#      sudo ./run_e2e_test.sh             # 有 root 权限时直接创建 vcan0
#      ./run_e2e_test.sh can0 30          # 用真实 can0 接口，跑 30 秒
#
#  退出码：0 = 全部通过；非 0 = 有检查项失败
# ============================================================================
set -u

IFNAME="${1:-vcan0}"
DURATION="${2:-16}"
WORK="$(cd "$(dirname "$0")" && pwd)"
# 日志与数据库一律用 mktemp 生成。
# 【为什么不用固定的 /tmp/xxx】这些脚本常常一会儿用 sudo 跑、一会儿用普通
# 用户跑。固定路径如果上一次是 root 建的，这一次的重定向就会"权限不够"而
# 失败 —— 表现出来是莫名其妙的"编译失败"或"日志是空的"，其实命令根本没跑。
DB="$(mktemp /tmp/gb27930_e2e.XXXXXX.db)"
MONLOG="$(mktemp /tmp/gb27930_mon.XXXXXX.log)"
BMSLOG="$(mktemp /tmp/gb27930_bms.XXXXXX.log)"
BUILD_LOG="$(mktemp /tmp/gb27930_build.XXXXXX.log)"

PASS=0
FAIL=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk()  { if [ "$1" = "0" ]; then ok "$2"; else bad "$2"; fi; }

# 执行一条只返回单个标量的 SQL。
# 优先使用 sqlite3 命令行工具；若系统只装了 SQLite 运行库而没有 CLI
# （Debian/Ubuntu 上 sqlite3 命令属于独立的 sqlite3 包），
# 则回退到 python3 内置的 sqlite3 模块，保证脚本在任何环境下都能自校验。
sql_one() {
    _q="$1"
    if command -v sqlite3 >/dev/null 2>&1; then
        sqlite3 "$DB" "$_q" 2>/dev/null
    else
        python3 -c '
import sqlite3, sys
try:
    con = sqlite3.connect(sys.argv[1])
    row = con.execute(sys.argv[2]).fetchone()
    print("" if (row is None or row[0] is None) else row[0])
except Exception:
    print("")
' "$DB" "$_q"
    fi
}

# 打印“最近 N 条充电数据”的小表格（同样兼容无 sqlite3 CLI 的环境）
sql_table() {
    if command -v sqlite3 >/dev/null 2>&1; then
        sqlite3 -header -column "$DB" "$1"
    else
        python3 -c '
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
cur = con.execute(sys.argv[2])
cols = [d[0] for d in cur.description]
rows = cur.fetchall()
print("  ".join("%-20s" % c for c in cols))
for r in rows:
    print("  ".join("%-20s" % ("" if v is None else v) for v in r))
' "$DB" "$1"
    fi
}

# 清理数字之外的字符，避免把 "command not found" 之类的噪声当成数值
num() { echo "$1" | tr -dc "0-9"; }

# 等待一个后台进程结束，最多等 $2 秒；超时则 SIGKILL，避免脚本永久挂起。
# 注意：不能用 `while kill -0 $pid` 轮询来判断进程是否结束 ——
#       子进程退出后会变成僵尸进程，此时 kill -0 依然返回成功，
#       会导致脚本在这里永久等待。正确做法是用 wait 阻塞回收，
#       另起一个看门狗子进程在超时后强制结束它。
wait_pid() {
    _pid="$1"; _max="$2"; _rc=0
    ( sleep "$_max"; kill -9 "$_pid" 2>/dev/null ) &
    _wd=$!
    wait "$_pid" 2>/dev/null
    _rc=$?
    kill "$_wd" 2>/dev/null
    wait "$_wd" 2>/dev/null
    return $_rc
}

echo "======================================================================"
echo " GB/T 27930-2015 端到端联调测试   接口=$IFNAME  时长=${DURATION}s"
echo "======================================================================"

# ---------------------------------------------------------------------------
# 1. 准备 CAN 接口
# ---------------------------------------------------------------------------
if ! ip link show "$IFNAME" >/dev/null 2>&1; then
    # 只有 vcan* 才允许自动创建。can0 这类真实接口必须由
    # 「内核 CAN 控制器驱动 + 设备树节点」创建出来，脚本无权凭空造一个；
    # 否则会把「创建 vcan 失败」误报成「can0 有问题」，误导排查方向。
    case "$IFNAME" in
        vcan*) ;;
        *)
            echo "[错误] 接口 $IFNAME 不存在。"
            echo "       本脚本只会自动创建 vcan* 虚拟接口；真实 CAN 接口需要先由"
            echo "       「内核 CAN 控制器驱动 + 设备树节点」创建出来。"
            echo "       请先在开发板上运行诊断脚本定位原因："
            echo "           bash board_can_diag.sh"
            echo "       若只想验证 Linux 侧软件链路，可改用虚拟接口："
            echo "           ./run_e2e_test.sh vcan0"
            exit 3
            ;;
    esac

    echo "[1/6] 接口 $IFNAME 不存在，尝试创建 vcan ..."
    if ip link add dev "$IFNAME" type vcan 2>/dev/null; then
        ip link set "$IFNAME" up
    elif [ "${GB_E2E_NESTED:-0}" = "1" ]; then
        # 已经重新执行过一次仍然失败，直接报错退出，避免无限递归
        echo "      [错误] 无法创建 $IFNAME。内核可能没编译 CAN_VCAN，请尝试："
        echo "             sudo modprobe vcan"
        echo "             sudo ip link add dev $IFNAME type vcan && sudo ip link set $IFNAME up"
        exit 2
    else
        # 没有 CAP_NET_ADMIN 时，退回到用户命名空间（Ubuntu 默认允许非特权用户命名空间）
        echo "      无权限创建接口，改为在用户命名空间中重新执行 ..."
        GB_E2E_NESTED=1 exec unshare -rn "$0" "$@"
    fi
else
    echo "[1/6] 接口 $IFNAME 已存在"
fi
ip link set "$IFNAME" up 2>/dev/null
ip -br link show "$IFNAME" | sed 's/^/      /'

# ---------------------------------------------------------------------------
# 2. 编译
# ---------------------------------------------------------------------------
echo "[2/6] 编译 can_monitor ..."
cd "$WORK" || exit 1
make all SQLITE_CFLAGS="${SQLITE_CFLAGS:-}" SQLITE_LDFLAGS="${SQLITE_LDFLAGS:-}" \
     >"$BUILD_LOG" 2>&1
chk $? "编译 can_monitor（详见 $BUILD_LOG）"
[ -x ./can_monitor ] || { echo "编译失败，终止"; exit 1; }

# ---------------------------------------------------------------------------
# 3. 启动监控终端（-n 不修改接口配置；-u 关闭仪表盘便于脚本比对）
# ---------------------------------------------------------------------------
echo "[3/6] 启动监控终端 ..."
rm -f "$DB" "$DB"-wal "$DB"-shm
# 【参数说明，三个都不能少】
#   --auto-start  本程序不会自己上线，只有收到"开始充电"请求后才发 CHM。
#                 手动跑时这个请求来自屏上的「充电」按钮；自动化测试里没有人
#                 去点按钮，必须靠这个开关把请求置上。少了它，can_monitor 会
#                 一直停在空闲态、一帧 CHM 都不发，fake_bms.py 等不到握手只能
#                 退出，整条链路一步都走不动。
#   -v            逐条报文的收发与解析日志（"收到 BRM …" / "收到 BCP …" /
#                 "充电机输出准备就绪"）只在 verbose 下打印。下面第 5 步要靠
#                 grep 这些行来判定多帧重组是否成功，所以必须开。
#   --no-gui      默认 gui=1。CI / 无头虚拟机上没有可用的 /dev/fb0，不开这个
#                 会白打一串 "打开 framebuffer 失败" 的告警。
# 给一个足够长的自动退出时限作为兜底（避免测试脚本把进程留成孤儿），
# 真正的退出由后面的 kill -TERM 触发 —— 它与 Ctrl+C 走的是同一条
# 信号处理路径（sigaction(SIGTERM) -> g_running = 0 -> 优雅停机），
# 因此测试的正是产品化的退出流程，而不是定时器路径。
./can_monitor -i "$IFNAME" -n -u -v --no-gui --auto-start -r $((DURATION + 60)) -d "$DB" > "$MONLOG" 2>&1 &
MON_PID=$!
sleep 2

# ---------------------------------------------------------------------------
# 4. 运行 BMS 模拟器
# ---------------------------------------------------------------------------
echo "[4/6] 运行 BMS 模拟器（${DURATION}s）..."
timeout -k 3 $((DURATION + 8)) python3 fake_bms.py "$IFNAME" "$DURATION" > "$BMSLOG" 2>&1
BMS_RC=$?
chk "$BMS_RC" "BMS 模拟器正常结束"

# 停止充电 + 让统计落盘
sleep 1
echo "      向监控终端发送 SIGTERM（等价于 Ctrl+C），验证优雅停机 ..."
kill -TERM "$MON_PID" 2>/dev/null
wait_pid "$MON_PID" 30
MON_RC=$?
chk "$MON_RC" "监控终端优雅退出（与 Ctrl+C 等价路径）"

# ---------------------------------------------------------------------------
# 5. 校验状态机是否走完整个流程
# ---------------------------------------------------------------------------
echo "[5/6] 校验状态机流转 ..."
# 注意：模式串以 '-' 开头，必须加 '--' 让 grep 停止解析选项，
#       否则会被当成非法选项（grep: 不适用的选项 -- >）。
for s in "握手(HANDSHAKE)" "辨识(IDENTIFY)" "参数配置(PARAM_CONFIG)" "充电准备(READY)" "充电中(CHARGING)"; do
    if grep -q -- "-> $s" "$MONLOG"; then ok "状态迁移到达 $s"; else bad "未到达状态 $s"; fi
done
grep -q -- "BRM BMS辨识" "$MONLOG"; chk $? "解析出 BRM（J1939 多帧重组成功）"
grep -q -- "BCP 充电参数" "$MONLOG"; chk $? "解析出 BCP（J1939 多帧重组成功）"
grep -q -- "充电机输出准备就绪" "$MONLOG"; chk $? "收到 BRO 并下发 CRO=0xAA"
if grep -q -- "充电阶段报文超时" "$MONLOG"; then bad "出现充电阶段超时"; else ok "充电阶段无超时"; fi
# BCL 是标准里唯一一条 **1 s** 超时的报文（BCS 5 s、其余 5 s）。
# 单独判一次：如果 BCL 的 1 s 判据误触发，这行会直接指出问题所在。
if grep -q -- "BCL 超时" "$MONLOG"; then bad "出现 BCL 1 秒超时"; else ok "BCL 无 1 秒超时"; fi
# 注意：本场景里 BMS 模拟器到点就退出了，充电机会先按 BCL 1 s / BCS 5 s
# 超时进故障态，走不到「结束阶段发 CSD」那条路径 —— 所以这里**不**断言 CSD。

# ---------------------------------------------------------------------------
# 6. 校验数据库
# ---------------------------------------------------------------------------
echo "[6/6] 校验 SQLite 持久化 ..."
if ! command -v sqlite3 >/dev/null 2>&1; then
    echo "      提示：未安装 sqlite3 命令行工具，改用 python3 内置 sqlite3 模块读取"
fi
if [ -f "$DB" ]; then
    ok "数据库文件已生成（$(du -h "$DB" | cut -f1)）"
    NRAW=$(num "$(sql_one 'SELECT COUNT(*) FROM can_raw;')")
    NCHG=$(num "$(sql_one 'SELECT COUNT(*) FROM charge_data;')")
    V=$(sql_one 'SELECT MAX(bcs_measure_voltage) FROM charge_data;')
    S=$(sql_one 'SELECT MAX(bcs_current_soc) FROM charge_data;')
    T=$(sql_one 'SELECT MAX(bsm_max_temp) FROM charge_data;')
    NRAW=${NRAW:-0}; NCHG=${NCHG:-0}

    if [ "$NRAW" -gt 50 ]; then ok "can_raw 记录 ${NRAW} 条（>50）"; else bad "can_raw 记录过少: ${NRAW}"; fi
    if [ "$NCHG" -gt 3 ];  then ok "charge_data 记录 ${NCHG} 条（>3）"; else bad "charge_data 记录过少: ${NCHG}"; fi
    if [ -n "$V" ]; then ok "解析出的最高电压 ${V} V（应在 400~600 之间）"; else bad "电压解析为空"; fi
    if [ -n "$S" ]; then ok "解析出的最高 SOC ${S} %"; else bad "SOC 解析为空"; fi
    if [ -n "$T" ]; then ok "解析出的最高温度 ${T} ℃（应在 20~60 之间）"; else bad "温度解析为空"; fi

    echo "      ---- 最近 3 条充电数据 ----"
    sql_table "SELECT datetime(ts_us/1000000,'unixepoch','localtime') AS t,
                      round(bcs_measure_voltage,1) AS V, round(bcs_measure_current,1) AS A,
                      round(bcs_current_soc,1) AS SOC, round(bsm_max_temp,1) AS Tmax
                 FROM charge_data ORDER BY id DESC LIMIT 3;" | sed 's/^/      /'
else
    bad "数据库文件未生成"
fi

echo "----------------------------------------------------------------------"
echo " 测试结果:  PASS $PASS / FAIL $FAIL"
echo " 详细日志:  $MONLOG   $BMSLOG"
echo "======================================================================"
exit $([ "$FAIL" = "0" ] && echo 0 || echo 1)
