#!/bin/sh
# ============================================================================
#  GB/T 27930 BMS CAN 监控终端 —— I.MX6ULL 开发板基准测试
#
#  用法（在开发板上）：
#      cd ~/CAN && make
#      sudo sh tools/bench_board.sh can0 60            # 默认：测充电中
#      sudo sh tools/bench_board.sh can0 60 idle       # 测待机
#
#  两种模式的区别只有一个参数：
#      charging（默认）  给 can_monitor 加 --auto-start，它会主动发起充电，
#                        采到的是充电阶段的工作点（报文密、负载高）
#      idle              不加 --auto-start，本程序停在空闲态、STM32 也停在
#                        待机，采到的是待机流量（心跳 1 Hz + BSM 4 Hz）
#  两种都要采，文档里的实测表才完整。
#
#  前提：CAN 总线已接好、STM32 已上电。
#        charging 模式下脚本会自己用 --auto-start 发起一次充电，并在充电过程中
#        采样，不需要你先手工点屏上的「充电」按钮。
#        idle 模式下不发起充电，两端都停在空闲态，采的是待机流量。
#
#  把整段输出发回即可。
#
#  为什么必须在板子上跑：CPU 占用率 / SQLite 写入速率 / 内存占用都跟硬件
#  强相关（Cortex-A7 792MHz + TF 卡 vs x86 + SSD，差一个数量级）。
# ============================================================================

IFACE=${1:-can0}
SECS=${2:-60}
MODE=${3:-charging}
PORT="$IFACE"

log()   { echo "$@"; }
hline() { echo "----------------------------------------------------------------"; }

[ "$(id -u)" = "0" ] || { echo "请用 sudo 运行：sudo sh $0 $IFACE $SECS"; exit 1; }

cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null; }
rss_kb()    { awk '/VmRSS/{print $2}' "/proc/$1/status" 2>/dev/null; }
HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)

hline
log " GB/T 27930 监控终端 —— I.MX6ULL 基准测试"
hline
log ""
log "【0】环境信息"
log "  日期        : $(date '+%Y-%m-%d %H:%M:%S')"
log "  内核        : $(uname -sr)"
log "  硬件        : $(awk -F': ' '/Hardware/{print $2; exit}' /proc/cpuinfo 2>/dev/null)"
log "  CPU 型号    : $(awk -F': ' '/model name|Processor/{print $2; exit}' /proc/cpuinfo 2>/dev/null)"
log "  CPU 核数    : $(grep -c '^processor' /proc/cpuinfo)"
log "  CPU 频率    : $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo '不可读') kHz"
log "  内存总量    : $(awk '/MemTotal/{printf "%.1f MB", $2/1024}' /proc/meminfo)"
log "  CLK_TCK     : $HZ"
log ""

# ---------- 1. 接口 ----------
log "【1】准备 CAN 接口（$PORT）"
if ! ip link show "$PORT" >/dev/null 2>&1; then
    log "  [失败] 接口 $PORT 不存在。先按 README 三.0 节启用设备树插件。"
    exit 1
fi
ip link set "$PORT" down 2>/dev/null
ip link set "$PORT" type can bitrate 250000 restart-ms 100 2>/dev/null
if ip link set "$PORT" up 2>/dev/null; then
    sleep 1
    log "  接口状态    : $(ip -br link show "$PORT" 2>/dev/null)"
    log "  位定时      : $(ip -details link show "$PORT" 2>/dev/null | awk '/bitrate/{print $1, $2; exit}')"
else
    log "  [失败] 无法 up $PORT"
    exit 1
fi
log ""

# ---------- 2. 启动本程序并发起充电 ----------
if [ "$MODE" = "idle" ]; then
    log "【2】启动 can_monitor（**待机模式**：不发起充电，两端都停在空闲态）"
    AUTO=""
else
    log "【2】启动 can_monitor 并发起充电（--auto-start）"
    AUTO="--auto-start"
fi
if [ ! -x ./can_monitor ]; then
    log "  [跳过] 当前目录没有 can_monitor，请先 make"
    exit 1
fi
rm -f /tmp/_bench.db /tmp/_bench.db-wal /tmp/_bench.db-shm
# -n        接口上面已经配好，不要再改
# -v        逐报文日志（判定多帧重组是否成功的依据）
# -u        滚动日志，便于事后统计
# --no-gui  不占 framebuffer，测的是纯 CAN+存储负载
# --auto-start  上电即发 CHM，不用人点按钮
./can_monitor -i "$PORT" -n -u -v --no-gui $AUTO \
              -d /tmp/_bench.db > /tmp/_bench_run.log 2>&1 &
PID=$!
sleep 5

if ! kill -0 "$PID" 2>/dev/null; then
    log "  [失败] can_monitor 起不来，日志尾部："
    tail -8 /tmp/_bench_run.log | sed 's/^/    /'
    exit 1
fi
log "  已启动，PID=$PID"
STATE=$(grep -a '状态切换' /tmp/_bench_run.log | tail -1 | sed 's/.*状态切换: //')
log "  当前状态    : ${STATE:-未识别}"
log ""

# ---------- 3. 充电过程中的总线帧率 ----------
log "【3】总线帧率（连续数 ${SECS} 秒，模式=$MODE）"
if command -v candump >/dev/null 2>&1; then
    START=$(date +%s)
    timeout "$SECS" candump "$PORT" > /tmp/_bench_frames.txt 2>/dev/null
    END=$(date +%s)
    N=$(wc -l < /tmp/_bench_frames.txt)
    DUR=$((END - START)); [ "$DUR" -le 0 ] && DUR=1
    log "  采样时长    : ${DUR} 秒"
    log "  收到帧数    : ${N}"
    log "  平均帧率    : $((N / DUR)) 帧/秒"
    log "  总线负载    : $(awk "BEGIN{printf \"%.2f\", $N*150/($DUR*250000)*100}") %  （按每帧约 150 位估算）"
    if [ "$N" -gt 0 ]; then
        log ""
        log "  各 ID 帧数前 12："
        awk '{print $2}' /tmp/_bench_frames.txt | sort | uniq -c | sort -rn | head -12 | sed 's/^/    /'
    else
        log "  [注意] 一帧都没收到。查：STM32 上电？J8 跳线？CANH/CANL 接对？"
    fi
    rm -f /tmp/_bench_frames.txt
else
    log "  [跳过] 没有 candump"
fi
log ""

# ---------- 4. 同一次运行里的 CPU / 内存 ----------
log "【4】进程 CPU 与内存（同一次运行，模式=$MODE，采样 ${SECS} 秒）"
T0=$(cpu_ticks "$PID")
sleep "$SECS"
T1=$(cpu_ticks "$PID")
RSS=$(rss_kb "$PID")
if [ -n "$T0" ] && [ -n "$T1" ]; then
    CPU=$(awk "BEGIN{printf \"%.1f\", ($T1-$T0)/$HZ/$SECS*100}")
    log "  CPU 占用    : ${CPU} %   （单核百分比）"
fi
log "  内存 RSS    : ${RSS:-?} KB"
RXLOG=$(grep -ac '收到 ' /tmp/_bench_run.log 2>/dev/null)
log "  解析日志行  : ${RXLOG:-0} 条"
DROP=$(grep -o '丢弃 [0-9]* 帧' /tmp/_bench_run.log | tail -1)
log "  缓冲丢弃    : ${DROP:-无（0 丢弃）}"
log "  最新状态    : $(grep -a '状态切换' /tmp/_bench_run.log | tail -1 | sed 's/.*状态切换: //')"
log ""

# ---------- 5. 优雅退出，拿程序自己的统计 ----------
log "【5】优雅退出并取程序统计"
kill -TERM "$PID" 2>/dev/null
sleep 3
kill -9 "$PID" 2>/dev/null
grep -aE '运行时长|接收帧数|发送帧数|错误帧数|解析报文数|数据库原始|数据库解析|数据库已关闭|关机收尾' \
     /tmp/_bench_run.log | tail -12 | sed 's/^/  /'
log ""

# ---------- 6. SQLite ----------
log "【6】SQLite 写入与读取速率"
if command -v sqlite3 >/dev/null 2>&1 && [ -f /tmp/_bench.db ]; then
    RAW=$(sqlite3 /tmp/_bench.db "select count(*) from can_raw;" 2>/dev/null)
    DATA=$(sqlite3 /tmp/_bench.db "select count(*) from charge_data;" 2>/dev/null)
    SESS=$(sqlite3 /tmp/_bench.db "select count(*) from charge_session;" 2>/dev/null)
    SIZE=$(du -k /tmp/_bench.db 2>/dev/null | awk '{print $1}')
    log "  can_raw 记录 : ${RAW:-0}"
    log "  charge_data  : ${DATA:-0}   （充电阶段每秒一条）"
    log "  charge_session: ${SESS:-0}  （每完成一次充电一条；本脚本用 --no-gui 跑，该表不会被创建，恒为 0）"
    log "  数据库体积   : ${SIZE:-?} KB"
    log ""
    log "  全表扫描 charge_data（逐条读出并求和）："
    START=$(date +%s%N)
    SUM=$(sqlite3 /tmp/_bench.db "select count(*), sum(voltage), sum(current) from charge_data;" 2>/dev/null)
    END=$(date +%s%N)
    MS=$(( (END - START) / 1000000 ))
    log "    聚合结果: $SUM"
    log "    耗时    : ${MS} ms"
    if [ "${DATA:-0}" -gt 0 ] && [ "$MS" -gt 0 ]; then
        log "    约 $(( DATA * 1000 / MS )) 条/秒"
    fi
else
    log "  [跳过] 没有 sqlite3 或数据库没生成"
fi
log ""

# ---------- 7. 系统 ----------
log "【7】系统整体负载"
log "  负载均值    : $(cut -d' ' -f1-3 /proc/loadavg)"
log "  空闲内存    : $(awk '/MemAvailable/{printf "%.1f MB", $2/1024}' /proc/meminfo)"
log "  TF 卡顺序写 : $(dd if=/dev/zero of=/tmp/_tf_test bs=1M count=32 conv=fdatasync 2>&1 | tail -1)"
rm -f /tmp/_tf_test
log ""

hline
log " 测试结束（模式=$MODE）。请把上面【0】~【7】的整段输出发回。"
if [ "$MODE" = "idle" ]; then
    log " idle 模式下【3】只应看到 18FFF4F4（心跳 1 Hz）与 181356F4（BSM 4 Hz）两个 ID，"
    log " 合计约 5 帧/秒 —— 这正是待机该有的画面。"
else
    log " 如果【3】只看到 18FFF4F4 和 181356F4 两个 ID，说明充电没起来 ——"
    log " 检查 STM32 是否已在待机（屏幕有刷新）、以及它是否收到了 CHM。"
fi
hline
