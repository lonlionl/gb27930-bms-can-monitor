/**
 * @file    gb27930.h
 * @brief   GB/T 27930-2015 协议解析层 —— 报文定义、物理量解析、充电机侧状态机
 *
 * ============================ 标准要点速查 ============================
 * 标准号：GB/T 27930-2015
 *         《电动汽车非车载传导式充电机与电池管理系统之间的通信协议》
 * 物理层：CAN 2.0B，250 kbps，29 位扩展帧，双绞线，120Ω 终端电阻
 * 应用层：基于 SAE J1939-21 的 29 位标识符 + 11 位 PGN + 8 字节数据域
 *
 * 29 位扩展 ID 结构（与 J1939 完全一致）：
 *   ┌──────┬────┬────┬────────┬────────┬──────────┐
 *   │ 28-26│ 25 │ 24 │  23-16 │  15-8  │   7-0    │
 *   │ 优先 │ R  │DP  │  PF    │   PS   │   SA     │
 *   └──────┴────┴────┴────────┴────────┴──────────┘
 *    优先：报文优先级，标准逐条规定（表 3~表 7），取值为 4~7，**不统一**；
 *          BRM/BCP/BCS 为 7，BRO/CRO/BST/CST 为 4，其余为 6。
 *    R/DP：标准 6.1/6.2 规定保留位 R 与数据页 DP 均置 0。
 *    PF  ：PDU 格式。PF < 240 为 PDU1，PS 表示**目标地址**；
 *          PF >= 240 为 PDU2，PS 表示组扩展（群发）。
 *          GB/T 27930 中所有报文 PF 均 < 240，因此 PS 恒为目标地址。
 *    SA  ：源地址。充电机 = 0x56，BMS = 0xF4。
 *    PGN = (DP<<16) | (R<<15) | (PF<<8) | (PDU2 ? PS : 0)
 *
 * ============================ 地址分配 ============================
 *   充电机（非车载充电机）：0x56
 *   BMS（电池管理系统）    ：0xF4
 *   广播地址               ：0xFF（本工程未使用）
 *
 * ============================ 报文清单 ============================
 * 方向说明：机→B 表示充电机发、BMS 收；B→机 表示 BMS 发、充电机收。
 * 下表 PGN / 优先权 / 长度 / 周期 / 29 位 ID 全部取自标准表 3~表 7
 * （核对基准：tools/GB_T_27930_标准核对基准.md）。
 *
 *  报文   方向    PGN     29 位 CAN ID     周期    长度   含义
 *  ─────────────────────────────────────────────────────────────────
 *  CHM    机→B   0x2600   0x1826F456      250ms    3    充电机握手
 *  BHM    B→机   0x2700   0x182756F4      250ms    2    BMS 握手
 *  CRM    机→B   0x0100   0x1801F456      250ms    8    充电机辨识
 *  BRM    B→机   0x0200   TP 多帧(见下)     1次    41    BMS 辨识
 *  BCP    B→机   0x0600   TP 多帧(见下)     1次    13    充电参数
 *  CTS    机→B   0x0700   0x1807F456      500ms    7    充电机时间同步
 *  CML    机→B   0x0800   0x1808F456      250ms    8    充电机最大输出能力
 *  BRO    B→机   0x0900   0x100956F4      250ms    1    BMS 充电准备就绪
 *  CRO    机→B   0x0A00   0x100AF456      250ms    1    充电机输出准备就绪
 *  BCL    B→机   0x1000   0x181056F4       50ms    5    电池充电需求
 *  BCS    B→机   0x1100   0x1C1156F4      250ms    9(注) 电池充电总状态
 *  BSM    B→机   0x1300   0x181356F4      250ms    7    动力蓄电池状态信息
 *  BST    B→机   0x1900   0x101956F4       10ms    4    BMS 中止充电
 *  CST    机→B   0x1A00   0x101AF456       10ms    4    充电机中止充电
 *  BSD    B→机   0x1C00   0x181C56F4      250ms    7    BMS 统计数据
 *
 *  注：标准表 5 规定 BCS 为 9 字节，超过单帧 8 字节上限，按标准 6.2 第 7 条
 *      应经传输协议（J1939 TP）传输；对端当前实现仍按 8 字节单帧发送，
 *      本端解析按 ≥8 字节容错处理，属已知偏差。
 *
 *  BRM 使用 J1939 传输协议，报文数据域的 PGN 为 0x0200（512）：
 *    TP.CM  PGN 0xEC00 -> BMS→充电机 0x1CEC56F4 / 充电机→BMS 0x1CECF456
 *    TP.DT  PGN 0xEB00 -> BMS→充电机 0x1CEB56F4 / 充电机→BMS 0x1CEBF456
 *
 * ============================ 充电时序 ============================
 *   ① 充电握手阶段：机发 CHM → BMS 回 BHM → 机发 CRM → BMS 发 BRM → 机发 CRM
 *   ② 参数配置阶段：BMS 发 BCP → 机发 CTS/CML → BMS 发 BRO → 机发 CRO
 *   ③ 充电阶段    ：BMS 周期发 BCL/BCS/BSM，机侧监控并响应
 *   ④ 充电结束阶段：BMS 发 BST（或机发 CST）→ 双方发 BSD/CSD 统计
 *
 *   关键超时（标准规定）：
 *     - 握手阶段任一报文 5 s 未收到 → 超时中止
 *     - 参数配置阶段 5 s 未完成      → 超时中止
 *     - 充电阶段 BCL/BSM 5 s 未刷新  → 超时中止
 *     - ISO/J1939 传输协议 1 s       → 组包失败
 *
 * ============================ 数据域布局表 ============================
 *  【字节序】标准 4.4：「数据信息传输采用低字节先发送的格式。」
 *  即所有多字节字段都是**小端（低字节在前）**，由 gb27930.c 的
 *  le16_put / le16_get / le24_get 统一处理。
 *  【填充】标准 7.9：本标准未规定的无效位 / 预留位一律**填充 1**，
 *  发送路径先 fill_reserved()（memset 0xFF）再写真实字段。
 *
 *  【重要】本工程的全部字节偏移、分辨率、偏移量都集中在 gb27930.c 顶部的
 *  「协议布局」常量区与本节说明中，解析/组包代码不含任何魔数。
 *  若被测设备的字段顺序与本表不同，只需改动 gb27930.c 顶部的偏移宏
 *  与 bms_protocol.c 顶部对应宏（两端保持一致即可），无需改动业务逻辑。
 *
 *  ① CHM 充电机握手（3 字节）/ BHM 车辆握手（2 字节）
 *       CHM：B1 版本号主版本  B2-B3 版本号次版本（小端）
 *             标准原文：V1.1 = byte3,byte2—0001H；byte1—01H ⇒ `01 01 00`
 *       BHM：B1-B2 最高允许充电总电压（0.1 V/位，0 偏移，小端）
 *             ★ 标准表 9 的 BHM **没有版本号字段**，只有 SPN2601 一个量。
 *       这两条报文的字节全部是有效字段，没有保留位。
 *
 *  ② CRM 充电机辨识报文（8 字节）
 *       B1    辨识结果（0x00 = BMS 不能辨识；0xAA = BMS 能辨识）  SPN2560 必须项
 *       B2-B5 充电机编号，1/位，0 偏移，0~0xFFFFFFFF               SPN2561 必须项
 *       B6-B8 充电机/充电站所在区域编码，**标准 ASCII 码**          SPN2562 **可选项**
 *
 *  ③ BRM 动力蓄电池辨识报文（41 字节，经 J1939 TP 传输）
 *       ★ 标准原文长度不自洽：表 5 声明 41，表 11 逐行相加得 49。
 *         本工程**保持 41 字节**，字段顺序按表 11 排列。
 *       B1-B3   BMS 通信协议版本号 V1.1 = `01 01 00`（小端）  SPN2565
 *       B4      电池类型 01 铅酸/02 镍氢/03 磷酸铁锂/04 锰酸锂/05 钴酸锂/
 *               06 三元/07 聚合物锂离子/08 钛酸锂/FF 其他        SPN2566
 *       B5-B6   额定容量              0.1 Ah/位, 0 偏移        SPN2567
 *       B7-B8   额定总电压            0.1 V/位,  0 偏移        SPN2568
 *       B9-B12  电池生产厂商名称      标准 ASCII 4 字节          SPN2569 可选
 *       B13-B16 电池组序号            预留，厂商自定义           SPN2570 可选
 *       B17     生产日期：年          **1 年/位，1985 年偏移**   SPN2571 可选
 *       B18     生产日期：月          1 月/位，0 偏移            SPN2571 可选
 *       B19     生产日期：日          1 日/位，0 偏移            SPN2571 可选
 *       B20-B22 电池组充电次数        1 次/位，0 偏移（小端 3B）  SPN2572 可选
 *       B23     电池组产权标识        0 = 租赁；1 = 车自有       SPN2573 可选
 *       B24     预留                                             SPN2574 可选
 *       B25-B41 车辆识别码 VIN        17 字节                    SPN2575 可选
 *       ⚠ SPN2576（BMS 软件版本号，8 字节）在 41 字节之外，本工程不发送。
 *
 *  ④ BCP 动力蓄电池充电参数（13 字节，经 J1939 TP 传输）
 *       B1-B2   单体最高允许充电电压    0.01 V/位,  0 偏移     SPN 2816
 *       B3-B4   最高允许充电电流        0.1 A/位,  -400 A 偏移 SPN 2817
 *       B5-B6   动力蓄电池标称总能量    0.1 kWh/位, 0 偏移     SPN 2818
 *       B7-B8   最高允许充电总电压      0.1 V/位,   0 偏移     SPN 2819
 *       B9      最高允许温度            1 ℃/位,   -50 ℃ 偏移  SPN 2820
 *       B10-B11 整车荷电状态 SOC        **0.1 %/位**, 0 偏移   SPN 2821
 *       B12-B13 整车动力蓄电池当前电压  0.1 V/位,   0 偏移     SPN 2822
 *       ★ BCP 的 SOC 是 0.1 %/位，BCS 的 SOC 是 1 %/位 —— 两者不同。
 *
 *  ⑤ CTS 充电机时间同步（7 字节）
 *       ★ 顺序反直觉（标准表 13 原文）：
 *         B1 秒  B2 分  B3 时  B4 日  B5 月  B6-B7 年，
 *         全部为**压缩 BCD 码**（年占 2 字节，同样是压缩 BCD）。
 *
 *  ⑥ CML 充电机最大输出能力（8 字节）
 *       B1-B2 最高输出电压 0.1 V/位, 0 偏移
 *       B3-B4 最低输出电压 0.1 V/位, 0 偏移
 *       B5-B6 最大输出电流 0.1 A/位, **-400 A 偏移**
 *       B7-B8 最小输出电流 0.1 A/位, **-400 A 偏移**
 *
 *  ⑦ BRO / CRO 准备就绪（1 字节）
 *       B1  0x00 = 未就绪/未完成,  0xAA = 就绪/完成,  0xFF = 无效
 *
 *  ⑧ BCL 电池充电需求（5 字节）
 *       B1-B2 电压需求       0.1 V/位, 0 偏移        SPN 3072
 *       B3-B4 电流需求       0.1 A/位, -400 A 偏移   SPN 3073
 *       B5    充电模式       1 = 恒压, 2 = 恒流      SPN 3074
 *       ★ 标准 BCL **只有这 3 个字段共 5 字节**。「允许充电电压/电流」属于
 *         BCP，BCL 没有 —— 本工程已把这两个字段的读写全部删除。
 *
 *  ⑨ BCS 电池充电总状态（标准表 5 为 9 字节）
 *       B1-B2 充电电压测量值 0.1 V/位,  0 偏移       SPN 3075
 *       B3-B4 充电电流测量值 0.1 A/位, -400 A 偏移  SPN 3076
 *       B5-B6 最高单体电压**及其组号**：
 *             1-12 位 = 电压（0.01 V/位, 0 偏移, 0~24 V）
 *             13-16 位 = 所在组号（**1/位**, 0 偏移, 0~15）        SPN 3077
 *       B7    当前荷电状态 SOC **1 %/位**, 0 偏移      SPN 3078
 *       B8-B9 估算剩余充电时间 1 min/位, 0 偏移, 0~600 SPN 3079
 *       【已知偏差】9 字节超过 CAN 单帧 8 字节上限，按标准 6.2 注 7 应走
 *       J1939 TP 传输；两端当前仍按 **8 字节单帧**发送，缺 B9（剩余充电
 *       时间的高字节），本端按 ≥8 字节容错解析。
 *
 *  ⑩ BSM 动力蓄电池状态信息（7 字节）
 *       ★ 标准 BSM **没有电压字段**，只有"编号"。
 *       B1    最高单体电压所在编号  1/位, **1 偏移**, 1~256   SPN 3085
 *       B2    最高动力蓄电池温度    1 ℃/位, -50 ℃ 偏移       SPN 3086
 *       B3    最高温度检测点编号    1/位, **1 偏移**, 1~128   SPN 3087
 *       B4    最低动力蓄电池温度    1 ℃/位, -50 ℃ 偏移       SPN 3088
 *       B5    最低温度检测点编号    1/位, 1 偏移, 1~128       SPN 3089
 *       B6.1  单体电压过高/过低  00 正常 / 01 过高 / 10 过低    SPN 3090
 *       B6.3  整车 SOC 过高/过低 同编码                        SPN 3091
 *       B6.5  充电过电流         00 正常 / 01 过流 / 10 不可信  SPN 3092
 *       B6.7  温度过高           00 正常 / 01 过高 / 10 不可信  SPN 3093
 *       B7.1  绝缘状态           00 正常 / 01 不正常 / 10 不可信 SPN 3094
 *       B7.3  输出连接器连接状态 同编码                        SPN 3095
 *       B7.5  充电允许           **00 禁止 / 01 允许**          SPN 3096
 *       B7.7  未定义（标准未说明）→ 按 7.9 填 1
 *
 *  ⑪ BST / CST 中止充电（4 字节）—— 每个字节都是若干 **2 位**字段
 *       B1    中止原因（4 个 2 位字段）  BST: SPN3511 / CST: SPN3521
 *       B2-B3 故障原因（8 个 2 位字段）  BST: SPN3512 / CST: SPN3522
 *       B4    错误原因（2 个 2 位字段）  BST: SPN3513 / CST: SPN3523
 *       枚举统一为 00 正常 / 01 异常 / 10 不可信。
 *       ★ 位移与掩码宏见下方 GB_BST_* / GB_CST_* 定义，
 *         **禁止再用整字节位掩码**（旧的 0x01/0x04/0x20 那套与标准不符）。
 *
 *  ⑫ BSD BMS 统计数据（7 字节）/ CSD 充电机统计数据（8 字节）
 *       BSD：B1    中止荷电状态 SOC 1 %/位, 0 偏移               SPN3601
 *            B2-B3 动力蓄电池单体**最低**电压 0.01 V/位, 0 偏移  SPN3602
 *            B4-B5 动力蓄电池单体**最高**电压 0.01 V/位, 0 偏移  SPN3603
 *            B6    动力蓄电池**最低**温度 1 ℃/位, -50 ℃ 偏移    SPN3604
 *            B7    动力蓄电池**最高**温度 1 ℃/位, -50 ℃ 偏移    SPN3605
 *       CSD：B1-B2 累计充电时间 1 min/位, 0 偏移, 0~600         SPN3611
 *            B3-B4 输出能量 0.1 kWh/位, 0 偏移, 0~1000          SPN3612
 *            B5-B8 充电机编号 1/位, **1 偏移**, 0~0xFFFFFFFF     SPN3613
 */

#ifndef GB27930_H
#define GB27930_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "can_layer.h"
#include "isotp.h"
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 *                          地址与 PGN 常量
 *============================================================================*/

#define GB_ADDR_CHARGER        0x56u    /**< 充电机地址 */
#define GB_ADDR_BMS            0xF4u    /**< BMS 地址 */
#define GB_ADDR_BROADCAST      0xFFu    /**< 广播地址 */

#define GB_PRIORITY_DEFAULT    6u       /**< GB/T 27930 统一优先级 */

/* PGN 全部取自 GB/T 27930-2015 表 3~表 7（6.4：PGN 第二字节为 PF 值）。
 * 这些 PGN 的 PF 就是 29 位 ID 的 PF 字节，改 PGN 必须同步改下面的完整 ID。 */
#define GB_PGN_CHM             0x2600u  /**< 充电机握手              PF=0x26 */
#define GB_PGN_BHM             0x2700u  /**< BMS 握手                PF=0x27 */
#define GB_PGN_CRM             0x0100u  /**< 充电机辨识              PF=0x01 */
#define GB_PGN_BRM             0x0200u  /**< BMS 辨识（数据 PGN，走 TP 传输） PF=0x02 */
#define GB_PGN_TPCM            0xEC00u  /**< J1939 连接管理          PF=0xEC */
#define GB_PGN_TPDT            0xEB00u  /**< J1939 数据传输          PF=0xEB */
#define GB_PGN_BCP             0x0600u  /**< 动力蓄电池充电参数      PF=0x06 */
#define GB_PGN_CTS             0x0700u  /**< 充电机发送时间同步信息  PF=0x07 */
#define GB_PGN_CML             0x0800u  /**< 充电机最大输出能力      PF=0x08 */
#define GB_PGN_BRO             0x0900u  /**< 电池充电准备就绪        PF=0x09 */
#define GB_PGN_CRO             0x0A00u  /**< 充电机输出准备就绪      PF=0x0A */
#define GB_PGN_BCL             0x1000u  /**< 电池充电需求            PF=0x10 */
#define GB_PGN_BCS             0x1100u  /**< 电池充电总状态          PF=0x11 */
#define GB_PGN_CCS             0x1200u  /**< 充电机充电状态          PF=0x12 */
#define GB_PGN_BSM             0x1300u  /**< 动力蓄电池状态信息      PF=0x13 */
#define GB_PGN_BST             0x1900u  /**< BMS 中止充电            PF=0x19 */
#define GB_PGN_CST             0x1A00u  /**< 充电机中止充电          PF=0x1A */
#define GB_PGN_BSD             0x1C00u  /**< BMS 中止充电统计数据    PF=0x1C */
#define GB_PGN_CSD             0x1D00u  /**< 充电机中止充电统计数据  PF=0x1D */
#define GB_PGN_BEM             0x1E00u  /**< BMS 错误报文            PF=0x1E */
#define GB_PGN_CEM             0x1F00u  /**< 充电机错误报文          PF=0x1F */

/*------------------------------- 完整 CAN ID -------------------------------*/
/*  完整 ID = (优先权<<26) | (R<<25) | (DP<<24) | (PF<<16) | (PS<<8) | SA
 *  R = DP = 0，PS = 目标地址，SA = 源地址；优先权逐条来自标准表 3~表 7。 */
/* 充电机 → BMS（目标地址 0xF4，源地址 0x56） */
#define GB_ID_CHM              0x1826F456u   /**< 优先权6 PGN 0x2600 */
#define GB_ID_CRM              0x1801F456u   /**< 优先权6 PGN 0x0100 */
#define GB_ID_CTS              0x1807F456u   /**< 优先权6 PGN 0x0700 */
#define GB_ID_CML              0x1808F456u   /**< 优先权6 PGN 0x0800 */
#define GB_ID_CRO              0x100AF456u   /**< 优先权4 PGN 0x0A00 */
#define GB_ID_CCS              0x1812F456u   /**< 优先权6 PGN 0x1200 */
#define GB_ID_CST              0x101AF456u   /**< 优先权4 PGN 0x1A00 */
#define GB_ID_CSD              0x181DF456u   /**< 优先权6 PGN 0x1D00 */
#define GB_ID_CEM              0x081FF456u   /**< 优先权**2** PGN 0x1F00（全项目最高）*/
#define GB_ID_CRM_TPCM         0x1CECF456u   /**< 充电机 → BMS 的 TP.CM */
#define GB_ID_CRM_TPDT         0x1CEBF456u   /**< 充电机 → BMS 的 TP.DT */

/* BMS → 充电机（目标地址 0x56，源地址 0xF4） */
#define GB_ID_BHM              0x182756F4u   /**< 优先权6 PGN 0x2700 */
#define GB_ID_BCP              0x1C0656F4u   /**< 优先权7 PGN 0x0600 */
#define GB_ID_BRO              0x100956F4u   /**< 优先权4 PGN 0x0900 */
#define GB_ID_BCL              0x181056F4u   /**< 优先权6 PGN 0x1000 */
#define GB_ID_BCS              0x1C1156F4u   /**< 优先权7 PGN 0x1100 */
#define GB_ID_BSM              0x181356F4u   /**< 优先权6 PGN 0x1300 */
#define GB_ID_BST              0x101956F4u   /**< 优先权4 PGN 0x1900 */
#define GB_ID_BSD              0x181C56F4u   /**< 优先权6 PGN 0x1C00 */
#define GB_ID_BEM              0x081E56F4u   /**< 优先权**2** PGN 0x1E00（全项目最高）*/
#define GB_ID_BRM_TPCM         0x1CEC56F4u   /**< BMS → 充电机的 TP.CM */
#define GB_ID_BRM_TPDT         0x1CEB56F4u   /**< BMS → 充电机的 TP.DT */

/** 充电机需要接收的 BMS→充电机 报文 ID 列表（用于硬件滤波）
 *  ★ 加了 BEM（0x081E56F4）后由 10 条变 11 条。 */
#define GB_RX_FILTER_COUNT     11

/*==============================================================================
 *                          缩放因子与偏移量
 *  以下宏全部来自 GB/T 27930-2015 附录，实现时不得改动。
 *  解析公式：物理量 = 原始整数 * 分辨率 + 偏移量
 *============================================================================*/

#define GB_RES_VOLTAGE_0_1     0.1f     /**< 电压 0.1 V/位，0 偏移 */
#define GB_RES_VOLTAGE_0_01    0.01f    /**< 电压 0.01 V/位，0 偏移（BCP 单体上限 / BCS 单体 / BSD 单体） */
#define GB_RES_CURRENT_0_1     0.1f     /**< 电流 0.1 A/位，-400 A 偏移（BCL/BCS/CML） */
#define GB_OFF_CURRENT         (-400.0f)/**< 电流偏移量 -400 A */
#define GB_RES_SOC_0_1         0.1f     /**< SOC 0.1 %/位，0 偏移（**BCP 专用**） */
#define GB_RES_SOC_1           1.0f     /**< SOC 1 %/位，0 偏移（**BCS/BSD 专用**） */
#define GB_RES_TEMP_1          1.0f     /**< 温度 1 ℃/位，-50 ℃ 偏移 */
#define GB_OFF_TEMP            (-50.0f) /**< 温度偏移量 -50 ℃ */
#define GB_RES_ENERGY_0_1      0.1f     /**< 能量 0.1 kWh/位，0 偏移 */
#define GB_RES_CAPACITY_0_1    0.1f     /**< 容量 0.1 Ah/位，0 偏移 */
#define GB_RES_TIME_1          1.0f     /**< 时间 1 min/位，0 偏移 */
#define GB_BCS_REMAIN_ABSENT   0xFFu    /**< BCS 的 B8（SPN3079 估算剩余充电时间）取值：
                                            本工程**没有剩余充电时间估算能力**，按标准 7.9
                                            「未规定的位或字段填充 1」发 0xFF，表示该项
                                            未规定 / 无有效值。
                                            ★ 不要填 600 —— 那是"至少还要 600 分钟"的
                                              溢出上限，填它等于编造一个具体数值；
                                            ★ 更不要填 0 —— 会被读成"还剩 0 分钟、
                                              马上充满"，是明确的错误信息。 */

/*------------------------------------------------------------------------------
 * BCS 的 B5-B6（SPN3077）是**位打包**字段，不是普通小端 16 位整数：
 *   bit 0-11 ：最高单体电压，0.01 V/位，0 偏移，0~24 V
 *   bit 12-15：该单体所在组号，1/位，0 偏移，0~15
 * 组包：raw16 = (group << 12) | voltage；解析反向拆。
 *---------------------------------------------------------------------------*/
#define GB_BCS_CELL_V_MASK     0x0FFFu  /**< 低 12 位 = 单体电压 */
#define GB_BCS_CELL_GROUP_MASK 0x000Fu  /**< 高 4 位 = 组号 */
#define GB_BCS_CELL_GROUP_SHIFT 12u     /**< 组号左移位数 */
#define GB_BCS_CELL_V_MAX_X100 2400u    /**< 电压上限 24.00 V，超出按上限发 */

/*------------------------------------------------------------------------------
 * CCS 充电机充电状态（标准表 19：8 字节，**周期 50 ms，充电机发、BMS 收**）
 *   B1-B2  SPN3081 电压输出值    0.1 V/位，0 偏移
 *   B3-B4  SPN3082 电流输出值    0.1 A/位，**-400 A 偏移**
 *   B5-B6  SPN3083 累计充电时间  1 min/位，0 偏移，0~600 min
 *   B7.1   SPN3929 充电允许      2 位：**00 暂停 / 01 允许**
 *   B7.3-7.8 与第 8 字节标准未定义，按 7.9 填 1
 * 【超时】标准原文：「如果 BMS 在 **1 s** 内没有收到该报文，即为超时错误，
 *   BMS 应立即结束充电。」—— 是 **BMS 侧**要判的 1 s。
 * 【表下注】收到 SPN3929 为 0 表示充电机将停止输出，为 1 表示将继续充电。
 *---------------------------------------------------------------------------*/
#define CCS_LEN                8
#define CCS_OFF_OUT_V          0     /**< B1-B2 电压输出值 0.1 V */
#define CCS_OFF_OUT_I          2     /**< B3-B4 电流输出值 0.1 A, -400 A 偏移 */
#define CCS_OFF_MINUTES        4     /**< B5-B6 累计充电时间 1 min */
#define CCS_OFF_PERMIT         6     /**< B7    充电允许（2 位字段在低两位） */
#define CCS_PERMIT_SHIFT       0u    /**< B7.1-7.2 SPN3929 */
#define CCS_MAX_MINUTES        600u  /**< 标准上限 600 min */
#define CCS_OUTPUT_PAUSE       0u    /**< 00 充电机将停止输出 */
#define CCS_OUTPUT_ALLOW       1u    /**< 01 充电机将继续充电 */

/*------------------------------------------------------------------------------
 * BEM BMS 错误报文（标准表 28：4 字节，250 ms，BMS 发、充电机收，优先权 **2**）
 *   B1.1 SPN3901 接收 SPN2560=0x00 的充电机辨识报文超时
 *   B1.3 SPN3902 接收 SPN2560=0xAA 的充电机辨识报文超时
 *   B2.1 SPN3903 接收充电机的时间同步和最大输出能力报文超时
 *   B2.3 SPN3904 接收充电机完成充电准备报文超时
 *   B3.1 SPN3905 接收充电机充电状态报文超时
 *   B3.3 SPN3906 接收充电机中止充电报文超时
 *   B4.1 SPN3907 接收充电机充电统计报文超时
 *   B4.3 其他（6 位，**可选项**，按 7.9 填 1）
 * 枚举：00 正常 / 01 超时 / 10 不可信状态。
 *---------------------------------------------------------------------------*/
#define BEM_LEN                4
#define BEM_OFF_B1             0     /**< B1 SPN3901 / SPN3902 */
#define BEM_OFF_B2             1     /**< B2 SPN3903 / SPN3904 */
#define BEM_OFF_B3             2     /**< B3 SPN3905 / SPN3906 */
#define BEM_OFF_B4             3     /**< B4 SPN3907 + 其他（6 位可选项） */
#define BEM_B1_CRM_00_SHIFT    0u    /**< B1.1-1.2 SPN3901 */
#define BEM_B1_CRM_AA_SHIFT    2u    /**< B1.3-1.4 SPN3902 */
#define BEM_B2_CTS_CML_SHIFT   0u    /**< B2.1-2.2 SPN3903 */
#define BEM_B2_CRO_SHIFT       2u    /**< B2.3-2.4 SPN3904 */
#define BEM_B3_CCS_SHIFT       0u    /**< B3.1-3.2 SPN3905 */
#define BEM_B3_CST_SHIFT       2u    /**< B3.3-3.4 SPN3906 */
#define BEM_B4_CSD_SHIFT       0u    /**< B4.1-4.2 SPN3907 */
#define BEM_B4_OTHER_FILL      0xFCu /**< B4.3-4.8 "其他"（可选项）按 7.9 填 1 */
/* 2 位枚举 */
#define BEM_ST_NORMAL          0u    /**< 00 正常 */
#define BEM_ST_TIMEOUT         1u    /**< 01 超时 */
#define BEM_ST_UNRELIABLE      2u    /**< 10 不可信状态 */

/*------------------------------------------------------------------------------
 * CEM 充电机错误报文（标准表 29：4 字节，250 ms，充电机发、BMS 收，优先权 **2**）
 *   B1.1 SPN3921 接收 BMS 和车辆的辨识报文超时
 *   B2.1 SPN3922 接收电池充电参数报文超时
 *   B2.3 SPN3923 接收 BMS 完成充电准备报文超时
 *   B3.1 SPN3924 接收电池充电总状态报文超时
 *   B3.3 SPN3925 接收电池充电要求报文超时
 *   B3.5 SPN3926 接收 BMS 中止充电报文超时
 *   B4.1 SPN3927 接收 BMS 充电统计报文超时
 *   B4.3 其他（6 位，可选项，按 7.9 填 1）
 * 注意与 BEM 的字段错位**不同**：CEM 的 B1 只有一个字段（SPN3921），
 * B2/B3 各两个，B4 一个 —— 必须按表 29 单独定义位移，不能套用 BEM 的。 */
#define CEM_LEN                4
#define CEM_OFF_B1             0     /**< B1 SPN3921 */
#define CEM_OFF_B2             1     /**< B2 SPN3922 / SPN3923 */
#define CEM_OFF_B3             2     /**< B3 SPN3924 / SPN3925 / SPN3926 */
#define CEM_OFF_B4             3     /**< B4 SPN3927 + 其他（6 位可选项） */
#define CEM_B1_IDENT_SHIFT     0u    /**< B1.1-1.2 SPN3921 */
#define CEM_B2_BCP_SHIFT       0u    /**< B2.1-2.2 SPN3922 */
#define CEM_B2_BRO_SHIFT       2u    /**< B2.3-2.4 SPN3923 */
#define CEM_B3_BCS_SHIFT       0u    /**< B3.1-3.2 SPN3924 */
#define CEM_B3_BCL_SHIFT       2u    /**< B3.3-3.4 SPN3925 */
#define CEM_B3_BST_SHIFT       4u    /**< B3.5-3.6 SPN3926 */
#define CEM_B4_BSD_SHIFT       0u    /**< B4.1-4.2 SPN3927 */
#define CEM_B4_OTHER_FILL      0xFCu /**< B4.3-4.8 "其他"（可选项）按 7.9 填 1 */
/** CEM 要上报的错误类别位掩码（把本工程的各类超时映射到 SPN3921~3927） */
#define CEM_ERR_IDENT          0x01u /**< -> SPN3921 接收 BMS 和车辆的辨识报文超时（BRM） */
#define CEM_ERR_BCP            0x02u /**< -> SPN3922 接收电池充电参数报文超时 */
#define CEM_ERR_BRO            0x04u /**< -> SPN3923 接收 BMS 完成充电准备报文超时 */
#define CEM_ERR_BCS            0x08u /**< -> SPN3924 接收电池充电总状态报文超时 */
#define CEM_ERR_BCL            0x10u /**< -> SPN3925 接收电池充电要求报文超时 */
#define CEM_ERR_BST            0x20u /**< -> SPN3926 接收 BMS 中止充电报文超时 */
#define CEM_ERR_BSD            0x40u /**< -> SPN3927 接收 BMS 充电统计报文超时 */

/** CEM 的每周期最大发送次数（250 ms x 20 = 5 s，配合故障态 10 s 自动复位） */
#define GB_T_CEM_MAX_TX        20u

/*==============================================================================
 *                  协议取值常量（**必须放在头文件里**）
 *
 *  【为什么强调这一点】这组常量原先定义在 gb27930.c 里，结果 selftest.c /
 *  uitest.c 引用它们时编译直接报 undeclared（`GB_READY_INVALID`、
 *  `GB_CRM_ID_UNKNOWN` 就是这么暴露出来的）。
 *  协议常量是**接口的一部分**，不是某个 .c 的实现细节 —— 凡是可能被别的
 *  编译单元用来做断言/比较的取值，一律定义在这里。
 *============================================================================*/

/* 标准表 15 / 表 16：BRO、CRO 的 1 字节取值 */
#define GB_READY_NOT_READY     0x00u  /**< 未做好 / 未完成 */
#define GB_READY_READY         0xAAu  /**< 完成准备       */
#define GB_READY_INVALID       0xFFu  /**< 无效           */

/* 标准表 10：CRM 的 B1 辨识结果 */
#define GB_CRM_ID_UNKNOWN      0x00u  /**< SPN2560：BMS 不能辨识 */
#define GB_CRM_ID_OK           0xAAu  /**< SPN2560：BMS 能辨识   */

/* 常用报文长度（标准表 3~表 7 的"数据长度"列）。
 * ★ 这几个会被 selftest.c 用来声明缓冲（`uint8_t bcl[BCL_LEN]`），
 *   所以必须放在头文件里 —— 放在 gb27930.c 会让 selftest 编译不过
 *   （真踩过：`BCL_LEN` undeclared）。 */
#define GB_LEN_CHM             3u     /**< 充电机握手        */
#define GB_LEN_BHM             2u     /**< 车辆握手          */
#define GB_LEN_CRM             8u     /**< 充电机辨识        */
#define GB_LEN_BRO             1u     /**< 充电准备就绪      */
#define GB_LEN_CRO             1u     /**< 输出准备就绪      */
#define GB_LEN_BCL             5u     /**< 电池充电需求      */
#define GB_LEN_BCS             8u     /**< 电池充电总状态（标准 9，本工程单帧 8） */
#define GB_LEN_BSM             7u     /**< 动力蓄电池状态信息 */
#define GB_LEN_BST             4u     /**< BMS 中止充电      */
#define GB_LEN_CST             4u     /**< 充电机中止充电    */
#define GB_LEN_BSD             7u     /**< BMS 统计数据      */
#define GB_LEN_CSD             8u     /**< 充电机统计数据    */
#define GB_LEN_BEM             4u     /**< BMS 错误报文      */
#define GB_LEN_CEM             4u     /**< 充电机错误报文    */
#define GB_LEN_CCS             8u     /**< 充电机充电状态    */

/* BCL 的充电模式（标准表 17 的 SPN3074） */
#define GB_BCL_MODE_CV         0x01u  /**< 恒压 */
#define GB_BCL_MODE_CC         0x02u  /**< 恒流 */

/*==============================================================================
 *                          状态机定义（充电机侧）
 *============================================================================*/

typedef enum
{
    GB_ST_IDLE = 0,          /**< 空闲：低压辅助上电，等待 BMS 上线 */
    GB_ST_HANDSHAKE,         /**< 握手阶段：周期发 CHM，等 BHM */
    GB_ST_IDENTIFY,          /**< 辨识阶段：发 CRM，等 BRM（多帧） */
    GB_ST_PARAM_CONFIG,      /**< 参数配置阶段：等 BCP，发 CTS/CML */
    GB_ST_CHARGING_READY,    /**< 充电准备：等 BRO，发 CRO */
    GB_ST_CHARGING,          /**< 充电中：接收 BCL/BCS/BSM */
    GB_ST_STOPPING,          /**< 结束阶段：发 CST，等 BST/BSD */
    GB_ST_FAULT,             /**< 故障态：等待人工复位 */
    GB_ST_MAX
} gb_state_t;

/** 异常码（用于日志、数据库与 UI 告警） */
typedef enum
{
    GB_ERR_NONE = 0,
    GB_ERR_HANDSHAKE_TIMEOUT,   /**< 握手超时（CHM/BHM 5 s 无交互） */
    GB_ERR_IDENTIFY_TIMEOUT,    /**< 辨识超时（BRM 未收到） */
    GB_ERR_PARAM_TIMEOUT,       /**< 参数配置超时（BCP 未收到） */
    GB_ERR_READY_TIMEOUT,       /**< 准备就绪超时（BRO/CRO 60 s 未就绪） */
    GB_ERR_CHARGE_TIMEOUT,      /**< 充电阶段报文超时（BCS/BSM 5 s 未刷新） */
    GB_ERR_BCL_TIMEOUT,         /**< BCL 专用超时（标准原文：1 s 未收到即结束充电） */
    GB_ERR_ISOTP,               /**< 多帧组包错误 */
    GB_ERR_DATA_INVALID,        /**< 数据域非法（长度/取值越界） */
    GB_ERR_BMS_ABORT,           /**< BMS 主动中止（收到 BST） */
    GB_ERR_BUS_FAULT,           /**< 总线错误 / Bus-Off */
    GB_ERR_MAX
} gb_error_t;

/*==============================================================================
 *                          状态位枚举（标准逐表取值，不得自行发挥）
 *============================================================================*/

/* ---- BSM B6/B7 的 2 位状态字段（标准表 20）---- */
#define GB_ST_NORMAL        0x0u  /**< 00 正常 */
#define GB_ST_ABNORMAL      0x1u  /**< 01 过高 / 过流 / 不正常 / 中止 */
#define GB_ST_LOW           0x2u  /**< 10 过低 / 不可信状态 */
#define GB_ST_UNRELIABLE    0x2u  /**< 10 不可信状态（与 GB_ST_LOW 同码，按语义取名） */

/* BSM B7.5 SPN3096 充电允许单独枚举（**只有 2 个取值**，没有"不可信"） */
#define GB_CHARGE_FORBIDDEN 0x0u  /**< 00 禁止充电 */
#define GB_CHARGE_ALLOWED   0x1u  /**< 01 允许充电 */

/* ---- BST SPN3511 / CST SPN3521：中止原因（B1，4 个 2 位字段）---- */
#define GB_BST_R_SOC_SHIFT          6u   /**< B1.1-1.2 达到所需 SOC 目标值 */
#define GB_BST_R_TOTAL_V_SHIFT      4u   /**< B1.3-1.4 达到总电压设定值   */
#define GB_BST_R_CELL_V_SHIFT       2u   /**< B1.5-1.6 达到单体电压设定值 */
#define GB_BST_R_CHARGER_STOP_SHIFT 0u   /**< B1.7-1.8 充电机主动中止     */

#define GB_CST_R_CHARGER_COND_SHIFT 6u   /**< B1.1-1.2 达到充电机设定条件中止 */
#define GB_CST_R_MANUAL_SHIFT       4u   /**< B1.3-1.4 人工中止               */
#define GB_CST_R_FAULT_SHIFT        2u   /**< B1.5-1.6 故障中止               */
#define GB_CST_R_BMS_STOP_SHIFT     0u   /**< B1.7-1.8 BMS 主动中止（收到 BST）*/

/* ---- BST SPN3512：故障原因（B2-B3，8 个 2 位字段，从高到低）----
 * 顺序（标准原文）：绝缘故障、输出连接器过温故障、BMS 元件与输出连接器过温、
 * 充电连接器故障、电池组温度过高故障、高压继电器故障、
 * 检测点 2 电压检测故障、其他故障。                                        */
#define GB_BST_F_INSULATION_SHIFT   14u  /**< B2.1-2.2 绝缘故障              */
#define GB_BST_F_OUT_OVERHEAT_SHIFT 12u  /**< B2.3-2.4 输出连接器过温故障    */
#define GB_BST_F_BMS_OVERHEAT_SHIFT 10u  /**< B2.5-2.6 BMS 元件与连接器过温  */
#define GB_BST_F_CONNECTOR_SHIFT    8u   /**< B2.7-2.8 充电连接器故障        */
#define GB_BST_F_BATT_OVERHEAT_SHIFT 6u  /**< B3.1-3.2 电池组温度过高故障    */
#define GB_BST_F_RELAY_SHIFT        4u   /**< B3.3-3.4 高压继电器故障        */
#define GB_BST_F_CHECK2_SHIFT       2u   /**< B3.5-3.6 检测点 2 电压检测故障 */
#define GB_BST_F_OTHER_SHIFT        0u   /**< B3.7-3.8 其他故障              */

/* ---- CST SPN3522：故障原因（B2-B3，6 个 2 位字段，从高到低）----
 * 顺序（标准原文）：充电机过温故障、充电连接器故障、充电机内部过温故障、
 * 所需电量不能传送、充电机急停故障、其他故障。                              */
#define GB_CST_F_CHARGER_OVERHEAT_SHIFT  10u /**< B2.1-2.2 充电机过温故障     */
#define GB_CST_F_CONNECTOR_SHIFT         8u  /**< B2.3-2.4 充电连接器故障     */
#define GB_CST_F_INNER_OVERHEAT_SHIFT    6u  /**< B2.5-2.6 充电机内部过温故障 */
#define GB_CST_F_ENERGY_SHIFT            4u  /**< B2.7-2.8 所需电量不能传送   */
#define GB_CST_F_ESTOP_SHIFT             2u  /**< B3.1-3.2 充电机急停故障     */
#define GB_CST_F_OTHER_SHIFT             0u  /**< B3.3-3.4 其他故障           */

/* ---- BST SPN3513 / CST SPN3523：错误原因（B4，2 个 2 位字段）---- */
#define GB_BST_E_CURRENT_SHIFT      2u   /**< B4.1-4.2 电流过大  00 正常/01 超过需求值/10 不可信 */
#define GB_BST_E_VOLTAGE_SHIFT      0u   /**< B4.3-4.4 电压异常  00 正常/01 异常/10 不可信       */
#define GB_CST_E_CURRENT_SHIFT      2u   /**< B4.1-4.2 电流不匹配 00 匹配/01 不匹配/10 不可信    */
#define GB_CST_E_VOLTAGE_SHIFT      0u   /**< B4.3-4.4 电压异常   同 BST                        */

/* ---- BSM B6/B7 的位段位移（与 STM32 端 bms_protocol.c 完全同名同值）---- */
#define BSM_B6_CELL_V_SHIFT     0u   /**< B6.1-6.2 SPN3090 单体电压过高/过低 */
#define BSM_B6_SOC_SHIFT        2u   /**< B6.3-6.4 SPN3091 整车 SOC 过高/过低 */
#define BSM_B6_OVER_I_SHIFT     4u   /**< B6.5-6.6 SPN3092 充电过电流 */
#define BSM_B6_OVER_T_SHIFT     6u   /**< B6.7-6.8 SPN3093 温度过高 */
#define BSM_B7_INSULATION_SHIFT 0u   /**< B7.1-7.2 SPN3094 绝缘状态 */
#define BSM_B7_CONNECTOR_SHIFT  2u   /**< B7.3-7.4 SPN3095 输出连接器连接状态 */
#define BSM_B7_PERMIT_SHIFT     4u   /**< B7.5-7.6 SPN3096 充电允许 00 禁止 / 01 允许 */
#define BSM_B7_UNDEFINED_SHIFT  6u   /**< B7.7-7.8 标准未定义 → 按 7.9 填 1 */

/** 取 2 位字段 */
#define GB_FIELD2(v, shift)     ((uint8_t)(((uint8_t)(v) >> (shift)) & 0x03u))
/** 把 2 位字段写进字节（值会先与 0x03 相与） */
#define GB_FIELD2_SET(v, shift, val) \
    ((uint8_t)(((uint8_t)(v) & (uint8_t)~(uint8_t)(0x03u << (shift))) | \
               (uint8_t)(((uint8_t)(val) & 0x03u) << (shift))))

/*==============================================================================
 *                          解析结果结构体
 *============================================================================*/

/** BCP 动力蓄电池充电参数 */
typedef struct
{
    float    max_single_voltage;      /**< B1-B2 单体最高允许充电电压  0.01 V/位, 0 偏移 */
    float    max_current;             /**< B3-B4 最高允许充电电流      0.1 A/位, -400 A 偏移 */
    float    nominal_energy;          /**< B5-B6 动力蓄电池标称总能量  0.1 kWh/位, 0 偏移 */
    float    max_total_voltage;       /**< B7-B8 最高允许充电总电压    0.1 V/位, 0 偏移 */
    float    max_temperature;         /**< B9    最高允许温度          1 ℃/位, -50 ℃ 偏移 */
    float    soc;                     /**< B10-B11 整车荷电状态 SOC    0.1 %/位, 0 偏移 */
    float    current_voltage;         /**< B12-B13 整车动力蓄电池当前电压 0.1 V/位 */
    struct timespec ts;               /**< 解析时间戳 */
} gb_bcp_t;

/** BCL 电池充电需求（PGN 0x1000，标准表 17：**5 字节**单帧） */
typedef struct
{
    float    voltage_demand;          /**< B1-B2 SPN 3072 电压需求      0.1 V/位, 0 偏移 */
    float    current_demand;          /**< B3-B4 SPN 3073 电流需求      0.1 A/位, -400 A 偏移 */
    uint8_t  charge_mode;             /**< B5    SPN 3074 充电模式      1=恒压 2=恒流 */
    struct timespec ts;
} gb_bcl_t;

/** BCS 电池充电总状态（PGN 0x1100，标准表 18：9 字节；本工程按 8 字节单帧） */
typedef struct
{
    float    measure_voltage;         /**< B1-B2 SPN 3075 充电电压测量值 0.1 V/位, 0 偏移 */
    float    measure_current;         /**< B3-B4 SPN 3076 充电电流测量值 0.1 A/位, -400 A 偏移 */
    float    max_single_voltage;      /**< B5-B6 SPN 3077 最高单体电压(1-12 位) 0.01 V/位, 0 偏移 */
    uint8_t  max_single_group;        /**< B5-B6 SPN 3077 最高单体所在组号(13-16 位) 1/位, 0 偏移 */
    float    current_soc;             /**< B7    SPN 3078 当前 SOC   **1 %/位**, 0 偏移 */
    uint8_t  remain_charge_raw;       /**< B8    SPN3079 剩余充电时间**原始字节**。
                                       *   本工程发/收的都是 0xFF（未估算，标准 7.9 填 1），
                                       *   所以这里刻意**不换算成分钟数** —— 假值换算了
                                       *   反而更容易被误当成真实估算结果。 */
    uint8_t  remain_charge_absent;    /**< 1 = 该字段为"未规定/无有效值"(0xFF)，不是真实估算 */
    uint8_t  remain_charge_lo_byte;   /**< 1 = 本帧只带 B8(低字节)，B9 缺失（8 字节偏差） */
    uint8_t  group_number_valid;      /**< 1 = 本帧携带了组号（标准布局），0 = 组号缺省 */
    struct timespec ts;
} gb_bcs_t;

/** BSM 动力蓄电池状态信息（PGN 0x1300，标准表 20：**7 字节**单帧） */
typedef struct
{
    uint8_t  max_single_voltage_no;   /**< B1    SPN 3085 最高单体电压所在编号 1/位, 1 偏移 */
    float    max_temp;                /**< B2    SPN 3086 最高温度  1 ℃/位, -50 ℃ 偏移 */
    uint8_t  max_temp_no;             /**< B3    SPN 3087 最高温度检测点编号 1/位, 1 偏移 */
    float    min_temp;                /**< B4    SPN 3088 最低温度  1 ℃/位, -50 ℃ 偏移 */
    uint8_t  min_temp_no;             /**< B5    SPN 3089 最低温度检测点编号 1/位, 1 偏移 */
    uint8_t  status_b6;               /**< B6    4 个 2 位状态字段原始值（SPN3090~3093） */
    uint8_t  status_b7;               /**< B7    4 个 2 位状态字段原始值（SPN3094~3096） */
    /* --- 下面是 B6/B7 拆出来的可读字段，避免各调用点重复做位运算 --- */
    uint8_t  cell_voltage_state;      /**< B6.1  SPN3090 单体电压过高/过低 */
    uint8_t  soc_state;               /**< B6.3  SPN3091 整车 SOC 过高/过低 */
    uint8_t  over_current_state;      /**< B6.5  SPN3092 充电过电流 */
    uint8_t  over_temp_state;         /**< B6.7  SPN3093 温度过高 */
    uint8_t  insulation_state;        /**< B7.1  SPN3094 绝缘状态 */
    uint8_t  connector_state;         /**< B7.3  SPN3095 输出连接器连接状态 */
    uint8_t  charge_permit;           /**< B7.5  SPN3096 充电允许：0 禁止 / 1 允许 */
    struct timespec ts;
} gb_bsm_t;

/** BRM 动力蓄电池辨识（41 字节，多帧重组后解析）—— 字段顺序见标准表 11 */
typedef struct
{
    uint32_t version;                 /**< B1-B3  BMS 通信协议版本号（**小端 24 位**）。
                                       *   标准原文：V1.1 = byte3,byte2—0001H；byte1—01H
                                       *   ⇒ 数据域 `01 01 00` ⇒ 按小端读回 = **0x000101**。
                                       *   ⚠ 这是 **24 位**的值，**不能塞进 uint16_t** ——
                                       *   16 位会把最高字节 0x00 截掉，读出来变成 0x0101，
                                       *   于是"版本号解析错误: 0x000101"这种断言就会失败。 */
    uint8_t  battery_type;            /**< B4     电池类型 */
    float    rated_capacity;          /**< B5-B6  额定容量    0.1 Ah/位, 0 偏移 */
    float    rated_voltage;           /**< B7-B8  额定总电压  0.1 V/位, 0 偏移 */
    char     manufacturer[8];         /**< B9-B12 电池生产厂商（4 字节 ASCII + 终止符） */
    uint8_t  pack_serial[4];          /**< B13-B16 电池组序号（预留，厂商自定义） */
    uint16_t produce_year;            /**< B17    生产年份（原始值，**加 1985** 才是公元年） */
    uint8_t  produce_month;           /**< B18    生产月份 */
    uint8_t  produce_day;             /**< B19    生产日 */
    uint32_t charge_count;            /**< B20-B22 电池组充电次数（小端 3 字节） */
    uint8_t  ownership;               /**< B23    电池组产权标识：0 租赁 / 1 车自有 */
    uint8_t  reserved24;              /**< B24    预留 */
    char     vin[18];                 /**< B25-B41 车辆识别码 VIN（17 字节 ASCII + 终止符） */
    uint16_t raw_len;                 /**< 原始报文长度（应为 41） */
    struct timespec ts;
} gb_brm_t;

/** BST / CST 中止充电报文（标准表 24 / 表 25：4 字节，全是 2 位字段） */
typedef struct
{
    uint8_t  reason_flags;            /**< B1    中止原因（4 个 2 位字段，见 GB_BST_R_*） */
    uint8_t  fault_flags_lo;          /**< B2    故障原因低 4 个 2 位字段 */
    uint8_t  fault_flags_hi;          /**< B3    故障原因高 4 个 2 位字段 */
    uint8_t  error_flags;             /**< B4    错误原因（2 个 2 位字段，见 GB_BST_E_*） */
    uint16_t fault_flags;             /**< B2-B3 拼成 16 位便于整包打印 */
    struct timespec ts;
} gb_stop_t;

/** BSD BMS 统计数据（标准表 26：7 字节） */
typedef struct
{
    uint8_t  soc;                     /**< B1    SPN3601 中止荷电状态 SOC 1 %/位, 0 偏移 */
    float    min_single_voltage;      /**< B2-B3 SPN3602 单体**最低**电压 0.01 V/位 */
    float    max_single_voltage;      /**< B4-B5 SPN3603 单体**最高**电压 0.01 V/位 */
    float    min_temp;                /**< B6    SPN3604 **最低**温度 1 ℃/位, -50 ℃ 偏移 */
    float    max_temp;                /**< B7    SPN3605 **最高**温度 1 ℃/位, -50 ℃ 偏移 */
    struct timespec ts;
} gb_bsd_t;

/** CSD 充电机统计数据（标准表 27：8 字节） */
typedef struct
{
    uint16_t total_charge_minutes;    /**< B1-B2 SPN3611 累计充电时间 1 min/位, 0~600 */
    float    output_energy;           /**< B3-B4 SPN3612 输出能量 0.1 kWh/位, 0~1000 */
    uint32_t charger_id;              /**< B5-B8 SPN3613 充电机编号 1/位, **1 偏移** */
    struct timespec ts;
} gb_csd_t;

/** CCS 充电机充电状态（标准表 19：8 字节，50 ms，充电机发、BMS 收） */
typedef struct
{
    float    out_voltage;             /**< B1-B2 SPN3081 电压输出值 0.1 V/位, 0 偏移 */
    float    out_current;             /**< B3-B4 SPN3082 电流输出值 0.1 A/位, -400 A 偏移 */
    uint16_t charge_minutes;          /**< B5-B6 SPN3083 累计充电时间 1 min/位, 0~600 */
    uint8_t  charge_permit;           /**< B7.1  SPN3929 充电允许：0 暂停 / 1 允许 */
    struct timespec ts;
} gb_ccs_t;

/** BEM BMS 错误报文（标准表 28：4 字节，250 ms，BMS 发、充电机收） */
typedef struct
{
    uint8_t  crm_00_timeout;          /**< B1.1 SPN3901 接收 SPN2560=0x00 的辨识报文超时 */
    uint8_t  crm_aa_timeout;          /**< B1.3 SPN3902 接收 SPN2560=0xAA 的辨识报文超时 */
    uint8_t  cts_cml_timeout;         /**< B2.1 SPN3903 时间同步与最大输出能力报文超时 */
    uint8_t  cro_timeout;             /**< B2.3 SPN3904 完成充电准备报文超时 */
    uint8_t  ccs_timeout;             /**< B3.1 SPN3905 充电状态报文超时 */
    uint8_t  cst_timeout;             /**< B3.3 SPN3906 中止充电报文超时 */
    uint8_t  csd_timeout;             /**< B4.1 SPN3907 充电统计报文超时 */
    uint8_t  raw[4];                  /**< 原始 4 字节 */
    struct timespec ts;
} gb_bem_t;

/** CEM 充电机错误报文（标准表 29：4 字节，250 ms，充电机发、BMS 收） */
typedef struct
{
    uint8_t  ident_timeout;           /**< B1.1 SPN3921 接收 BMS 和车辆的辨识报文超时 */
    uint8_t  bcp_timeout;             /**< B2.1 SPN3922 接收电池充电参数报文超时 */
    uint8_t  bro_timeout;             /**< B2.3 SPN3923 接收 BMS 完成充电准备报文超时 */
    uint8_t  bcs_timeout;             /**< B3.1 SPN3924 接收电池充电总状态报文超时 */
    uint8_t  bcl_timeout;             /**< B3.3 SPN3925 接收电池充电要求报文超时 */
    uint8_t  bst_timeout;             /**< B3.5 SPN3926 接收 BMS 中止充电报文超时 */
    uint8_t  bsd_timeout;             /**< B4.1 SPN3927 接收 BMS 充电统计报文超时 */
    uint8_t  raw[4];                  /**< 原始 4 字节 */
    struct timespec ts;
} gb_cem_t;

/** CML 充电机最大输出能力 */
typedef struct
{
    float    max_voltage;             /**< B1-B2 最高输出电压 0.1 V/位 */
    float    min_voltage;             /**< B3-B4 最低输出电压 0.1 V/位 */
    float    max_current;             /**< B5-B6 最大输出电流 0.1 A/位 */
    float    min_current;             /**< B7-B8 最小输出电流 0.1 A/位 */
    struct timespec ts;
} gb_cml_t;

/*==============================================================================
 *                          回调接口（事件通知）
 *  为了不引入回调依赖，先在前向声明区位定义回调类型，
 *  之后才定义使用它的 gb_context_t。
 *============================================================================*/

struct gb_context_s;

/** 状态切换事件回调 */
typedef void (*gb_state_cb)(void *arg, gb_state_t old_state, gb_state_t new_state);

/** 异常事件回调 */
typedef void (*gb_error_cb)(void *arg, gb_error_t err, const char *detail);

/** 收到完整报文事件回调 */
typedef void (*gb_msg_cb)(void *arg, uint32_t can_id, const uint8_t *data, uint8_t len);

typedef struct
{
    gb_state_cb  on_state_change;     /**< 状态迁移通知 */
    gb_error_cb  on_error;            /**< 异常通知 */
    gb_msg_cb    on_message;          /**< 报文通知 */
    void        *arg;                 /**< 回调透传参数 */
} gb_callbacks_t;

/**
 * @brief 协议层解析出的完整充电上下文（供 UI / 存储 / 状态机共用）
 */
typedef struct gb_context_s
{
    /* 会话信息 */
    uint32_t      session_id;         /**< 本次充电会话编号 */
    gb_state_t    state;              /**< 当前状态机状态 */
    gb_error_t    error;              /**< 最近一次异常码 */

    /* --- 时间基准：全部使用 CLOCK_MONOTONIC 的毫秒时间戳，不受系统时间调整影响 --- */
    uint64_t      now_ms;             /**< 最近一次 tick 的时间 */
    uint64_t      state_enter_ms;     /**< 进入当前状态的时间 */
    uint64_t      last_rx_ms;         /**< 最近一次收到 BMS 有效报文的时间 */
    uint64_t      session_start_ms;   /**< 会话开始时间 */

    /* --- 逐报文超时基准（标准是**逐条不同**的超时，不能共用一个 5 s）---
     *   BCL 1 s（标准原文：充电机 1 s 内没收到就该立即结束充电）
     *   BCS 5 s
     *   其余走通用 T_CHARGE_MS(5 s)。 */
    uint64_t      last_bcl_ms;        /**< 最近一次收到 BCL 的时刻 */
    uint64_t      last_bcs_ms;        /**< 最近一次收到 BCS 的时刻 */

    /* 输出能量积分（CSD 的 SPN3612 要用），单位 0.1 kWh */
    double        charge_energy_x10;  /**< 本次会话累计输出能量，0.1 kWh */
    uint64_t      bcs_energy_last_ms; /**< 上一次参与积分的时间戳（0 = 尚未开始） */

    /* 就绪阶段起点：BRO/CRO 的"未准备好则等待"时限是 **60 s**，
     * 不能套用通用的 5 s，故单独记一个起点。 */
    uint64_t      ready_since_ms;     /**< 进入 GB_ST_CHARGING_READY 的时刻 */

    /* --- 发送侧：CCS / CEM 的状态 ---
     *   CCS 是标准表 19 规定的 50 ms 周期报文（充电机 → BMS），
     *   本工程之前完全没发，导致对端 BMS 的 CCS 1 s 超时判据必然触发。
     *   CEM 是错误报文（标准表 29），挂在已有的超时/故障分支上发。 */
    uint64_t      ccs_publish_ms;     /**< CCS 的本机已发送时间（会话内秒数用） */
    uint8_t       cem_mask;           /**< 本次故障要上报的 CEM 错误掩码（CEM_ERR_*） */
    uint8_t       cem_sent;           /**< CEM 已发送次数 */
    uint64_t      next_cem_ms;        /**< 下一次可发 CEM 的时刻 */

    /* --- 接收侧：BEM 错误报文（BMS 主动报错） --- */
    int           has_bem;            /**< 1 = 至少收到过一次 BEM */
    gb_bem_t      bem;                /**< 解析出的 BEM 内容 */
    gb_cem_t      cem;                /**< 解析出的 / 本端要发的 CEM 内容 */

    /* --- 各周期报文的下一次发送时刻 --- */
    uint64_t      next_chm_ms;        /**< CHM 充电机握手       250 ms */
    uint64_t      next_crm_ms;        /**< CRM 充电机辨识       250 ms */
    uint64_t      next_cts_ms;        /**< CTS 时间同步         500 ms */
    uint64_t      next_cml_ms;        /**< CML 最大输出能力     250 ms */
    uint64_t      next_cro_ms;        /**< CRO 输出准备就绪     250 ms */
    uint64_t      next_ccs_ms;        /**< CCS 充电机充电状态   **50 ms**（标准表 5）*/

    /* CSD 发送控制：结束阶段发一次即可，标准周期 250 ms 但统计报文重发无意义 */
    uint64_t      next_csd_ms;        /**< 下一次可发 CSD 的时刻 */
    int           csd_sent;           /**< 1 = 本会话已发过 CSD */

    /* 充电机身份（CSD 的 B5-B8 SPN3613 要用，1 偏移） */
    uint32_t      charger_id;         /**< 充电机编号，写入 CSD */

    /* 各报文解析结果与有效性标志 */
    int           has_bhm, has_bcp, has_bcl, has_bcs, has_bsm, has_brm, has_bst, has_bro;
    float         bhm_max_total_voltage; /**< BHM B1-B2 SPN2601 最高允许充电总电压 0.1 V/位 */
    gb_bcp_t      bcp;
    gb_bcl_t      bcl;
    gb_bcs_t      bcs;
    gb_bsm_t      bsm;
    gb_brm_t      brm;
    gb_stop_t     bst;
    gb_bsd_t      bsd;

    /* 统计 */
    uint64_t      rx_msg_count;       /**< 解析成功的 BMS 报文数 */
    uint64_t      parse_err_count;    /**< 解析失败次数 */
    uint64_t      unknown_id_count;   /**< 未知 ID 帧数 */

    /* 运行模式 */
    int           dump_mode;          /**< 1 = 只抓包解析，不主动发送任何报文 */
    int           verbose;            /**< 1 = 输出详细调试日志 */

    /* ISO-TP 状态 */
    isotp_rx_t    brm_rx;             /**< BRM / BCP 多帧重组上下文 */
    uint32_t      tp_target_pgn;      /**< 当前多帧会话承载的目标 PGN（区分 BRM/BCP） */
    int           brm_ready;          /**< 1 = 多帧重组完成待解析 */
    uint8_t       brm_buf[ISOTP_MAX_PAYLOAD];
    uint16_t      brm_len;

    /* 发送侧 ISO-TP（用于需要发送长报文的场景） */
    isotp_tx_t    tx;

    /* ---- 充电启动请求 ----
     * 0 = 待机：不发 CHM，充电机什么都不做（上电默认就是待机）
     * 1 = 用户已在 i.MX 界面上按下「充电」，下一次 tick 进入握手
     * 相当于现实里"把充电枪插上"这个动作。 */
    int           start_req;

    /* ---- 停止充电请求 ----
     * 1 = 用户又在界面上按了一次「充电」按钮（这时它是「停止」），
     *     相当于现实里**拔掉充电枪**：充电机发 CST 中止充电。
     * 由 gb27930_tick() 消费掉。 */
    int           stop_req;

    /* 上一次"用户主动停止"的时刻（毫秒）。
     * request_start() 会拒绝在这个时刻之后的一小段时间内到达的开始请求 ——
     * 防止"停止"紧接着被当成"开始"，造成停了又自己充上。 */
    uint64_t      stop_ms;

    /* 回调集合（由 gb27930_init 注入） */
    gb_callbacks_t cb;
} gb_context_t;

/*==============================================================================
 *                          API
 *============================================================================*/

/**
 * @brief  初始化协议层上下文
 * @param  ctx 上下文
 * @param  cb  回调集合，可为 NULL
 */
void gb27930_init(gb_context_t *ctx, const gb_callbacks_t *cb);

/**
 * @brief  复位状态机（保留统计量），用于开始新一轮充电
 */
void gb27930_reset(gb_context_t *ctx);

/**
 * @brief  请求开始充电（i.MX 界面「充电」按钮 / 插入充电枪）
 * @note   只是置一个标志，真正的握手在 gb27930_tick() 里发起。
 *         待机状态下反复点也没关系。
 */
void gb27930_request_start(gb_context_t *ctx);

/**
 * @brief  请求停止充电（i.MX 界面再按一次「充电」/ 拔充电枪）
 * @note   只有在充电流程里才生效。真正的停机上报文由 tick 发出。
 */
void gb27930_request_stop(gb_context_t *ctx);

/**
 * @brief  当前是否处在充电流程中（握手 ~ 充电中）
 * @return 1 = 是；0 = 空闲 / 结束 / 故障
 * @note   界面用它决定「充电」按钮这一次是"开始"还是"停止"。
 */
int  gb27930_is_active(const gb_context_t *ctx);

/**
 * @brief  向硬件层下发 GB/T 27930 的 BMS 侧报文滤波器
 * @return 0 成功；负值为 errno
 */
int  gb27930_apply_rx_filter(can_layer_t *cl);

/**
 * @brief  解析一帧 CAN 报文并推动状态机流转（核心函数）
 * @param  ctx  协议上下文
 * @param  cl   CAN 层（用于发送握手/配置报文）
 * @param  item 收到的 CAN 帧
 * @return 1 = 成功解析出有效报文；0 = 与本层无关（已过滤/组包中）；
 *         负值为错误码
 * @note   本函数应在协议解析线程中调用，不可重入。
 */
int  gb27930_process_frame(gb_context_t *ctx, can_layer_t *cl, const can_item_t *item);

/**
 * @brief  周期性驱动（建议 10~50 ms 调用一次）
 *        负责：报文周期发送、超时判定、状态机推进、ISO-TP 超时检查
 * @param  ctx    协议上下文
 * @param  cl     CAN 层
 * @param  now_us 当前单调时间戳（微秒）。传 0 由函数内部获取。
 */
void gb27930_tick(gb_context_t *ctx, can_layer_t *cl, uint64_t now_us);

/**
 * @brief  取当前状态的可读名称
 */
const char *gb27930_state_str(gb_state_t st);

/**
 * @brief  取异常码的可读名称
 */
const char *gb27930_error_str(gb_error_t err);

/**
 * @brief  把状态机当前上下文导出为存储记录
 * @param  ctx 协议上下文
 * @param  rec 输出记录（storage.h 中定义）
 */
void gb27930_fill_storage_record(const gb_context_t *ctx, storage_charge_t *rec);

/*==============================================================================
 *                  编码工具（对外可见，供离线自检验证字节布局）
 *============================================================================*/

/**
 * @brief  二进制 0~99 -> 压缩 BCD（标准表 13 的时间字段编码）
 * @return 压缩 BCD 字节；入参大于 99 时返回 0（不产生非法 BCD）
 */
uint8_t  gb27930_bcd_encode(uint8_t bin);

/**
 * @brief  压缩 BCD -> 二进制
 * @return 0~99；非法 BCD（任一半字节 > 9）返回 0
 */
uint8_t  gb27930_bcd_decode(uint8_t bcd);

/**
 * @brief  按标准表 13 打包 CTS 的 7 字节数据域
 * @param  out    输出缓冲，至少 7 字节
 * @param  year   公元年（如 2025）
 * @param  month  月 1~12
 * @param  day    日 1~31
 * @param  hour   时 0~23
 * @param  minute 分 0~59
 * @param  second 秒 0~59
 * @note   **字节顺序为「秒、分、时、日、月、年」，全部压缩 BCD 码。**
 *         参数顺序刻意按协议字节顺序排列，便于逐行对照标准原文。
 */
void     gb27930_cts_pack(uint8_t out[7],
                          uint16_t year, uint8_t month, uint8_t day,
                          uint8_t hour, uint8_t minute, uint8_t second);

/**
 * @brief  CSD 的 SPN3613 充电机编号编码（标准：1/位，**1 偏移**）
 * @return 线上应发送的值 = charger_id + 1
 */
uint32_t gb27930_csd_encode_id(uint32_t charger_id);

/**
 * @brief  按标准表 29 打包 CEM 的 4 字节数据域（供离线自检验证字段位移）
 * @param  out      输出缓冲，至少 4 字节
 * @param  err_mask 要上报的错误类别位掩码（CEM_ERR_* 按位或）
 * @note   CEM 的字段错位与 BEM **不同**（B1 只有一个字段，
 *         B3 的 SPN3926 落在 B3.5-3.6），所以两张表各写各的位移。
 */
void     gb27930_cem_pack(uint8_t out[4], uint8_t err_mask);

#ifdef __cplusplus
}
#endif

#endif /* GB27930_H */
