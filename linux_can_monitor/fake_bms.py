#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fake_bms.py —— 使用标准 SocketCAN 在 Linux 侧模拟 GB/T 27930-2015 的 BMS 侧，
              用于在没有 STM32 硬件时对 can_monitor（充电机侧）做端到端联调测试。

用法（需要先创建 vcan0 或真实 can0）:
    sudo ip link add dev vcan0 type vcan && sudo ip link set vcan0 up
    python3 fake_bms.py vcan0 20        # 在 vcan0 上运行 20 秒

流程与 STM32 端完全一致：
    CHM -> BHM -> CRM -> BRM(TP 41B) -> BCP(TP 13B) -> BRO -> CRO -> BCL/BCS/BSM
"""
import socket
import struct
import sys
import time
import threading

CAN_EFF_FLAG = 0x80000000
CAN_EFF_MASK = 0x1FFFFFFF

# ---- 报文集 ---------------------------------------------------------------
# 全部取自 GB/T 27930-2015 表 3~表 7（核对基准：tools/GB_T_27930_标准核对基准.md）。
# 本脚本模拟 BMS 侧，故 BMS -> 充电机用 SA=0xF4 / PS=0x56（0x...56F4），
# 接收（判断）充电机 -> BMS 的报文用 SA=0x56 / PS=0xF4（0x...F456）。
# 充电机 -> BMS：CHM 0x1826F456(3B) CRM 0x1801F456(8B) CTS 0x1807F456(7B)
#               CML 0x1808F456(8B) CRO 0x100AF456(1B) CST 0x101AF456(4B)
CHM = 0x1826F456
CRM = 0x1801F456
CTS = 0x1807F456
CML = 0x1808F456
CRO = 0x100AF456
CCS = 0x1812F456
CST = 0x101AF456
CEM = 0x081FF456
TPCM_TO_BMS = 0x1CECF456

# BMS -> 充电机：BHM 0x182756F4(2B) BCP 0x1C0656F4(13B,TP) BRO 0x100956F4(1B)
#               BCL 0x181056F4(5B) BCS 0x1C1156F4(9B标准/8B当前) BSM 0x181356F4(7B)
#               BST 0x101956F4(4B) BSD 0x181C56F4(7B)
#               BEM 0x081E56F4(4B, **优先权 2** —— 全项目最高优先级)
BHM = 0x182756F4
BCP = 0x1C0656F4
BRO = 0x100956F4
BCL = 0x181056F4
BCS = 0x1C1156F4
BSM = 0x181356F4
BST = 0x101956F4
BSD = 0x181C56F4
BEM = 0x081E56F4
TPCM_FROM_BMS = 0x1CEC56F4
TPDT_FROM_BMS = 0x1CEB56F4

PGN_BRM = 0x0200
PGN_BCP = 0x0600

# ---------------------------------------------------------------------------
# 各类报文的超时（标准是**逐报文不同**的，不能统一用 5 s）
#   BCL 标准 p.12: 充电机 1 s 内没收到即超时，应立即结束充电
#   BCS 标准 p.12: 5 s
#   CCS 标准 p.13: BMS 1 s 内没收到即超时，应立即结束充电
#   通用（第 8 章）: 除特殊规定外均为 5 s
# 本模拟器据此校验"充电机是否按时发了该发的报文"。
# ---------------------------------------------------------------------------
T_BCL_MS = 1000
T_BCS_MS = 5000
T_CCS_MS = 1000
T_GENERIC_MS = 5000


def u16(v):
    """16 位无符号整数 -> 小端 2 字节（标准 4.4：低字节先发送）"""
    return struct.pack("<H", v)


def u24(v):
    """24 位无符号整数 -> 小端 3 字节（标准 4.4：低字节先发送）"""
    return struct.pack("<I", v)[:3]


class Bms(object):
    def __init__(self, ifname):
        self.sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        self.sock.bind((ifname,))
        self.sock.settimeout(0.2)
        self.seen = {}
        self.lock = threading.Lock()
        self.running = True
        self.cts = None          # 收到的 CTS 信息
        self.ack = False

    def send(self, can_id, data):
        # 数据域不足 8 字节时，CAN 控制器要凑满 8 字节；标准 7.9 规定
        # 未规定的位 / 预留位**填充 1**，所以填充值用 0xFF 而不是 0x00。
        # DLC 仍按真实长度上报，对端按 DLC 判断有效字节数。
        data = data + b"\xff" * (8 - len(data))
        self.sock.send(struct.pack("<IB3s8s", can_id | CAN_EFF_FLAG, len(data), b"", data))

    def rx_thread(self):
        while self.running:
            try:
                raw = self.sock.recv(16)
            except socket.timeout:
                continue
            except OSError:
                break
            can_id, dlc = struct.unpack("<IB", raw[:5])
            data = raw[8:16]
            can_id &= CAN_EFF_MASK
            with self.lock:
                self.seen[can_id] = (data, time.time())
                if can_id == TPCM_TO_BMS and data[0] == 0x11:
                    self.cts = (data[1], data[2], data[3] | (data[4] << 8))
                if can_id == TPCM_TO_BMS and data[0] == 0x13:
                    self.ack = True

    def last(self, can_id):
        with self.lock:
            e = self.seen.get(can_id)
        return e[0] if e else None

    def age_ms(self, can_id):
        """距上次收到该 ID 过了多少毫秒；从未收到返回 None。

        用来按标准做**逐报文**的超时判定（BCL 1 s、BCS 5 s、CCS 1 s 各不同）。
        """
        with self.lock:
            e = self.seen.get(can_id)
        if e is None:
            return None
        return (time.time() - e[1]) * 1000.0

    def wait(self, can_id, timeout=5.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            e = None
            with self.lock:
                e = self.seen.get(can_id)
            if e:
                return e[0]
            time.sleep(0.02)
        return None

    # ---- J1939 TP 多帧发送 ----
    def send_tp(self, pgn, payload, timeout=3.0):
        n = len(payload)
        pkts = (n + 6) // 7
        self.cts = None
        d = bytes([0x10, n & 0xFF, (n >> 8) & 0xFF, pkts, 0xFF,
                   pgn & 0xFF, (pgn >> 8) & 0xFF, (pgn >> 16) & 0xFF])
        self.send(TPCM_FROM_BMS, d)

        t0 = time.time()
        while self.cts is None and time.time() - t0 < timeout:
            time.sleep(0.01)
        if self.cts is None:
            print("  [BMS] 等待 CTS 超时")
            return False
        bs, _seq, stmin = self.cts
        print("  [BMS] 收到 CTS: 允许 %d 包, 间隔 %d ms" % (bs, stmin))

        sent = 0
        seq = 1
        while sent < n:
            m = min(7, n - sent)
            self.send(TPDT_FROM_BMS, bytes([seq]) + payload[sent:sent + m])
            sent += m
            seq += 1
            if stmin:
                time.sleep(stmin / 1000.0)
        print("  [BMS] 多帧发送完成: PGN=0x%04X %d 字节 / %d 包" % (pgn, n, pkts))
        return True


def build_brm():
    """BRM BMS 和车辆辨识（41 字节，标准表 11 的字段顺序）

    ★ 标准原文长度不自洽：表 5 声明 41，表 11 逐行相加得 49。
      这里保持 41 字节，字段顺序严格按表 11：
        B1-B3 版本 / B4 电池类型 / B5-B6 额定容量 / B7-B8 额定总电压 /
        B9-B12 厂商 ASCII(4B) / B13-B16 电池组序号(4B) /
        B17 年(1985 偏移) B18 月 B19 日 / B20-B22 充电次数(3B) /
        B23 产权标识 / B24 预留 / B25-B41 VIN(17B)
      SPN2576（BMS 软件版本号 8 字节）在 41 字节之外，本工程不发送。
    """
    d = bytearray(b"\xff" * 41)       # 未写到的位按标准 7.9 填 1
    d[0:3] = bytes([0x01, 0x01, 0x00])  # B1-B3 协议版本 V1.1 = 01H,0001H
    d[3] = 0x03                       # B4 电池类型：03 磷酸铁锂
    d[4:6] = u16(1000)                # B5-B6 额定容量 100.0 Ah
    d[6:8] = u16(5120)                # B7-B8 额定总电压 512.0 V
    d[8:12] = b"CATL"                 # B9-B12 生产厂商 4 字节 ASCII
    d[12:16] = b"SN01"                # B13-B16 电池组序号
    d[16] = 2024 - 1985               # B17 生产年：1 年/位，1985 偏移
    d[17] = 6                         # B18 月
    d[18] = 18                        # B19 日
    d[19:22] = u24(128)               # B20-B22 充电次数 128（小端 3 字节）
    d[22] = 0x01                      # B23 产权标识：1 车自有
    d[23] = 0xFF                      # B24 预留（填 1）
    d[24:41] = b"LFP512V100A000001"   # B25-B41 VIN 17 字节
    return bytes(d)


def build_bcp(soc_x10, v_x10):
    """BCP 动力蓄电池充电参数（13 字节，标准表 12）

    ★ 注意 B10-B11 的 SOC 是 **0.1 %/位**（而 BCS 的 SOC 是 1 %/位）。
    """
    d = bytearray(13)
    d[0:2] = u16(365)                 # B1-B2 单体最高允许充电电压 3.65 V (0.01V)
    d[2:4] = u16(1000 + 4000)         # B3-B4 最高允许充电电流 100.0 A (-400A 偏移)
    d[4:6] = u16(512)                 # B5-B6 标称总能量 51.2 kWh (0.1kWh)
    d[6:8] = u16(5840)                # B7-B8 最高允许充电总电压 584.0 V (0.1V)
    d[8] = 55 + 50                    # B9    最高允许温度 55 ℃ (-50℃ 偏移)
    d[9:11] = u16(soc_x10)            # B10-B11 整车 SOC (0.1%/位)
    d[11:13] = u16(v_x10)             # B12-B13 当前电池电压 (0.1V)
    return bytes(d)


def build_bcs(volt_x10, i_a_x10, cell_x100, group, soc_pct, remain_min):
    """BCS 电池充电总状态（标准表 18 为 **9 字节**；本工程按 8 字节单帧）

    B1-B2 充电电压测量值 0.1 V/位
    B3-B4 充电电流测量值 0.1 A/位，-400 A 偏移
    B5-B6 最高单体电压**及其组号**（位打包）：
            1-12 位 = 电压 0.01 V/位；13-16 位 = 所在组号 1/位
    B7    当前 SOC **1 %/位**（注意与 BCP 的 0.1 %/位 不同）
    B8-B9 估算剩余充电时间 1 min/位，0~600（超 600 按 600 发）

    【已知偏差】9 字节超过 CAN 单帧 8 字节上限，按标准 6.2 注 7 应走
    J1939 TP 传输；两端当前仍按 8 字节单帧发送 —— 因此这里只发 B1~B8，
    **缺 B9（剩余充电时间的高字节）**。接收侧按 DLC>=8 容错。
    """
    d = bytearray(8)
    d[0:2] = u16(volt_x10)
    d[2:4] = u16(i_a_x10 + 4000)
    packed = (cell_x100 & 0x0FFF) | ((group & 0x0F) << 12)
    d[4:6] = u16(packed)
    d[6] = soc_pct & 0xFF
    d[7] = remain_min & 0xFF          # 只有低字节（B9 缺失，见上）
    return bytes(d)


def build_bsm(cell_no, t_max_c, t_max_no, t_min_c, t_min_no,
              cell_v_state, soc_state, ov_i_state, ov_t_state,
              insulation, connector, charge_permit):
    """BSM 动力蓄电池状态信息（**7 字节**，标准表 20）

    ★ 标准 BSM **没有电压字段**，B1 只是"最高单体电压所在编号"。

    B1    最高单体电压所在编号 1/位，**1 偏移**
    B2    最高动力蓄电池温度 1 ℃/位，-50 ℃ 偏移
    B3    最高温度检测点编号 1/位，1 偏移
    B4    最低动力蓄电池温度 1 ℃/位，-50 ℃ 偏移
    B5    最低温度检测点编号 1/位，1 偏移
    B6.1/3/5/7  单体电压过高过低 / SOC 过高过低 / 充电过电流 / 温度过高
                枚举 00 正常、01 过高(过流)、10 过低(不可信)
    B7.1/3/5/7  绝缘 / 输出连接器 / 充电允许 / 未定义
                绝缘与连接器枚举同上；充电允许 00 禁止 / 01 允许；
                未定义位按标准 7.9 填 1。
    """
    d = bytearray(7)
    d[0] = cell_no & 0xFF
    d[1] = (t_max_c + 50) & 0xFF
    d[2] = t_max_no & 0xFF
    d[3] = (t_min_c + 50) & 0xFF
    d[4] = t_min_no & 0xFF
    d[5] = ((cell_v_state & 3) << 0) | ((soc_state & 3) << 2) | \
           ((ov_i_state & 3) << 4) | ((ov_t_state & 3) << 6)
    d[6] = ((insulation & 3) << 0) | ((connector & 3) << 2) | \
           ((charge_permit & 3) << 4) | (3 << 6)      # 未定义位填 1
    return bytes(d)


def build_bsd(soc_pct, min_cell_x100, max_cell_x100, t_min_c, t_max_c):
    """BSD BMS 统计数据（**7 字节**，标准表 26）

    ★ 注意 B2-B3 是单体**最低**电压、B4-B5 才是单体**最高**电压；
      B6 是**最低**温度、B7 是**最高**温度。
    """
    d = bytearray(7)
    d[0] = soc_pct & 0xFF
    d[1:3] = u16(min_cell_x100)
    d[3:5] = u16(max_cell_x100)
    d[5] = (t_min_c + 50) & 0xFF
    d[6] = (t_max_c + 50) & 0xFF
    return bytes(d)


# BSM 状态位取值（标准表 20）：
#   00 正常 / 01 过高(过流) / 10 过低(不可信)
ST_NORMAL = 0
ST_HIGH = 1
ST_LOW = 2
ST_UNRELIABLE = 2
#   充电允许：00 禁止 / 01 允许
CHARGE_FORBIDDEN = 0
CHARGE_ALLOWED = 1


def build_bem(crm_00=ST_NORMAL, crm_aa=ST_NORMAL, cts_cml=ST_NORMAL,
              cro=ST_NORMAL, ccs=ST_NORMAL, cst=ST_NORMAL, csd=ST_NORMAL):
    """BEM BMS 错误报文（**4 字节**，标准表 28，优先权 2）

    每个"错误类别"占一个 **2 位字段**，枚举 00 正常 / 01 超时 / 10 不可信。
    B1 有两个字段（SPN3901 / SPN3902），B2/B3 各两个，B4 一个 + 6 位"其他"
    （标准标注为可选项，按 7.9 填 1）。
    """
    b1 = ((crm_00 & 3) << 0) | ((crm_aa & 3) << 2)
    b2 = ((cts_cml & 3) << 0) | ((cro & 3) << 2)
    b3 = ((ccs & 3) << 0) | ((cst & 3) << 2)
    b4 = ((csd & 3) << 0) | 0xFC          # 高 6 位"其他"按 7.9 填 1
    return bytes([b1, b2, b3, b4])


def main():
    ifname = sys.argv[1] if len(sys.argv) > 1 else "vcan0"
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

    bms = Bms(ifname)
    threading.Thread(target=bms.rx_thread, daemon=True).start()
    print("[BMS] 模拟器已启动，接口 %s，运行 %.0f 秒" % (ifname, duration))

    t_end = time.time() + duration
    soc = 450          # 45.0 %
    volt = 4800        # 480.0 V

    # ---------- 阶段 1：等待 CHM ----------
    print("[BMS] 等待 CHM 充电机握手...")
    if bms.wait(CHM, 15.0) is None:
        print("[BMS] 未收到 CHM，退出")
        bms.running = False
        return 1
    print("[BMS] 收到 CHM -> 回复 BHM")
    # 标准表 3：BHM 数据域 2 字节（B1 主版本 + B2 次版本）
    bms.send(BHM, bytes([0x01, 0x01]))

    # ---------- 阶段 2：等待 CRM，发送 BRM / BCP ----------
    if bms.wait(CRM, 5.0) is None:
        print("[BMS] 未收到 CRM，退出")
        bms.running = False
        return 1
    print("[BMS] 收到 CRM -> 发送 BRM (41B, 多帧)")
    bms.send_tp(PGN_BRM, build_brm())

    time.sleep(0.3)
    print("[BMS] 发送 BCP (13B, 多帧)")
    bms.send_tp(PGN_BCP, build_bcp(soc, volt))

    # ---------- 阶段 3：发 BRO，等待 CRO ----------
    print("[BMS] 周期发送 BRO，等待 CRO=0xAA")
    t0 = time.time()
    cro_ok = False
    while time.time() - t0 < 8.0:
        bms.send(BRO, bytes([0xAA]))
        c = bms.last(CRO)
        if c and c[0] == 0xAA:
            cro_ok = True
            break
        time.sleep(0.25)
    if not cro_ok:
        print("[BMS] 未收到 CRO=0xAA，退出")
        bms.running = False
        return 1
    print("[BMS] 收到 CRO=0xAA -> 进入充电阶段，周期上报 BCL/BCS/BSM")
    # CCS 是标准表 19 规定的充电机周期报文（50 ms），BMS 侧 1 s 收不到就是超时。
    # 进充电阶段前先确认收到过 CCS —— 收不到说明充电机侧没实现它。
    if bms.wait(CCS, 2.0) is None:
        print("[BMS] 警告: 进入充电阶段后 2 秒内未收到 CCS 充电机充电状态"
              "（标准表 19 要求充电机以 50 ms 周期发送，BMS 侧 1 s 超时）")
    else:
        print("[BMS] 已收到 CCS 充电机充电状态（50 ms 周期）")

    # ---------- 阶段 4：充电阶段 ----------
    # 【周期按标准表 5】BCL 50 ms、BCS 250 ms、BSM 250 ms。
    # 这里刻意比标准稍慢但仍远快于超时判据：
    #   BCL 标准超时 1 s，本模拟器每 0.5 s 发一次（留 2 倍余量）；
    #   BCS/BSM 标准超时 5 s，每 0.25 s 发一次。
    # 之所以不严格按 50 ms 发，是因为本脚本跑在 vcan 上、还要被
    # run_e2e_test.sh 的性能统计覆盖，发太快会把日志刷爆。
    #
    # 【BEM 错误上报】标准表 28 + 第 8 章：检测到对端报文超时就发 BEM。
    # 本模拟器按标准做**逐报文**判定，超时就周期性补发 BEM，并在日志里
    # 明确指出是哪一条超时 —— 这样充电机侧漏发报文能被立刻发现。
    t_bcl = t_bcs = t_bsm = 0.0
    t_ccs_missing_log = 0.0
    bem_count = 0
    while time.time() < t_end:
        now = time.time()
        if now >= t_bcl:
            t_bcl = now + 0.5
            soc = min(1000, soc + 5)
            volt = 4800 + (soc - 450) * (5840 - 4800) // (1000 - 450)
            cell = volt * 10 // 160
            # 标准表 17：BCL 数据域 **5 字节**
            #   B1-B2 电压需求 / B3-B4 电流需求 / B5 充电模式(1 恒压 2 恒流)
            #   ★ 没有"允许充电电压/电流"—— 那两个量属于 BCP。
            d = bytearray(5)
            d[0:2] = u16(min(5840, volt + 20))     # 电压需求 0.1 V
            d[2:4] = u16(1000 + 4000)              # 电流需求 100.0 A（含 -400A 偏移）
            d[4] = 2 if soc < 950 else 1           # 2 恒流 / 1 恒压
            bms.send(BCL, bytes(d))
        if now >= t_bcs:
            t_bcs = now + 0.25
            cell = volt * 10 // 160
            # B8 剩余充电时间：本模拟器同样不做估算，按标准 7.9 发 0xFF
            bms.send(BCS, build_bcs(volt, 987, cell, 5, soc // 10, 0xFF))
        if now >= t_bsm:
            t_bsm = now + 0.25
            # 本模拟器模拟一块"状态正常且允许充电"的电池：
            #   单体电压/SOC/过流/过温 都按真实读数判：这里恒为正常；
            #   绝缘与连接器**没有检测能力 → 如实填 10（不可信）**，
            #   不允许填 00（正常）冒充实测结果。
            bms.send(BSM, build_bsm(
                cell_no=7,
                t_max_c=38, t_max_no=7,
                t_min_c=31, t_min_no=2,
                cell_v_state=ST_NORMAL,
                soc_state=ST_HIGH if (soc // 10) >= 100 else ST_NORMAL,
                ov_i_state=ST_NORMAL,
                ov_t_state=ST_NORMAL,
                insulation=ST_UNRELIABLE,      # 无绝缘检测
                connector=ST_UNRELIABLE,       # 无连接器检测
                charge_permit=CHARGE_ALLOWED))

        # ---- 逐报文超时检查：充电机的 CCS 必须在 1 s 内刷新（标准表 19）----
        ccs_age = bms.age_ms(CCS)
        if ccs_age is None or ccs_age > T_CCS_MS:
            if now >= t_ccs_missing_log:
                t_ccs_missing_log = now + 1.0
                bem_count += 1
                print("[BMS] CCS 超时(%.0f ms > %d ms) -> 按标准发送 BEM"
                      "（SPN3905 接收充电机充电状态报文超时）"
                      % (ccs_age if ccs_age is not None else -1, T_CCS_MS))
                # B3.1-3.2 = SPN3905 置 01 超时
                bms.send(BEM, build_bem(ccs=ST_HIGH))

        time.sleep(0.02)

    # ---------- 阶段 5：中止充电 ----------
    print("[BMS] 发送 BST 中止充电")
    # 标准表 24：BST 4 字节，逐字段是 **2 位枚举**（不是整字节位掩码）：
    #   B1    中止原因（4 个 2 位）：1-2 达到所需 SOC 目标值 / 3-4 达到总电压
    #         设定值 / 5-6 达到单体电压设定值 / 7-8 充电机主动中止
    #   B2-B3 故障原因（8 个 2 位）
    #   B4    错误原因（2 个 2 位）：1-2 电流过大 / 3-4 电压异常
    # 枚举统一 00 正常 / 01 达到(异常) / 10 不可信。
    # 这里模拟"达到 SOC 目标值"：1-2 位 = 01 → 0x40。
    # B2-B3 本模拟器不做 BMS 侧故障检测，6 个字段如实填 10（不可信），
    # 未定义的最低位填 1；B4 电压/电流均正常 = 00。
    bst_fault = 0
    for shift in (14, 12, 10, 8, 6, 4):       # SPN3512 的 6 个 2 位字段
        bst_fault |= (ST_UNRELIABLE & 3) << shift
    bst_fault |= 0x000F                        # 未定义的低 4 位按 7.9 填 1
    bms.send(BST, bytes([0x40,
                         (bst_fault >> 8) & 0xFF,
                         bst_fault & 0xFF,
                         0x00]))
    time.sleep(0.2)
    # 标准表 26：BSD 数据域 **7 字节**
    bms.send(BSD, build_bsd(soc // 10, cell - 4, cell, 28, 45))
    time.sleep(0.5)

    bms.running = False
    print("[BMS] 模拟结束")
    return 0


if __name__ == "__main__":
    sys.exit(main())
