#!/bin/bash
# ============================================================================
#  can_link_test.sh —— CAN 物理链路一键排查（在 I.MX6ULL 开发板上运行）
#
#  适用场景：STM32 侧一直报
#              CAN总线: TEC=128 REC=0 LEC=3(ACK错误(无人应答))
#            即「我在发，但总线上没人应答，而且我一帧也收不到」，
#            而对端也收不到 —— 说明两块板子不在同一条差分对上。
#
#  本脚本只用软件手段，不需要万用表，逐项排除：
#    ① 接口是否存在 / 是否 UP / 控制器状态与 tx,rx 错误计数
#    ② 内核有没有 probe 到 FlexCAN 控制器
#    ③ 回环自测（控制器内部自收自发，不经过收发器）
#    ④ 发帧测试（restart-ms=0），看会不会 Bus-Off
#    ⑤ 短路测试提示（判断线到底通不通）
#
#  用法：bash can_link_test.sh [can0]
# ============================================================================

IF="${1:-can0}"

echo "======================================================================"
echo " CAN 物理链路排查   接口=$IF   $(date)"
echo "======================================================================"

echo
echo "【1】接口状态与控制器错误计数"
echo "   重点看:前面是 UP 还是 DOWN；can state 是什么；berr-counter tx/rx 是多少"
for i in can0 can1; do
    echo "  --- $i ---"
    ip -details -statistics link show "$i" 2>&1 | head -8 | sed 's/^/    /'
done

echo
echo "【2】内核 FlexCAN 控制器"
echo "   期望看到 'flexcan 2090000.can can0: device registered' 之类的行；"
echo "   一行都没有说明设备树插件没生效。"
dmesg 2>/dev/null | grep -i -E 'flexcan|can[0-9]' | tail -12 | sed 's/^/    /'

echo
echo "【3】回环自测（控制器内部自收自发，不经过收发器）"
echo "   收到 0 帧 => i.MX 控制器/驱动本身有问题，跟接线无关"
echo "   收到 >=1 帧 => 控制器正常，问题在收发器或接线"
sudo ip link set "$IF" down 2>/dev/null
sudo ip link set "$IF" type can bitrate 250000 restart-ms 0 loopback on 2>&1 | sed 's/^/    /'
sudo ip link set "$IF" up
rm -f /tmp/can_lb.txt
( timeout 4 candump "$IF" > /tmp/can_lb.txt 2>&1 ) &
sleep 1
cansend "$IF" 1826F456#0100010000000000 2>&1 | sed 's/^/    /'
sleep 3
echo "    >>> 回环收到帧数 = $(wc -l < /tmp/can_lb.txt 2>/dev/null)"
sed 's/^/        /' /tmp/can_lb.txt 2>/dev/null

echo
echo "【4】发帧测试（关闭自动恢复，看控制器会不会 Bus-Off）"
echo "   会 Bus-Off  => i.MX 的帧确实发到了总线上（只是没人 ACK）"
echo "   一直 ERROR-ACTIVE 且 berr-counter 不动 => i.MX 的帧根本没出去"
sudo ip link set "$IF" down 2>/dev/null
sudo ip link set "$IF" type can bitrate 250000 restart-ms 0 loopback off 2>&1 | sed 's/^/    /'
sudo ip link set "$IF" up
sudo ip link set "$IF" txqueuelen 1000 2>/dev/null
echo "    发送前:"; ip -details link show "$IF" 2>&1 | sed -n '1p;3p' | sed 's/^/      /'
for n in 1 2 3; do
    cansend "$IF" 1826F456#0100010000000000 2>&1 | sed 's/^/      /'
    sleep 0.3
done
sleep 1
echo "    发送后:"; ip -details link show "$IF" 2>&1 | sed -n '1p;3p' | sed 's/^/      /'

echo
echo "【5】恢复正式配置（250 kbps + Bus-Off 自动恢复）"
sudo ip link set "$IF" down 2>/dev/null
sudo ip link set "$IF" type can bitrate 250000 restart-ms 100 2>&1 | sed 's/^/    /'
sudo ip link set "$IF" up
ip -details link show "$IF" 2>&1 | sed -n '1p;3p' | sed 's/^/    /'

echo
echo "======================================================================"
echo " 【6】下一步：判断线到底通不通（不需要万用表）"
echo "======================================================================"
cat <<'TIP'
  测试期间让 STM32 保持上电运行（它会每秒发一帧心跳，串口会打印
  'CAN总线: TEC=.. REC=.. LEC=..'）。

  做法：在 I.MX 板的 CAN 端子上，用一根杜邦线把 CAN_H 和 CAN_L 直接短接，
        然后观察 STM32 串口那一行：

    A) STM32 的 REC 从 0 开始上涨，或 LEC 从 3(ACK错误) 变成
       1(填充错误)/2(格式错误)/5(位显性错误)
         => 线是通的！STM32 的电平确实到达了 I.MX 的端子。
            问题出在 I.MX 的收发器/控制器，不在接线。

    B) STM32 的那一行完全没变化（还是 TEC 涨、REC=0、LEC=3）
         => 线不通。检查：
              * 插的是不是 CAN 端子（丝印 CAN_H/CAN_L，不是 485_A/485_B）
              * 螺丝/按压端子有没有真的夹住线芯
              * 杜邦线本身是否断线（换两根再试）
              * 是不是插在了排针而不是端子上

  短路 CANH-CANL 对 TJA1050 这类收发器是安全的（总线引脚有短路保护），
  测完把短接线拿掉即可。
TIP
echo
