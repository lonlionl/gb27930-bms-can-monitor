#!/bin/bash
# ============================================================================
#  deploy_linux.sh —— 把 linux_can_monitor 平铺到部署目录并编译
#
#  为什么需要它
#  ------------
#  开发板上习惯的工作方式是 `cd ~/CAN && make && ./can_monitor`
#  （源码平铺在 CAN/ 顶层）。但仓库里源码在 linux_can_monitor/ 子目录。
#  这两份一旦不同步，就会出现最难查的一种假象：
#
#      "我明明改了代码，板上编译出来还是老行为。"
#
#  现场已经踩过一次：改的全在 linux_can_monitor/，而 ~/CAN 顶层的平铺副本
#  停留在几天前，于是怎么编都是旧产物。这个脚本把两件事绑在一起做，
#  并且**编完立刻自检**，让"没同步"这件事不可能悄悄发生。
#
#  用法（在虚拟机上、仓库根目录下执行）：
#      bash tools/deploy_linux.sh                # 默认铺到 ~/CAN
#      bash tools/deploy_linux.sh /root/CAN      # 铺到指定目录
#
#  之后照旧用：
#      cd ~/CAN && make && ./can_monitor
#
#  传到开发板（tar 走 ssh，绝不会像 scp -r 那样在里面多套一层目录）：
#      cd ~ && tar czf - CAN | ssh root@<板子IP> 'cd /root && tar xzf -'
# ============================================================================
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/linux_can_monitor"
DST="${1:-$HOME/CAN}"

if [ ! -d "$SRC" ]; then
    echo "错误: 找不到源码目录 $SRC"
    exit 1
fi

mkdir -p "$DST"
echo "源码: $SRC"
echo "目标: $DST"

# ---- 1) 平铺源码（只覆盖，不删除目标目录里的其它东西） ----
cp -f "$SRC"/*.c "$SRC"/*.h "$SRC"/Makefile "$DST"/
for f in "$SRC"/*.sh "$SRC"/*.py; do
    [ -e "$f" ] && cp -f "$f" "$DST"/
done
echo "[1/3] 源码已同步"

# ---- 2) 重新编译 ----
cd "$DST"
make clean >/dev/null 2>&1 || true
make

# ---- 3) 自检：证明部署目录里确实是最新代码 ----
echo
echo "[3/3] 部署自检 --------------------------------------------------"
n1=$(grep -c -- 'no-gui'                            main.c 2>/dev/null || true)
n2=$(grep -c -- 'V("[1/5]'                          main.c 2>/dev/null || true)
n3=$(grep -c -- '硬件滤波放行的报文 ID 列表'         gb27930.c 2>/dev/null || true)
printf "  main.c    含 --no-gui 选项        : %s 处  (应 > 0)\n" "$n1"
printf "  main.c    含 V(\"[1/5]\") 精简打印  : %s 处  (应 > 0)\n" "$n2"
printf "  gb27930.c 含旧 ID 列表串           : %s 处  (应为 0)\n" "$n3"

if [ "$n1" -gt 0 ] && [ "$n2" -gt 0 ] && [ "$n3" -eq 0 ]; then
    echo "  结论: 部署目录是最新代码 [OK]"
else
    echo "  结论: 部署目录仍是旧代码 [失败] —— 请检查上面的路径是否正确"
    exit 2
fi
echo "----------------------------------------------------------------"
echo
echo "运行: cd $DST && ./can_monitor"
