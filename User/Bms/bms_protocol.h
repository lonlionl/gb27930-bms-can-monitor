/**
  ******************************************************************************
  * @file    Bms/bms_protocol.h
  * @author  GB27930 BMS Simulator Project
  * @version V1.0.0
  * @date    2025
  * @brief   GB/T 27930-2015 BMS 侧协议层 —— 报文定义、BMS 充电状态机、J1939 TP 发送
  *
  * ============================ 标准要点速查 ============================
  * 标准：GB/T 27930-2015《电动汽车非车载传导式充电机与电池管理系统之间的通信协议》
  * 物理层：CAN 2.0B，250 kbps，29 位扩展帧
  * 应用层：SAE J1939-21 的 29 位标识符 + PGN + 8 字节数据域
  *
  * 29 位 ID 结构：  优先(3) | R(1) | DP(1) | PF(8) | PS(8) | SA(8)
  *   所有报文 PF < 240（PDU1），因此 PS 字段即 **目标地址**。
  *   充电机地址 = 0x56，BMS 地址 = 0xF4。
  *   优先权按标准逐条取值（4~7），不统一；R 与 DP 标准规定置 0。
  *   PGN = (DP<<16) | (R<<15) | (PF<<8) | (PDU2 ? PS : 0)
  *
  * ============================ BMS 侧报文清单 ============================
  *   下表 PGN / 优先权 / 长度 / 周期 / 29 位 ID 全部取自
  *   GB/T 27930-2015 表 3~表 7（核对基准见 tools/GB_T_27930_标准核对基准.md）。
  *  报文  方向      PGN     29 位 CAN ID     周期      长度
  *  ------------------------------------------------------------------
  *  CHM   机->B     0x2600  0x1826F456      250ms    3
  *  BHM   B->机     0x2700  0x182756F4      250ms    2
  *  CRM   机->B     0x0100  0x1801F456      250ms    8
  *  BRM   B->机     0x0200  TP 多帧           一次    41
  *  BCP   B->机     0x0600  TP 多帧           一次    13
  *  CTS   机->B     0x0700  0x1807F456      500ms    7
  *  CML   机->B     0x0800  0x1808F456      250ms    8
  *  BRO   B->机     0x0900  0x100956F4      250ms    1
  *  CRO   机->B     0x0A00  0x100AF456      250ms    1
  *  BCL   B->机     0x1000  0x181056F4       50ms    5
  *  BCS   B->机     0x1100  0x1C1156F4      250ms    9(注)
  *  BSM   B->机     0x1300  0x181356F4      250ms    7
  *  BST   B->机     0x1900  0x101956F4       10ms    4
  *  CST   机->B     0x1A00  0x101AF456       10ms    4
  *  BSD   B->机     0x1C00  0x181C56F4      250ms    7
  *
  *  注：标准表 5 规定 BCS 为 9 字节，超过单帧 8 字节上限，按标准 6.2 第 7 条
  *      应经传输协议（J1939 TP）传输。当前实现仍按 8 字节单帧发送，
  *      属已知偏差（见 bms_protocol.c 的 BCS_LEN 处说明）。
  *
  *  TP 传输协议（J1939-21）：
  *    TP.CM  PGN 0xEC00  B->机 0x1CEC56F4  机->B 0x1CECF456
  *    TP.DT  PGN 0xEB00  B->机 0x1CEB56F4  机->B 0x1CEBF456
  *    完整流程：RTS(0x10) -> CTS(0x11) -> DT(1..N) -> EndOfMsgACK(0x13)
  *
  * ============================ BMS 状态机 ============================
  *
  *   IDLE ──收到CHM──> HANDSHAKE ──(回BHM, 收到CRM)──> IDENTIFY
  *     ^                                                  │ 发送 BRM(TP)
  *     │                                                  v
  *     │                                             PARAM_CONFIG
  *     │                                                  │ 发送 BCP(TP)
  *     │                                                  v
  *     │                                            CHARGING_READY
  *     │                                                  │ 发送 BRO, 收到 CRO=0xAA
  *     │                                                  v
  *     │          收到 CST / 本地中止                    CHARGING
  *     └────────────── STOPPING <─────────────────────────┘
  *
  *   异常处理：任一阶段 5 秒内未收到期望报文 -> 回到 IDLE 并置位错误标志、
  *             通过 USART1 打印告警。
  *
  * ==================== 数据域布局（小端：低字节先发送） ====================
  *  字节序依据标准 4.4：「数据信息传输采用低字节先发送的格式。」
  *  即所有多字节字段都是**低字节在前**（小端）。读写统一走 §2 的 le16/le24。
  *  未使用的位 / 保留位按标准 7.9 填 1（见 fill_reserved()），不再填 0。
  *
  *  ① CHM      (3B) : B1-B3 协议版本号（小端 24 位，V1.1 = `01 01 00`）
  *      BHM     (2B) : B1-B2 最高允许充电总电压（0.1 V/位，小端）。
  *                     ★ 标准表 9 的 BHM **没有版本号字段**。
  *  ② CRM    (8B) : B1 辨识结果(0x00/0xAA) B2-B5 充电机编号(小端 32 位)
  *                  B6-B8 区域编码（3 字节标准 ASCII，可选项）
  *  ③ BRM   (41B) : B1-B3 版本(01 01 00) B4 电池类型 B5-B6 额定容量(0.1Ah)
  *                  B7-B8 额定总压(0.1V) B9-B12 厂商(4B ASCII)
  *                  B13-B16 电池组序号(4B) B17 年(**1985 偏移**) B18 月 B19 日
  *                  B20-B22 充电次数(3B 小端) B23 产权标识 B24 预留(填1)
  *                  B25-B41 VIN(17B ASCII)
  *                  ? 标准原文长度不自洽（表 5 说 41、表 11 相加 49），
  *                    本工程保持 41 字节，没有 SPN2576（BMS 软件版本号）。
  *  ④ BCP   (13B) : B1-B2 单体最高允许充电电压(0.01V)  B3-B4 最高允许充电电流(0.1A,-400A)
  *                  B5-B6 标称总能量(0.1kWh)  B7-B8 最高允许充电总电压(0.1V)
  *                  B9 最高允许温度(1C,-50C)  B10-B11 SOC(**0.1 %/位**)
  *                  B12-B13 当前电池电压(0.1V)
  *  ⑤ BCL    (5B) : B1-B2 电压需求(0.1V) B3-B4 电流需求(0.1A,-400A)
  *                  B5 充电模式(0x01 恒压 / 0x02 恒流)
  *                  ★ 标准表 17 的 BCL **只有这 3 个字段**，没有"允许充电电压/电流"
  *                    —— 那两个量在 BCP 里（SPN2819 / SPN2817）。
  *  ⑥ BCS  (9B/8B): B1-B2 充电电压测量值(0.1V) B3-B4 充电电流测量值(0.1A,-400A)
  *                  B5-B6 最高单体电压**及其组号**（1-12 位电压 0.01V；13-16 位组号 1/位）
  *                  B7 当前 SOC(**1 %/位**) B8-B9 估算剩余充电时间(1min，0~600)
  *                  【已知偏差】标准 9 字节超过单帧上限、应走 J1939 TP；
  *                  本工程按 **8 字节单帧**发送，缺 B9（剩余时间高字节）。
  *  ⑦ BSM    (7B) : B1 最高单体电压**所在编号**(1 偏移) B2 最高温度(1C,-50C)
  *                  B3 最高温度检测点编号(1 偏移) B4 最低温度 B5 最低温度检测点编号
  *                  B6 SPN3090~3093 四个 2 位状态字段
  *                  B7 SPN3094 绝缘 / SPN3095 连接器 / SPN3096 充电允许 + 未定义位
  *                  ★ 标准 BSM **没有电压字段**。没有检测能力的（绝缘、连接器）
  *                    一律填 **10 不可信**，不要填 00 正常冒充实测。
  *  ⑧ BRO/CRO(1B) : B1 0x00 未就绪 / 0xAA 就绪 / 0xFF 无效
  *  ⑨ BST/CST(4B) : B1 中止原因(4 个 2 位) B2-B3 故障原因(8/6 个 2 位)
  *                  B4 错误原因(2 个 2 位)。**不是整字节位掩码。**
  *  ⑩ BSD    (7B) : B1 中止 SOC(1%) B2-B3 单体**最低**电压(0.01V)
  *                  B4-B5 单体**最高**电压(0.01V) B6 **最低**温度 B7 **最高**温度
  *
  * 注：本工程全部字节偏移、分辨率、偏移量集中定义在 bms_protocol.c 顶部，
  *     与 Linux 端 gb27930.c 保持完全一致（可用 tools/gb27930_layout_check.js
  *     自动比对两端的宏定义与实际载荷字节）。
  ******************************************************************************
  */

#ifndef __BMS_PROTOCOL_H
#define __BMS_PROTOCOL_H

#include "stm32f10x.h"
#include "can.h"

/*==============================================================================
 *                              地址与 PGN
 *============================================================================*/
#define GB_ADDR_CHARGER         0x56u          /* 充电机地址 */
#define GB_ADDR_BMS             0xF4u          /* BMS 地址     */

/* PGN 全部取自 GB/T 27930-2015 表 3~表 7（6.4：PGN 第二字节为 PF 值）。
 * 注意：这些 PGN 的 PF 就是 29 位 ID 的 PF 字节，改 PGN 必须同步改下面的完整 ID。 */
#define GB_PGN_CHM              0x2600u   /* 充电机握手              PF=0x26 */
#define GB_PGN_BHM              0x2700u   /* 车辆握手                PF=0x27 */
#define GB_PGN_CRM              0x0100u   /* 充电机辨识              PF=0x01 */
#define GB_PGN_BRM              0x0200u   /* BMS 和车辆辨识（走 TP） PF=0x02 */
#define GB_PGN_TPCM             0xEC00u   /* J1939 连接管理          PF=0xEC */
#define GB_PGN_TPDT             0xEB00u   /* J1939 数据传输          PF=0xEB */
#define GB_PGN_BCP              0x0600u   /* 动力蓄电池充电参数      PF=0x06 */
#define GB_PGN_CTS              0x0700u   /* 充电机发送时间同步信息  PF=0x07 */
#define GB_PGN_CML              0x0800u   /* 充电机最大输出能力      PF=0x08 */
#define GB_PGN_BRO              0x0900u   /* 电池充电准备就绪        PF=0x09 */
#define GB_PGN_CRO              0x0A00u   /* 充电机输出准备就绪      PF=0x0A */
#define GB_PGN_BCL              0x1000u   /* 电池充电需求            PF=0x10 */
#define GB_PGN_BCS              0x1100u   /* 电池充电总状态          PF=0x11 */
#define GB_PGN_CCS              0x1200u   /* 充电机充电状态          PF=0x12 */
#define GB_PGN_BSM              0x1300u   /* 动力蓄电池状态信息      PF=0x13 */
#define GB_PGN_BST              0x1900u   /* BMS 中止充电            PF=0x19 */
#define GB_PGN_CST              0x1A00u   /* 充电机中止充电          PF=0x1A */
#define GB_PGN_BSD              0x1C00u   /* BMS 统计数据            PF=0x1C */
#define GB_PGN_CSD              0x1D00u   /* 充电机统计数据          PF=0x1D */
#define GB_PGN_BEM              0x1E00u   /* BMS 错误报文            PF=0x1E */
#define GB_PGN_CEM              0x1F00u   /* 充电机错误报文          PF=0x1F */

/*  完整 ID = (优先权<<26) | (R<<25) | (DP<<24) | (PF<<16) | (PS<<8) | SA
 *  R = DP = 0（标准 6.1/6.2），PS = 目标地址，SA = 源地址。
 *  优先权逐条来自标准表 3~表 7：BRM/BCP/BCS 为 7，BRO/CRO/BST/CST 为 4，
 *  其余为 6；**BEM/CEM 为 2（全项目优先级最高）**。 */
/*------------------------- 充电机 -> BMS 的 CAN ID -------------------------*/
#define GB_ID_CHM               0x1826F456u    /* 优先权6 PGN 0x2600 */
#define GB_ID_CRM               0x1801F456u    /* 优先权6 PGN 0x0100 */
#define GB_ID_CTS               0x1807F456u    /* 优先权6 PGN 0x0700 */
#define GB_ID_CML               0x1808F456u    /* 优先权6 PGN 0x0800 */
#define GB_ID_CRO               0x100AF456u    /* 优先权4 PGN 0x0A00 */
#define GB_ID_CCS               0x1812F456u    /* 优先权6 PGN 0x1200 */
#define GB_ID_CST               0x101AF456u    /* 优先权4 PGN 0x1A00 */
#define GB_ID_CSD               0x181DF456u    /* 优先权6 PGN 0x1D00 */
#define GB_ID_CEM               0x081FF456u    /* 优先权2 PGN 0x1F00（最高优先级）*/
#define GB_ID_TPCM_TO_BMS       0x1CECF456u    /* 充电机发给 BMS 的 TP.CM */
#define GB_ID_TPDT_TO_BMS       0x1CEBF456u    /* 充电机发给 BMS 的 TP.DT */

/*------------------------- BMS -> 充电机的 CAN ID -------------------------*/
#define GB_ID_BHM               0x182756F4u    /* 优先权6 PGN 0x2700 */
#define GB_ID_BCP               0x1C0656F4u    /* 优先权7 PGN 0x0600 */
#define GB_ID_BRO               0x100956F4u    /* 优先权4 PGN 0x0900 */
#define GB_ID_BCL               0x181056F4u    /* 优先权6 PGN 0x1000 */
#define GB_ID_BCS               0x1C1156F4u    /* 优先权7 PGN 0x1100 */
#define GB_ID_BSM               0x181356F4u    /* 优先权6 PGN 0x1300 */
#define GB_ID_BST               0x101956F4u    /* 优先权4 PGN 0x1900 */
#define GB_ID_BSD               0x181C56F4u    /* 优先权6 PGN 0x1C00 */
#define GB_ID_BEM               0x081E56F4u    /* 优先权2 PGN 0x1E00（最高优先级）*/
#define GB_ID_TPCM_FROM_BMS     0x1CEC56F4u    /* BMS 发给充电机的 TP.CM */
#define GB_ID_TPDT_FROM_BMS     0x1CEB56F4u    /* BMS 发给充电机的 TP.DT */

/*==============================================================================
 *                              上电心跳（物理层排查用）
 *  背景：BMS 在空闲态是不发任何报文的，此时如果两端物理层有问题，
 *        现场表现就是「i.MX 一直在发、STM32 什么都不发也收不到」，
 *        很难判断到底是哪一端的收发器/接线有问题。
 *
 *  置 1 后：BMS 上电后在「空闲态」每 1 秒主动发一帧心跳，一旦收到 CHM
 *           立即停止，完全不干扰正常协议流程。用它配合对端的 candump 即可
 *           把故障范围一刀切开：
 *
 *     ┌─ i.MX 的 candump 能看到心跳，且本行 TEC 保持 0
 *     │     → STM32→i.MX 方向通，且 i.MX 正常 ACK，物理层没问题
 *     ├─ i.MX 的 candump 能看到心跳，但 TEC 一路涨到 255
 *     │     → STM32 确实在驱动总线（收发器是好的），但对端没 ACK
 *     │       （对端不在线 / 只通了一半 / 对端 Bus-Off）
 *     └─ i.MX 的 candump 什么都看不到，TEC 也不动
 *           → STM32 侧收发器根本没有驱动总线（没供电/没接/芯片坏）
 *
 *  正式演示时置 0 即可，协议行为不受影响。
 *============================================================================*/
#define BMS_HEARTBEAT_ENABLE    1
#define BMS_HEARTBEAT_PERIOD    1000u
/* 刻意选一个非 GB/T 27930 的 ID（PF=0xFF 属 PDU2 群发），
 * i.MX 端的硬件滤波会丢弃它，只有 candump 能看到，不会污染协议流程 */
#define GB_ID_BMS_HEARTBEAT     0x18FFF4F4u

/*==============================================================================
 *                              时间参数（ms）
 *============================================================================*/
#define GB_T_BHM_PERIOD         250u    /* BHM / BRO 发送周期 */
#define GB_T_BCL_PERIOD         50u     /* BCL 周期（国标表 5：50 ms） */
#define GB_T_BCS_PERIOD         250u    /* BCS 周期（国标表 5：250 ms） */
#define GB_T_BSM_PERIOD         250u    /* BSM 周期（国标 250 ms）。
                                         这个宏在 BMS_Protocol_Tick() 里被真正引用。
                                         注意一处**有意保留的非国标扩展**：国标只在充电
                                         阶段发 BSM，本工程让它不分状态一直广播，这样
                                         i.MX 在待机 / 充满 / 掉电阶段也能看到实时电量。
                                         待机时 4 帧/秒、总线负载约 0.24%，可以忽略；
                                         而且待机帧不入库，不会撑大数据库。 */
#define GB_T_SIM_PERIOD         1000u   /* 电池模型仿真步长 */
#define GB_T_BST_PERIOD         10u     /* BST 重发周期（国标表 5：10 ms） */

/* 恒流转恒压的 SOC 门限，单位 0.1%（950 = 95.0%）。
 *
 * 【为什么要单独定义】这个门限有两处消费方：一是 BCL 报文里的"充电模式"字节
 * （0x01 恒压 / 0x02 恒流），二是界面上"模式"那一行的显示。早期两处各写各的
 * ——报文用 950、界面用 900——于是在 90.0%~94.9% 这一段会出现
 * "屏上显示恒流、发给充电机的报文里报恒压" 的矛盾。
 * 统一到这个宏之后就不可能再写歪。 */
#define BMS_CV_SOC_X10          950u

/*------------------------------------------------------------------------------
 * 【重要】发送周期配置说明（已按 GB/T 27930-2015 对齐）
 *
 *   本工程早期为了少刷串口、少写数据库，把 BCL 放宽到 1 秒、BCS/BSM 放宽到
 *   1 秒，比国标慢 20 倍 / 4 倍。这跟"按国标实现"的定位是矛盾的，已全部改回：
 *       BCL 50 ms   BCS 250 ms   BSM 250 ms
 *   改回后的工作量级（**按新周期推算，尚未上板实测**）：
 *       充电中约 32 帧/秒（BCL 20 + BCS 4 + BSM 4 + CRO 4），
 *       总线负载约 1.9%（250 kbps），串口约 28 行/秒（115200 下约 11% 占用）。
 *   唯一要留意的是数据库：can_raw 按会话落库，帧率提高约 5 倍后
 *   增长量级约 15~20 MB/小时（同样是推算值）。
 *   真实数字请烧录后用 tools/bench_board.sh 采集，别拿这一段当实测。
 *
 *   下面这段是改造前的说明，保留作为记录。
 *   GB/T 27930-2015 规定充电阶段 BCL 周期为 50 ms、BCS/BSM 周期为 250 ms。
 *   本工程按项目需求文档把 BCL / BSM 周期放宽到 1 s / 2 s，
 *   目的是让 USART1 的日志输出更容易观察、也让 Linux 端仪表盘曲线更清晰。
 *   如需严格按国标周期运行，只需把上面两个宏改为 50 / 250 即可，
 *   协议组包与状态机代码无需任何修改。
 *----------------------------------------------------------------------------*/

#define GB_T_HANDSHAKE_TIMEOUT  5000u   /* 等待 CRM 超时（标准第 8 章通用 5 s） */
#define GB_T_IDENTIFY_TIMEOUT   5000u   /* 等待 CML 超时（通用 5 s） */
#define GB_T_PARAM_TIMEOUT      5000u   /* 等待 CRO 超时（通用 5 s） */
#define GB_T_CHARGE_TIMEOUT     5000u   /* 充电阶段"任意充电机报文"刷新超时（通用 5 s） */
#define GB_T_CCS_TIMEOUT        1000u   /* CCS 专用超时：标准原文「如果 BMS 在 **1 s**
                                         * 内没有收到该报文，即为超时错误，BMS 应立即
                                         * 结束充电。」—— 这是**逐报文**的特殊规定，
                                         * 不能套用通用 5 s（CCS 周期本身只有 50 ms）。 */
#define GB_T_TP_TIMEOUT         1000u   /* J1939 TP 流控/应答超时 */
#define GB_T_TP_RETRY           3u      /* TP 失败重试次数 */

/*------------------------- 协议取值常量（**只能定义在这里**） --------------
 * 标准表 15 / 表 16：BRO、CRO 的 1 字节取值；
 * 标准表 17：BCL 的充电模式；
 * 标准表 18：BCS 的 B8（本工程不估算剩余充电时间，按 7.9 发 1）。
 *
 * ★ 这几个原本在 bms_protocol.c 里**也各写了一份** —— 值一样时能编过、
 *   看不出问题，但只要改一处忘一处就是静默的语义漂移（两处都还能编过）。
 *   现在一律只在这里定义，.c 里不再重复。
 *   校验脚本 tools/gb27930_layout_check.js 会检查"同名宏定义两次"。 */
#define GB_READY_NOT_READY      0x00u   /* 未做好 / 未完成 */
#define GB_READY_READY          0xAAu   /* 完成准备       */
#define GB_READY_INVALID        0xFFu   /* 无效           */
#define GB_BCL_MODE_CV          0x01u   /* 充电模式：恒压（SPN3074 = 0x01） */
#define GB_BCL_MODE_CC          0x02u   /* 充电模式：恒流（SPN3074 = 0x02） */
#define GB_BCS_REMAIN_ABSENT    0xFFu   /* BCS 的 B8：该字段无有效值（本工程不估算） */

/*==============================================================================
 *                              调试开关
 *============================================================================*/
/* 置 1 打印每一帧收发的详细十六进制内容；置 0 只打印状态与关键事件 */
#define BMS_DEBUG_FRAME_LOG     1
/* 置 1 在每次状态切换/异常时打印；置 0 关闭全部协议日志 */
#define BMS_DEBUG_ENABLE        1

#if (BMS_DEBUG_ENABLE)
  #define BMS_LOG(fmt, ...)     printf(fmt, ##__VA_ARGS__)
#else
  #define BMS_LOG(fmt, ...)     do { } while (0)
#endif

/*==============================================================================
 *                              BMS 状态机
 *============================================================================*/
typedef enum
{
    BMS_ST_IDLE = 0,          /* 空闲：等待充电机 CHM 握手报文 */
    BMS_ST_HANDSHAKE,         /* 握手：已回 BHM，等待 CRM 辨识报文 */
    BMS_ST_IDENTIFY,          /* 辨识：发送 BRM（多帧），等待 CTS/CML */
    BMS_ST_PARAM_CONFIG,      /* 参数配置：发送 BCP（多帧），等待 CRO */
    BMS_ST_CHARGING_READY,    /* 充电准备：周期发 BRO，等待 CRO=0xAA */
    BMS_ST_CHARGING,          /* 充电中：周期发 BCL/BCS/BSM */
    BMS_ST_STOPPING,          /* 结束：发送 BST/BSD 后回空闲 */
    BMS_ST_FAULT              /* 故障：等待自动复位 */
} BMS_State_t;

/** BMS 错误码 */
typedef enum
{
    BMS_ERR_NONE = 0,
    BMS_ERR_CRM_TIMEOUT,      /* 未收到 CRM */
    BMS_ERR_CML_TIMEOUT,      /* 未收到 CML */
    BMS_ERR_CRO_TIMEOUT,      /* 未收到 CRO */
    BMS_ERR_CHARGE_TIMEOUT,   /* 充电阶段"任意充电机报文"停止刷新（通用 5 s） */
    BMS_ERR_CCS_TIMEOUT,      /* 未收到 CCS（标准逐报文规定：**1 s**） */
    BMS_ERR_TP_FAIL,          /* J1939 TP 传输失败 */
    BMS_ERR_CHARGER_ABORT,    /* 充电机主动中止（收到 CST） */
    BMS_ERR_BUS_OFF           /* CAN 总线关闭 */
} BMS_Error_t;

/*==============================================================================
 *                              模拟电池参数
 *============================================================================*/
#define SIM_CELL_COUNT          160       /* 单体数量（512V / 3.2V = 160 串） */
#define SIM_RATED_CAP_X10       1000      /* 额定容量 100.0 Ah（0.1Ah 单位） */
#define SIM_RATED_V_X10         5120      /* 额定总电压 512.0 V（0.1V 单位）
                                           * 折算单体标称 = 512.0 / 160 = 3.20 V */
#define SIM_MAX_CELL_V_X100     365       /* 单体最高允许充电电压 3.65 V（0.01V）
                                           * = SIM_END_V_X10(584.0V) / SIM_CELL_COUNT
                                           * BCP 的 B1-B2，也是 BSM 判"单体电压过高"的门限 */
#define SIM_FLOOR_V_X10         3200      /* 电池模型的**总压下限** 320.0 V（0.1V 单位）
                                           * Sim_Step() 里对开路电压做钳位用的就是这个值，
                                           * 折算单体 = 320.0 / SIM_CELL_COUNT = 2.00 V */
#define SIM_MIN_CELL_V_X100     200       /* 单体"电压过低"门限 2.00 V（0.01V）
                                           * = SIM_FLOOR_V_X10 / SIM_CELL_COUNT
                                           * 只用于 BSM 的 SPN3090 判"单体电压过低"。
                                           *
                                           * 【取值理由】本模型的开路电压被钳在
                                           *   [SIM_FLOOR_V_X10, SIM_END_V_X10]
                                           * = [320.0 V, 584.0 V]，即单体 [2.00 V, 3.65 V]，
                                           *   标称 3.20 V（= SIM_RATED_V_X10 / SIM_CELL_COUNT）。
                                           * 门限取模型自身的下限 2.00 V，含义是"电压已经
                                           * 掉到本模型允许的最低值"。
                                           * **标准未规定该门限**，这是与模型自洽的取值。
                                           *
                                           * 注：磷酸铁锂常见的放电截止电压是 2.5 V，本模型的
                                           * 钳位下限比它更低，所以这个状态位在正常充放电过程中
                                           * **不会误触发** —— 这是符合预期的：它只在电压真的掉到
                                           * 模型下限时才置位，而不是随便找个值让它"看起来在工作"。 */
#define SIM_MAX_CURRENT_X10     1000      /* 最高允许充电电流 100.0 A */
#define SIM_ENERGY_X10          512       /* 标称总能量 51.2 kWh */
#define SIM_MAX_TOTAL_V_X10     5840      /* 最高允许充电总电压 584.0 V */
#define SIM_MAX_TEMP_C          55        /* 最高允许温度 55 ℃ */
#define SIM_START_SOC_X10       450       /* 初始 SOC 45.0 % */
#define SIM_START_V_X10         4800      /* 初始总压 480.0 V */
#define SIM_END_V_X10           5840      /* 截止总压 584.0 V */
#define SIM_SOC_STEP_X10        5         /* 仿真步长：每步 SOC +0.5 % */
#define SIM_TEMP_BASE_C         25        /* 环境温度 25 ℃ */
/* 停止充电后的掉电速率：与充电速率完全一致（用户明确要求） */
#define SIM_DISCHARGE_STEP_X10  SIM_SOC_STEP_X10
/* 充满后先保持这么多拍，把 BST/BSD 发完再开始掉电 */
#define SIM_FULL_HOLD_TICKS     5

/*==============================================================================
 *                              对外接口
 *============================================================================*/

/**
  * @brief  协议层初始化：配置 SysTick 毫秒时基、复位状态机
  */
void BMS_Protocol_Init(void);

/**
  * @brief  SysTick 中断回调（由 stm32f10x_it.c 的 SysTick_Handler 调用）
  * @note   每 1 ms 调用一次，为整个协议层提供时间基准
  */
void BMS_SysTick_Handler(void);

/**
  * @brief  处理一帧从 CAN 接收队列取出的报文（在主循环中调用）
  * @param  frame 收到的 CAN 帧
  */
void BMS_Protocol_OnFrame(const CAN_Frame_t *frame);

/**
  * @brief  协议层周期任务（在主循环中尽可能频繁地调用）
  *         内部依据毫秒时基完成：状态机推进、周期报文发送、超时判定、
  *         J1939 多帧传输的流控推进
  */
void BMS_Protocol_Tick(void);

/**
  * @brief  强制复位到空闲态（清错误标志、清 TP 上下文）
  */
void BMS_Protocol_Reset(void);

/**
  * @brief  打印当前状态、模拟电池数据与收发统计（供主循环每秒调用）
  */
void BMS_Protocol_PrintStatus(void);

/**
  * @brief  取当前状态名的字符串
  */
const char *BMS_StateStr(BMS_State_t st);

/**
  * @brief  取错误码的字符串
  */
const char *BMS_ErrorStr(BMS_Error_t err);

/*==============================================================================
 *                          UI 显示快照
 *
 *  STM32 本地界面（User/Ui/ui_app.c）不直接访问协议层的内部变量，
 *  而是通过下面这个只读快照取数：
 *    · 快照是纯数据拷贝，界面在主循环里怎么读都不会影响状态机；
 *    · 字段全部是整型（0.1V / 0.1A / 0.1% 这类缩放整数），
 *      避免在单片机上做浮点运算与浮点打印；
 *    · 显示时再按小数点位置插入 '.' 即可，ui_app.c 里有现成的格式化函数。
 *============================================================================*/
typedef struct
{
    BMS_State_t state;              /* 状态机当前状态                       */
    BMS_Error_t error;              /* 最近一次异常码                       */
    uint32_t    session_id;         /* 充电会话编号                         */

    uint16_t    soc_x10;            /* SOC，0.1 %（455 = 45.5 %）           */
    uint16_t    voltage_x10;        /* 总电压，0.1 V                        */
    uint16_t    current_x10;        /* 总电流，0.1 A（已去掉 -400A 偏移）   */
    uint16_t    cell_max_mv;        /* 最高单体电压，mV                     */
    uint8_t     cell_max_no;        /* 最高单体电压编号                     */
    uint8_t     temp_max_c;         /* 最高温度，℃                          */
    uint8_t     temp_min_c;         /* 最低温度，℃                          */

    uint16_t    limit_v_x10;        /* 最高允许充电总电压，0.1 V（进度条量程）*/
    uint16_t    limit_i_x10;        /* 最高允许充电电流，0.1 A（进度条量程）  */

    uint32_t    charge_seconds;     /* 本次会话累计充电时长，秒             */
    uint32_t    energy_x10;         /* 本次会话累计充电电量，0.1 kWh        */

    uint8_t     charge_mode;        /* 充电模式：1 = 恒压，2 = 恒流         */
    uint8_t     cro_ready;          /* 1 = 已收到 CRO，充电机准备就绪       */
    uint8_t     phase;              /* 充电阶段: 0 待机 1 充电中 2 充满 3 放电中 */
    uint32_t    rx_count;           /* 累计接收报文数                       */
    uint32_t    tx_count;           /* 累计发送报文数                       */
} BMS_UiSnapshot_t;

/**
  * @brief  取一份 UI 显示快照（只读，可在主循环任意时刻调用）
  * @param  out 输出缓冲，不可为 NULL
  */
void BMS_Protocol_GetUiSnapshot(BMS_UiSnapshot_t *out);

#endif /* __BMS_PROTOCOL_H */
