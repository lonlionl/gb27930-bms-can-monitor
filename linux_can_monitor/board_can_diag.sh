#!/bin/bash
# ============================================================================
#  board_can_diag.sh —— I.MX6ULL 开发板 CAN 接口诊断脚本
#
#  作用：当 `ip link show can0` 报 "Device can0 does not exist" 时，
#        一次性把所有可能的原因查清楚，避免来回试。
#
#  用法（在开发板上执行）：
#      bash board_can_diag.sh
# ============================================================================

echo "======================================================================"
echo " I.MX6ULL CAN 接口诊断   $(date)"
echo "======================================================================"

echo
echo "【1】内核版本与根文件系统"
uname -a
cat /etc/os-release 2>/dev/null | head -3
echo "当前用户: $(id -un)   (创建 vcan 需要 root 或用户命名空间支持)"

echo
echo "【2】CAN 相关内核配置（若没有 /proc/config.gz 说明内核未开 IKCONFIG_PROC）"
if [ -f /proc/config.gz ]; then
    zcat /proc/config.gz | grep -iE 'CONFIG_CAN|FLEXCAN|CAN_RAW|CAN_VCAN' | sort
else
    echo "  /proc/config.gz 不存在；尝试其它位置："
    ls /boot/config* 2>/dev/null
    for f in /boot/config-$(uname -r) /boot/config; do
        [ -f "$f" ] && { echo "  --- $f ---"; grep -iE 'CONFIG_CAN|FLEXCAN' "$f" | sort; }
    done
fi

echo
echo "【3】已加载的 CAN 相关模块"
lsmod 2>/dev/null | grep -iE 'can|flexcan' || echo "  lsmod 无 CAN 相关项"

echo
echo "【4】内核自带的 CAN 驱动（built-in 与 module）"
KB=/lib/modules/$(uname -r)
echo "  模块目录: $KB"
ls "$KB" 2>/dev/null | head -5
echo "  --- modules.builtin 中的 flexcan / can ---"
grep -iE 'flexcan|can_dev|vcan' "$KB/modules.builtin" 2>/dev/null || echo "  （未找到，或 modules.builtin 不存在）"
echo "  --- 磁盘上的 CAN 驱动模块文件 ---"
find "$KB" -path '*net/can*' -name '*.ko*' 2>/dev/null | head -20 || true
[ -z "$(find "$KB" -path '*net/can*' -name '*.ko*' 2>/dev/null)" ] && echo "  （没有任何 CAN 驱动 .ko 文件）"

echo
echo "【5】设备树中的 CAN 节点（决定控制器能不能被 probe）"
echo "  --- 名字里含 can 的节点 ---"
find /proc/device-tree -maxdepth 4 -iname '*can*' 2>/dev/null || echo "  （未找到任何 can 节点）"
echo "  --- compatible 里含 flexcan 的节点 ---"
find /proc/device-tree -name compatible 2>/dev/null | while read -r f; do
    if tr -d '\0' < "$f" 2>/dev/null | grep -qi flexcan; then
        d=$(dirname "$f")
        echo "  节点: $d"
        echo -n "    compatible: "; tr '\0' ' ' < "$f"; echo
        echo -n "    status    : "; tr -d '\0' < "$d/status" 2>/dev/null || echo "(无 status 属性)"
        echo
    fi
done

echo
echo "【6】野火 I.MX6ULL 的设备树插件（CAN 通常靠 overlay 启用）"
echo "  --- overlays 目录 ---"
ls -d /usr/lib/linux-image-*/overlays 2>/dev/null || echo "  （没有 overlays 目录）"
ls /usr/lib/linux-image-*/overlays/ 2>/dev/null | grep -i can || echo "  （overlays 里没有 can 相关 dtbo）"
echo "  --- 启动配置里现有的 dtoverlay 项 ---"
grep -rn 'dtoverlay' /boot/ 2>/dev/null | head -30 || echo "  （/boot 下没有 dtoverlay 配置）"
echo "  --- 【重点】被注释掉的 CAN 插件（最常见的根因）---"
if grep -rn '^[[:space:]]*#.*dtoverlay.*can' /boot/ 2>/dev/null; then
    echo "  ^^^ 上面这些 CAN 设备树插件处于「注释掉」状态，"
    echo "      把行首的 '#' 去掉并重启，can0 就会出现。"
else
    echo "  （没有被注释掉的 CAN 插件）"
fi
echo "  --- /boot 目录 ---"
ls /boot/ 2>/dev/null

echo
echo "【7】CAN 接口是否存在"
ip -br link 2>/dev/null
echo "  --- 尝试列出 can0 ---"
ip -details link show can0 2>&1

echo
echo "【8】vcan 虚拟接口能否创建（无收发器时的替代测试手段）"
if ip link add dev vcan0 type vcan 2>/dev/null; then
    ip link set vcan0 up
    echo "  vcan0 创建成功 —— 可以用它先验证 Linux 软件链路"
    ip -br link show vcan0
    ip link del dev vcan0 2>/dev/null
else
    echo "  vcan0 创建失败（内核未启用 CAN_VCAN，或权限不足）"
    echo "  试试: sudo modprobe vcan"
    echo "        sudo ip link add dev vcan0 type vcan && sudo ip link set vcan0 up"
fi

echo
echo "【9】dmesg 中的 CAN / FlexCAN 记录"
dmesg 2>/dev/null | grep -iE 'can|flexcan' | tail -20

echo
echo "======================================================================"
echo " 诊断结束。请把以上完整输出发回，即可定位是「驱动未编入内核」、"
echo " 「设备树节点被禁用」还是「设备树插件未启用」。"
echo "======================================================================"
