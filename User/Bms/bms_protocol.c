/**
  ******************************************************************************
  * @file    Bms/bms_protocol.c
  * @author  GB27930 BMS Simulator Project
  * @version V1.0.0
  * @date    2025
  * @brief   GB/T 27930-2015 BMS 侧协议实现
  *          —— 国标报文组包 + J1939 多帧传输 + BMS 充电状态机 + 电池模型仿真
  *
  * 文件结构
  * --------
  *   §1  协议布局常量（字节偏移 / 分辨率 / 偏移量）—— 所有魔数集中在此
  *   §2  小端读写与定标工具（含保留位填 1 辅助）
  *   §3  模块静态状态
  *   §4  J1939 TP 发送状态机（RTS -> CTS -> DT）
  *   §5  定点数打印辅助（Keil MicroLIB 下不使用 %f）
  *   §6  报文组包（BHM / BRM / BCP / BRO / BCL / BCS / BSM / BST / BSD）
  *   §7  电池模型仿真
  *   §8  CAN 报文接收处理
  *   §9  状态机与周期任务
  *   §10 对外接口
  *
  * 编译环境
  * --------
  *   Keil MDK5 + ARM Compiler 5（C90）+ MicroLIB
  *   因此本文件：变量全部在块首声明、不使用 // 注释、不使用浮点与 %f、
  *   不使用 64 位 printf 格式符。
  *
  * 字节序
  * ------
  *   GB/T 27930-2015 标准 4.4 原文：「数据信息传输采用低字节先发送的格式。」
  *   因此数据域内的所有多字节字段一律**小端（低字节在前）**，
  *   由 §2 的 le16_put / le16_get / le24_put 统一处理。
  *   J1939 传输协议 TP.CM / TP.DT 的长度、PGN、包间隔字段按 J1939-21
  *   同样是小端，与本标准数据域一致，不再存在"相反"的情况。
  *
  * 保留位
  * ------
  *   GB/T 27930-2015 标准 7.9：未规定的无效位 / 预留位一律**填充 1**。
  *   发送路径统一先 fill_reserved()（memset 0xFF）再写真实字段。
  ******************************************************************************
  */

#include "bms_protocol.h"
#include "stm32f10x_flash.h"      /* SOC 掉电记忆：写 Flash 最后一页 */
#include "bsp_led.h"

#include <stdio.h>
#include <string.h>

/*==============================================================================
 * §1  协议布局常量
 *============================================================================*/

/*----------------------------- BHM / CHM ----------------------------------*/
/* 标准表 8：CHM 数据域 3 字节 = 协议版本号（byte3,byte2—0001H；byte1—01H
 *           ? 数据域 `01 01 00`）。CHM 是**充电机发、BMS 收**，本端只解析。
 * 标准表 9：BHM 数据域 2 字节 = **最高允许充电总电压**（SPN2601，0.1 V/位）。
 *   ★ BHM 里**没有版本号字段** —— 原先按"主版本+次版本"组包是错的。 */
#define CHM_LEN                 3     /* 充电机握手：B1-B3 协议版本号 V1.1 */
#define BHM_LEN                 2     /* 车辆握手：  B1-B2 最高允许充电总电压 0.1 V */
#define BHM_OFF_MAX_TOTAL_V     0     /* B1-B2  最高允许充电总电压 0.1 V/位，小端 */
#define CHM_OFF_VERSION         0     /* B1-B3  版本号（小端 24 位，V1.1 = 0x000101） */
#define CHM_LEN_VERSION         3

/*------------------------------- CRM --------------------------------------
 * 标准表 10：CRM 数据域 8 字节。本端只解析（CRM 由充电机发）。 */
#define CRM_LEN                 8
#define CRM_OFF_RESULT          0     /* B1    辨识结果：0x00 不能辨识 / 0xAA 能辨识 */
#define CRM_OFF_ID              1     /* B2-B5 充电机编号（小端 32 位，0 偏移） */
#define CRM_OFF_REGION          5     /* B6-B8 区域编码（3 字节标准 ASCII，可选项） */
#define CRM_LEN_REGION          3
#define CRM_ID_UNKNOWN          0x00u /* BMS 不能辨识 */
#define CRM_ID_OK               0xAAu /* BMS 能辨识   */

/*------------------------------- CTS --------------------------------------
 * 标准表 13：CTS 数据域 7 字节。
 * ★ 顺序是「秒、分、时、日、月、年」，**不是**常见的年月日时分秒；
 *   而且**全部是压缩 BCD 码**（高 4 位十位、低 4 位个位），年占 2 字节。
 *   本端只解析（CTS 由充电机发）。 */
#define CTS_LEN                 7
#define CTS_OFF_SECOND          0     /* B1    秒（压缩 BCD） */
#define CTS_OFF_MINUTE          1     /* B2    分（压缩 BCD） */
#define CTS_OFF_HOUR            2     /* B3    时（压缩 BCD） */
#define CTS_OFF_DAY             3     /* B4    日（压缩 BCD） */
#define CTS_OFF_MONTH           4     /* B5    月（压缩 BCD） */
#define CTS_OFF_YEAR_HI         5     /* B6    年高位（压缩 BCD，2025 -> 0x20） */
#define CTS_OFF_YEAR_LO         6     /* B7    年低位（压缩 BCD，2025 -> 0x25） */

/*------------------------------- CML --------------------------------------
 * 标准表 14：CML 数据域 8 字节。本端只解析（CML 由充电机发）。
 * ★ B5-B8 的两个电流字段都带 **-400 A 偏移**，不是 0 偏移。 */
#define CML_LEN                 8
#define CML_OFF_MAX_V           0     /* B1-B2 最高输出电压 0.1 V，0 偏移 */
#define CML_OFF_MIN_V           2     /* B3-B4 最低输出电压 0.1 V，0 偏移 */
#define CML_OFF_MAX_I           4     /* B5-B6 最大输出电流 0.1 A，-400 A 偏移 */
#define CML_OFF_MIN_I           6     /* B7-B8 最小输出电流 0.1 A，-400 A 偏移 */

/*--------------------------- 准备就绪 / 充电模式 --------------------------
 * ★ GB_READY_NOT_READY / _READY / _INVALID 与 BCL_MODE_CV / _CC 是**协议取值
 *   常量**，统一定义在 bms_protocol.h 里，本文件不重复定义。
 *   【教训】同名宏在 .c 与 .h 里各写一遍，值一样时能编过、看不出问题，
 *   但只要改一处忘一处，就是**静默的语义漂移**（两处都还能编过）。
 *   校验脚本 tools/gb27930_layout_check.js 会把"同名宏定义两次"直接报出来。 */

/* BCS 的 B8（SPN3079 估算剩余充电时间）：
 * 本工程**没有剩余充电时间估算能力**，按标准 7.9「未规定的位或字段填充 1」
 * 发 0xFF 表示"未规定 / 无有效值"。
 *   ★ 不要填 600 —— 那是"至少还要 600 分钟"的溢出上限，填它等于编造具体数值；
 *   ★ 更不要填 0 —— 会被读成"还剩 0 分钟、马上充满"，是明确的错误信息。
 * 取值宏 GB_BCS_REMAIN_ABSENT 也在 .h 里。 */

/*------------------------------- BRM --------------------------------------
 * 标准表 11 的字段顺序（【注】标准原文长度不自洽：表 5 声明 41，表 11 相加得 49）。
 * 本工程**保持 41 字节**，顺序严格按表 11：
 *   B1-B3 版本 / B4 电池类型 / B5-B6 额定容量 / B7-B8 额定总电压 /
 *   B9-B12 厂商 ASCII 4B / B13-B16 电池组序号 4B /
 *   B17 年(**1985 偏移**) B18 月 B19 日 / B20-B22 充电次数 3B /
 *   B23 产权标识 / B24 预留 / B25-B41 VIN 17B
 * SPN2576（BMS 软件版本号 8 字节）落在 41 字节之外，本工程不发送。 */
#define BRM_OFF_VERSION         0     /* B1-B3  BMS 通信协议版本号（小端 24 位） */
#define BRM_LEN_VERSION         3
#define BRM_OFF_BATT_TYPE       3     /* B4     电池类型 */
#define BRM_OFF_RATED_CAP       4     /* B5-B6  额定容量   0.1 Ah */
#define BRM_OFF_RATED_V         6     /* B7-B8  额定总电压 0.1 V  */
#define BRM_OFF_MANUFACTURER    8     /* B9-B12 生产厂商 4 字节 ASCII */
#define BRM_LEN_MANUFACTURER    4
#define BRM_OFF_PACK_SERIAL     12    /* B13-B16 电池组序号（预留，厂商自定义） */
#define BRM_LEN_PACK_SERIAL     4
#define BRM_OFF_PROD_YEAR       16    /* B17    生产年份（1 年/位，1985 偏移） */
#define BRM_OFF_PROD_MONTH      17    /* B18    生产月份 */
#define BRM_OFF_PROD_DAY        18    /* B19    生产日 */
#define BRM_OFF_CHARGE_CNT      19    /* B20-B22 电池组充电次数（小端 3 字节） */
#define BRM_OFF_OWNERSHIP       22    /* B23    电池组产权标识 0 租赁 / 1 车自有 */
#define BRM_OFF_RESERVED        23    /* B24    预留 */
#define BRM_OFF_VIN             24    /* B25-B41 车辆识别码 VIN 17 字节 */
#define BRM_LEN_VIN             17
#define BRM_LEN                 41
#define BRM_YEAR_OFFSET         1985  /* 生产年份偏移量 */

/*------------------------------- BCP --------------------------------------*/
#define BCP_OFF_MAX_CELL_V      0     /* B1-B2  单体最高允许充电电压 0.01 V */
#define BCP_OFF_MAX_I           2     /* B3-B4  最高允许充电电流 0.1 A, -400 A */
#define BCP_OFF_ENERGY          4     /* B5-B6  动力蓄电池标称总能量 0.1 kWh */
#define BCP_OFF_MAX_TOTAL_V     6     /* B7-B8  最高允许充电总电压 0.1 V */
#define BCP_OFF_MAX_TEMP        8     /* B9     最高允许温度 1 C, -50 C */
#define BCP_OFF_SOC             9     /* B10-B11 整车 SOC 0.1 % */
#define BCP_OFF_CUR_V           11    /* B12-B13 当前电池电压 0.1 V */
#define BCP_LEN                 13

/*------------------------------- BCL --------------------------------------*/
#define BCL_OFF_V_DEMAND        0     /* B1-B2  电压需求     0.1 V */
#define BCL_OFF_I_DEMAND        2     /* B3-B4  电流需求     0.1 A, -400 A */
#define BCL_OFF_MODE            4     /* B5     充电模式     1 恒压 / 2 恒流 */
#define BCL_LEN                 5     /* 标准表 5：BCL 数据域 5 字节，单帧发送。
                                       * 【注意】标准 BCL 只有这 5 字节，**没有**"允许充电电压/电流"。
                                       * 那两个字段标准里属于 BCP（SPN2819 最高允许充电总电压、
                                       * SPN2817 最高允许充电电流），不要往 BCL 里加。 */

/*------------------------------- BCS --------------------------------------
 * 标准表 18 的 BCS 共 **9 字节**：
 *   B1-B2  SPN3075 充电电压测量值     0.1 V/位，0 偏移
 *   B3-B4  SPN3076 充电电流测量值     0.1 A/位，-400 A 偏移
 *   B5-B6  SPN3077 最高单体电压**及其组号**
 *            1-12 位 = 电压       0.01 V/位，0 偏移，0~24 V
 *            13-16 位 = 所在组号   1/位，      0 偏移，0~15
 *   B7     SPN3078 当前 SOC           **1 %/位**，0~100（注意 BCP 的 SOC 是 0.1 %/位）
 *   B8-B9  SPN3079 估算剩余充电时间   1 min/位，0~600（超 600 按 600 发）
 *
 * 【已知偏差】9 字节超过 CAN 单帧 8 字节上限，按标准 6.2 注 7 应经传输协议
 * （J1939 TP，PGN 0x1100）传输。本轮**有意不改 TP 架构**（会牵动 BRM/BCP
 * 既有 TP 逻辑），仍按 **8 字节单帧**发送：
 *   B1~B8 正常送出，**缺 B9（估算剩余充电时间的高字节）**，即剩余时间只能
 *   表达 0~255 min。属已知偏差，留待后续单独评估是否改走 TP。 */
#define BCS_OFF_MEASURE_V       0     /* B1-B2  充电电压测量值 0.1 V */
#define BCS_OFF_MEASURE_I       2     /* B3-B4  充电电流测量值 0.1 A, -400 A */
#define BCS_OFF_CELL_V_GROUP    4     /* B5-B6  最高单体电压(1-12 位) + 组号(13-16 位) */
#define BCS_OFF_SOC             6     /* B7     当前 SOC       **1 %/位** */
#define BCS_OFF_REMAIN_LO       7     /* B8     估算剩余充电时间低字节（B9 为高字节） */
#define BCS_LEN                 8     /* 当前实现 8 字节（标准 9，见上） */
#define BCS_LEN_STD             9     /* 标准长度，仅供注释与上报使用 */

/* B5-B6 的位打包掩码（标准表 18 的 SPN3077） */
#define BCS_CELL_V_MASK         0x0FFFu  /* 低 12 位 = 单体电压 */
#define BCS_CELL_GROUP_MASK     0x000Fu  /* 高 4 位  = 所在组号 */
#define BCS_CELL_GROUP_SHIFT    12u
#define BCS_CELL_V_MAX_X100     2400u    /* 24.00 V 上限，超出按上限发 */

/*------------------------------- BSM --------------------------------------
 * 标准表 20 的 BSM 共 **7 字节**。★ **标准 BSM 里没有电压字段**，
 * B1 只是"最高单体电压**所在编号**"。原先把最高单体电压放在 B2-B3、
 * 把温度挤到 B4/B5、且完全没有 B6/B7 的状态位 —— 已按标准整体重写。
 *   B1    SPN3085 最高单体电压所在编号 1/位，**1 偏移**，1~256
 *   B2    SPN3086 最高动力蓄电池温度   1 ℃/位，-50 ℃ 偏移
 *   B3    SPN3087 最高温度检测点编号   1/位，**1 偏移**，1~128
 *   B4    SPN3088 最低动力蓄电池温度   1 ℃/位，-50 ℃ 偏移
 *   B5    SPN3089 最低温度检测点编号   1/位，1 偏移，1~128
 *   B6    4 个 2 位状态字段：SPN3090 单体电压过高/过低、SPN3091 整车 SOC
 *         过高/过低、SPN3092 充电过电流、SPN3093 温度过高
 *   B7    2 位状态字段 SPN3094 绝缘、SPN3095 输出连接器、SPN3096 充电允许，
 *         以及标准未定义的 7.7 两位（按 7.9 填 1） */
#define BSM_OFF_CELL_V_NO       0     /* B1     最高单体电压所在编号 1/位, 1 偏移 */
#define BSM_OFF_MAX_TEMP        1     /* B2     最高温度  1 C, -50 C */
#define BSM_OFF_MAX_TEMP_NO     2     /* B3     最高温度检测点编号 1/位, 1 偏移 */
#define BSM_OFF_MIN_TEMP        3     /* B4     最低温度  1 C, -50 C */
#define BSM_OFF_MIN_TEMP_NO     4     /* B5     最低温度检测点编号 1/位, 1 偏移 */
#define BSM_OFF_STATUS_B6       5     /* B6     SPN3090~3093 四个 2 位状态字段 */
#define BSM_OFF_STATUS_B7       6     /* B7     SPN3094~3096 + 未定义位 */
#define BSM_LEN                 7     /* 标准表 5 / 表 20：BSM 数据域 7 字节 */

/* B6 的 4 个 2 位字段（低两位在前） */
#define BSM_B6_CELL_V_SHIFT     0     /* B6.1-6.2 SPN3090 单体电压过高/过低 */
#define BSM_B6_SOC_SHIFT        2     /* B6.3-6.4 SPN3091 整车 SOC 过高/过低 */
#define BSM_B6_OVER_I_SHIFT     4     /* B6.5-6.6 SPN3092 充电过电流 */
#define BSM_B6_OVER_T_SHIFT     6     /* B6.7-6.8 SPN3093 温度过高 */
/* B7 的 4 个 2 位字段 */
#define BSM_B7_INSULATION_SHIFT 0     /* B7.1-7.2 SPN3094 绝缘状态 */
#define BSM_B7_CONNECTOR_SHIFT  2     /* B7.3-7.4 SPN3095 输出连接器连接状态 */
#define BSM_B7_PERMIT_SHIFT     4     /* B7.5-7.6 SPN3096 充电允许 00 禁止 / 01 允许 */
#define BSM_B7_UNDEFINED_SHIFT  6     /* B7.7-7.8 标准未定义 → 按 7.9 填 1 */

/* 编号上限（标准给定），超出时截断 */
#define BSM_TEMP_NO_MAX         128u
#define BSM_CELL_NO_MAX         256u

/* 状态位取值（标准表 20 的枚举，不得自行发挥）
 *   00 正常 / 01 过高(过流) / 10 过低(不可信)
 *   ★ 没有检测能力的项目一律填 10（不可信），**不要填 00（正常）冒充实测**。 */
#define BSM_ST_NORMAL           0u
#define BSM_ST_HIGH             1u    /* 01 过高 / 过流 */
#define BSM_ST_LOW              2u    /* 10 过低 */
#define BSM_ST_UNRELIABLE       2u    /* 10 不可信（与 LOW 同码，按语义取名） */
#define BSM_CHARGE_FORBIDDEN    0u    /* SPN3096：00 禁止充电 */
#define BSM_CHARGE_ALLOWED      1u    /* SPN3096：01 允许充电 */

/*------------------------------- BSD --------------------------------------
 * 标准表 26 的 BSD 共 **7 字节**。★ 注意 B2-B3 是单体**最低**电压、
 * B4-B5 才是**最高**；B6 是**最低**温度、B7 是**最高**温度。
 * 原先按"中止原因/累计电量/累计时间/充电总电压"组包，与标准完全不符。 */
#define BSD_OFF_SOC             0     /* B1    SPN3601 中止荷电状态 SOC 1 %/位 */
#define BSD_OFF_MIN_CELL_V      1     /* B2-B3 SPN3602 单体**最低**电压 0.01 V/位 */
#define BSD_OFF_MAX_CELL_V      3     /* B4-B5 SPN3603 单体**最高**电压 0.01 V/位 */
#define BSD_OFF_MIN_TEMP        5     /* B6    SPN3604 **最低**温度 1 C, -50 C */
#define BSD_OFF_MAX_TEMP        6     /* B7    SPN3605 **最高**温度 1 C, -50 C */
#define BSD_LEN                 7     /* 标准表 6 / 表 26：BSD 数据域 7 字节 */

/*------------------------------- CST --------------------------------------
 * 标准表 25 的 CST 共 4 字节，**每个字节都是若干 2 位字段**，
 * 不是整字节位掩码。本端只解析（CST 由充电机发）。 */
#define CST_LEN                 4
#define CST_OFF_REASON          0     /* B1    中止原因（4 个 2 位字段）SPN3521 */
#define CST_OFF_FAULT_LO        1     /* B2    故障原因高 4 个字段       SPN3522 */
#define CST_OFF_FAULT_HI        2     /* B3    故障原因第 5-6 个字段     SPN3522 */
#define CST_OFF_ERROR           3     /* B4    错误原因（2 个 2 位字段） SPN3523 */

/* CST SPN3521 中止原因（B1，低两位在前） */
#define CST_R_CHARGER_COND_SHIFT 6u   /* B1.1-1.2 达到充电机设定条件中止 */
#define CST_R_MANUAL_SHIFT       4u   /* B1.3-1.4 人工中止               */
#define CST_R_FAULT_SHIFT        2u   /* B1.5-1.6 故障中止               */
#define CST_R_BMS_SHIFT          0u   /* B1.7-1.8 BMS 主动中止（收到 BST）*/
/* CST SPN3523 错误原因（B4） */
#define CST_E_CURRENT_SHIFT      2u   /* B4.1-4.2 电流不匹配 00 匹配/01 不匹配/10 不可信 */
#define CST_E_VOLTAGE_SHIFT      0u   /* B4.3-4.4 电压异常   00 正常/01 异常/10 不可信   */

/*------------------------------- BST --------------------------------------
 * 标准表 24 的 BST 共 4 字节，同样是逐字节的 2 位字段。 */
#define BST_LEN                 4
#define BST_OFF_REASON          0     /* B1    SPN3511 中止原因（4 个 2 位字段） */
#define BST_OFF_FAULT_LO        1     /* B2    SPN3512 故障原因高 4 个字段 */
#define BST_OFF_FAULT_HI        2     /* B3    SPN3512 故障原因第 5-8 个字段 */
#define BST_OFF_ERROR           3     /* B4    SPN3513 错误原因（2 个 2 位字段） */

/* BST SPN3511 中止原因（B1，低两位在前） */
#define BST_R_SOC_SHIFT         6u    /* B1.1-1.2 达到所需 SOC 目标值 */
#define BST_R_TOTAL_V_SHIFT     4u    /* B1.3-1.4 达到总电压设定值   */
#define BST_R_CELL_V_SHIFT      2u    /* B1.5-1.6 达到单体电压设定值 */
#define BST_R_CHARGER_STOP_SHIFT 0u   /* B1.7-1.8 充电机主动中止     */
/* BST SPN3512 故障原因（B2-B3，8 个 2 位字段，从高到低）
 *   顺序（标准原文）：绝缘故障、输出连接器过温故障、BMS 元件与输出连接器过温、
 *   充电连接器故障、电池组温度过高故障、高压继电器故障、
 *   检测点 2 电压检测故障、其他故障。 */
#define BST_F_INSULATION_SHIFT  14u   /* B2.1-2.2 绝缘故障              */
#define BST_F_OUT_OVERHEAT_SHIFT 12u  /* B2.3-2.4 输出连接器过温故障    */
#define BST_F_BMS_OVERHEAT_SHIFT 10u  /* B2.5-2.6 BMS 元件与连接器过温  */
#define BST_F_CONNECTOR_SHIFT   8u    /* B2.7-2.8 充电连接器故障        */
#define BST_F_BATT_OVERHEAT_SHIFT 6u  /* B3.1-3.2 电池组温度过高故障    */
#define BST_F_RELAY_SHIFT       4u    /* B3.3-3.4 高压继电器故障        */
#define BST_F_CHECK2_SHIFT      2u    /* B3.5-3.6 检测点 2 电压检测故障 */
#define BST_F_OTHER_SHIFT       0u    /* B3.7-3.8 其他故障              */
/* BST SPN3513 错误原因（B4） */
#define BST_E_CURRENT_SHIFT     2u    /* B4.1-4.2 电流过大 00 正常/01 超过需求值/10 不可信 */
#define BST_E_VOLTAGE_SHIFT     0u    /* B4.3-4.4 电压异常 00 正常/01 异常/10 不可信       */

/* BST 故障原因两个字节的预置值：本工程**不做 BMS 侧故障检测**，
 * 6 个已定义字段一律如实填"10 不可信"，未定义的低 4 位按 7.9 填 1。
 *   B2 = 10 10 10 10 = 0xAA，B3 = 10 10 11 11 = 0xAF */
#define BST_FAULT_UNRELIABLE_HI 0xAAu
#define BST_FAULT_UNRELIABLE_LO 0xAFu

/** 取 2 位字段 */
#define GB_FIELD2(v, shift)     ((uint8_t)(((uint8_t)(v) >> (shift)) & 0x03u))
/** 把 2 位字段写进字节（值先与 0x03 相与） */
#define GB_FIELD2_SET(v, shift, val) \
    ((uint8_t)(((uint8_t)(v) & (uint8_t)~(uint8_t)(0x03u << (shift))) | \
               (uint8_t)(((uint8_t)(val) & 0x03u) << (shift))))

/*------------------------------- CCS --------------------------------------
 * 标准表 19：CCS 充电机充电状态，8 字节，**周期 50 ms，充电机发、BMS 收**。
 *   B1-B2  SPN3081 电压输出值      0.1 V/位，0 偏移
 *   B3-B4  SPN3082 电流输出值      0.1 A/位，**-400 A 偏移**
 *   B5-B6  SPN3083 累计充电时间    1 min/位，0 偏移，0~600 min
 *   B7.1   SPN3929 充电允许        2 位：**00 暂停 / 01 允许**
 *   B7.3-7.8 与 B8 标准未定义，按 7.9 填 1
 *
 * 【超时】标准原文：「如果 BMS 在 **1 s** 内没有收到该报文，即为超时错误，
 *   BMS 应立即结束充电。」—— 这是 **BMS 侧（本端）** 要判的超时。 */
#define CCS_LEN                 8
#define CCS_OFF_OUT_V           0     /* B1-B2 电压输出值 0.1 V */
#define CCS_OFF_OUT_I           2     /* B3-B4 电流输出值 0.1 A, -400 A 偏移 */
#define CCS_OFF_MINUTES         4     /* B5-B6 累计充电时间 1 min */
#define CCS_OFF_PERMIT          6     /* B7    充电允许（2 位字段在低两位） */
#define CCS_PERMIT_SHIFT        0u    /* B7.1-7.2 SPN3929：00 暂停 / 01 允许 */
#define CCS_MAX_MINUTES         600u  /* 标准上限 */
/* CCS 的充电允许枚举（**只有 2 个取值，没有"不可信"**） */
#define CCS_OUTPUT_PAUSE        0u    /* 00 充电机将停止输出 */
#define CCS_OUTPUT_ALLOW        1u    /* 01 充电机将继续充电 */

/*------------------------------- BEM --------------------------------------
 * 标准表 28：BEM BMS 错误报文，4 字节，**周期 250 ms，BMS 发、充电机收**。
 * 优先权 **2**（全项目最高），ID = 0x081E56F4。
 *
 *   B1.1  SPN3901 接收 SPN2560=0x00 的充电机辨识报文超时
 *   B1.3  SPN3902 接收 SPN2560=0xAA 的充电机辨识报文超时
 *   B2.1  SPN3903 接收充电机的时间同步和最大输出能力报文超时
 *   B2.3  SPN3904 接收充电机完成充电准备报文超时
 *   B3.1  SPN3905 接收充电机充电状态报文超时
 *   B3.3  SPN3906 接收充电机中止充电报文超时
 *   B4.1  SPN3907 接收充电机充电统计报文超时
 *   B4.3  其他（6 位，**可选项**，填 1）
 * 枚举：00 正常 / 01 超时 / 10 不可信状态。
 *
 * 【发送时机】标准原文：「当 BMS 检测到错误时，发送给充电机充电错误报文，
 *   直到 BMS 收到充电机发送的充电机辨识报文(CRM)或拔掉充电插头为止。」
 *   = 挂在**已有的超时/故障分支**上周期发，不新建机制。
 *   本端在 BMS_ST_FAULT 态里周期发送，转出故障态后自然停止。 */
#define BEM_LEN                 4
#define BEM_OFF_B1              0     /* B1  SPN3901 / SPN3902 */
#define BEM_OFF_B2              1     /* B2  SPN3903 / SPN3904 */
#define BEM_OFF_B3              2     /* B3  SPN3905 / SPN3906 */
#define BEM_OFF_B4              3     /* B4  SPN3907 + 其他（6 位可选项） */
/* 每个字节里两个 2 位字段的位移：低两位在前 */
#define BEM_B1_CRM_00_SHIFT     0u    /* B1.1-1.2 SPN3901 */
#define BEM_B1_CRM_AA_SHIFT     2u    /* B1.3-1.4 SPN3902 */
#define BEM_B2_CTS_CML_SHIFT    0u    /* B2.1-2.2 SPN3903 */
#define BEM_B2_CRO_SHIFT        2u    /* B2.3-2.4 SPN3904 */
#define BEM_B3_CCS_SHIFT        0u    /* B3.1-3.2 SPN3905 */
#define BEM_B3_CST_SHIFT        2u    /* B3.3-3.4 SPN3906 */
#define BEM_B4_CSD_SHIFT        0u    /* B4.1-4.2 SPN3907 */
/* "其他"占 B4 的高 6 位（B4.3-4.8），标准标为**可选项**，按 7.9 填 1 */
#define BEM_B4_OTHER_FILL       0xFCu
#define BEM_OTHER_UNDEFINED     3u    /* 未定义位填 1 时该 2 位字段的取值 */

/* 错误报文的 2 位枚举（标准表 28 / 表 29 同用） */
#define BEM_ST_NORMAL           0u    /* 00 正常 */
#define BEM_ST_TIMEOUT          1u    /* 01 超时 */
#define BEM_ST_UNRELIABLE       2u    /* 10 不可信状态 */

/* BEM 要上报的错误类别位掩码（本端内部用，把各类超时映射到 SPN3901~3907）
 *   SPN3901 接收 SPN2560=0x00 的充电机辨识报文超时
 *   SPN3902 接收 SPN2560=0xAA 的充电机辨识报文超时
 *   SPN3903 接收充电机的时间同步和最大输出能力报文超时
 *   SPN3904 接收充电机完成充电准备报文超时
 *   SPN3905 接收充电机充电状态报文超时
 *   SPN3906 接收充电机中止充电报文超时
 *   SPN3907 接收充电机充电统计报文超时 */
#define BEM_ERR_CRM_UNKNOWN     0x01u /* -> SPN3901 */
#define BEM_ERR_CRM_IDENT       0x02u /* -> SPN3902 */
#define BEM_ERR_CTS_CML         0x04u /* -> SPN3903 */
#define BEM_ERR_CRO             0x08u /* -> SPN3904 */
#define BEM_ERR_CCS             0x10u /* -> SPN3905 */
#define BEM_ERR_CST             0x20u /* -> SPN3906 */
#define BEM_ERR_CSD             0x40u /* -> SPN3907 */

/* BEM 报文的周期与"同一错误最多连发多久"（防止故障态里无限刷屏） */
#define GB_T_BEM_PERIOD         250u  /* 标准表 5：BEM 周期 250 ms */
#define GB_T_BEM_MAX_TX         20u   /* 连发上限（20 x 250 ms = 5 s） */

/**
  * @brief  组装 BEM BMS 错误报文（标准表 28：4 字节）
  * @param  err_mask 要上报的错误类别位掩码（BEM_ERR_* 宏按位或）
  * @note   每个"错误类别"对应一个 **2 位字段**，取值 00 正常 / 01 超时 /
  *         10 不可信状态。本端只区分"超时"（01）与"正常"（00）：
  *         每一类错误都是由"规定时间内没收到对端报文"触发的，
  *         不存在"收到了但不可信"的判据，所以**不填 10 冒充**。
  *         B4 的高 6 位是标准标注的**可选项"其他"**，按 7.9 填 1。
  */
/* 前置声明：Build_BEM 等函数在本文件中出现得比 §2 辅助工具区更早，
 * 而 fill_reserved 定义在那一区。C 语言要求 static 函数先声明后使用，
 * 否则会报 "static declaration follows non-static declaration"。 */
static void fill_reserved(uint8_t *p, uint8_t n);

static void Build_BEM(uint8_t *d, uint8_t err_mask)
{
    uint8_t b1 = 0;
    uint8_t b2 = 0;
    uint8_t b3 = 0;
    uint8_t b4 = 0;

    fill_reserved(d, BEM_LEN);

    b1 = GB_FIELD2_SET(b1, BEM_B1_CRM_00_SHIFT,
                       ((err_mask & BEM_ERR_CRM_UNKNOWN) ? BEM_ST_TIMEOUT : BEM_ST_NORMAL));
    b1 = GB_FIELD2_SET(b1, BEM_B1_CRM_AA_SHIFT,
                       ((err_mask & BEM_ERR_CRM_IDENT) ? BEM_ST_TIMEOUT : BEM_ST_NORMAL));
    b2 = GB_FIELD2_SET(b2, BEM_B2_CTS_CML_SHIFT,
                       ((err_mask & BEM_ERR_CTS_CML) ? BEM_ST_TIMEOUT : BEM_ST_NORMAL));
    b2 = GB_FIELD2_SET(b2, BEM_B2_CRO_SHIFT,
                       ((err_mask & BEM_ERR_CRO) ? BEM_ST_TIMEOUT : BEM_ST_NORMAL));
    b3 = GB_FIELD2_SET(b3, BEM_B3_CCS_SHIFT,
                       ((err_mask & BEM_ERR_CCS) ? BEM_ST_TIMEOUT : BEM_ST_NORMAL));
    b3 = GB_FIELD2_SET(b3, BEM_B3_CST_SHIFT,
                       ((err_mask & BEM_ERR_CST) ? BEM_ST_TIMEOUT : BEM_ST_NORMAL));
    b4 = GB_FIELD2_SET(b4, BEM_B4_CSD_SHIFT,
                       ((err_mask & BEM_ERR_CSD) ? BEM_ST_TIMEOUT : BEM_ST_NORMAL));
    /* B4.3-4.8 "其他"（6 位，标准标为可选项）：按 7.9 填 1 */
    b4 |= BEM_B4_OTHER_FILL;

    d[BEM_OFF_B1] = b1;
    d[BEM_OFF_B2] = b2;
    d[BEM_OFF_B3] = b3;
    d[BEM_OFF_B4] = b4;
}

/*----------------------------- 定标常量 -----------------------------------*/
#define OFF_I_400_X10           4000  /* 电流偏移 -400.0 A，单位 0.1A */
#define OFF_T_50                50    /* 温度偏移 -50 C */

/*=============================== BMS 身份信息 ==============================*/
/* 电池类型取值来自标准表 11 的 SPN2566 */
#define BMS_BATT_TYPE_LFP       3                 /* 03 = 磷酸铁锂 */
#define BMS_MANUFACTURER        "CATL"            /* B9-B12，4 字节 ASCII，不足补空格 */
#define BMS_PACK_SERIAL         "SN01"            /* B13-B16，4 字节，预留厂商自定义 */
#define BMS_PROD_YEAR           2024
#define BMS_PROD_MONTH          6
#define BMS_PROD_DAY            18
#define BMS_CHARGE_COUNT        128               /* 电池组充电次数 */
#define BMS_OWNERSHIP           1                 /* 产权标识：1 = 车自有 */
#define BMS_VIN                 "LFP512V100A000001"   /* B25-B41，17 字节 ASCII */
/* 仅用于开机横幅显示（标准 BRM 里没有"电池型号"字段，故不参与组包） */
#define BMS_MODEL               "LFP-512V100A"

/*==============================================================================
 * §2  小端读写与定标工具（标准 4.4：低字节先发送）
 *============================================================================*/

/** 未使用的位 / 保留位填充值：标准 7.9 规定填 1 */
#define GB_FILL_RESERVED        0xFFu

/** 小端 16 位写入（标准 4.4：低字节先发送） */
static void le16_put(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/** 小端 16 位读取（标准 4.4：低字节先发送） */
static uint16_t le16_get(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/** 小端 24 位写入（BRM 充电次数等 3 字节字段） */
static void le24_put(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
}

/**
  * @brief  把整段缓冲按标准 7.9 填充为 1
  * @param  p 缓冲首地址
  * @param  n 要填充的字节数：填充范围是 [p, p+n)，即**从首字节起 n 个字节**
  * @note   只能在写真实字段**之前**调用；真实字段随后覆盖，不会被冲掉。
  *         长度小于 8 的报文，其数据域内用不到的字节都靠它填 1。
  */
static void fill_reserved(uint8_t *p, uint8_t n)
{
    memset(p, (int)GB_FILL_RESERVED, (size_t)n);
}

/**
  * @brief  压缩 BCD 码转二进制
  * @param  b 形如 0x25 表示十进制的 25
  * @return 二进制值 25
  * @note   标准表 13（CTS 时间同步）的 7 个字节全部是**压缩 BCD 码**：
  *         一个字节的高 4 位是十位、低 4 位是个位。
  *         接收 CTS 时必须先转换，否则 0x25 会被当成十进制的 37。
  */
static uint8_t bcd2bin(uint8_t b)
{
    return (uint8_t)(((b >> 4) & 0x0Fu) * 10u + (b & 0x0Fu));
}

/*------------------------------------------------------------------------------
 * 周期任务的调度工具
 *---------------------------------------------------------------------------*/

/**
  * @brief  周期任务的绝对调度：把下次发送时刻推进一个周期
  * @param  next   下次发送时刻（毫秒，会被修改）
  * @param  now    当前时刻（毫秒）
  * @param  period 周期（毫秒）
  * @note   **不能用 next = now + period** —— 那是相对调度，每次都会多等
  *         一个 tick 的零头，误差逐次累积。实测 i.MX 端用相对调度时
  *         CCS 周期从 50 ms 偏到 52.9 ms（+5.8 %），因为它的主循环是
  *         5 ms 一拍（usleep(5000)）且循环体还有耗时。
  *         改成 next += period 后零头不累积，平均周期回到标称值。
  *         若落后超过一个周期（被长任务阻塞），重锚到 now + period，
  *         避免连续补发把总线打爆。
  */
#define GB_SCHED_NEXT(next, now, period)                              \
    do {                                                              \
        (next) += (period);                                           \
        if ((int32_t)((now) - (next)) > (int32_t)(period)) {          \
            (next) = (now) + (period);                                \
        }                                                             \
    } while (0)

/*==============================================================================
 * §3  模块静态状态
 *============================================================================*/

static volatile uint32_t s_tick_ms   = 0;      /* SysTick 毫秒计数 */
static BMS_State_t       s_state     = BMS_ST_IDLE;
static BMS_Error_t       s_error     = BMS_ERR_NONE;
static uint32_t          s_state_ms  = 0;      /* 进入当前状态的时间戳 */
static uint32_t          s_last_rx_ms = 0;     /* 最近一次收到充电机报文的时刻 */
static uint32_t          s_last_ccs_ms = 0;    /* 最近一次收到 CCS 的时刻
                                                * 【为什么要单独记】CCS 的超时是标准
                                                * **逐报文**规定的 1 s，与通用的 5 s 不同，
                                                * 共用一个 s_last_rx_ms 就分不出来了。 */

/* ---- CCS 充电机充电状态：收到的实时输出值（标准表 19） ----
 * 本端**只解析、只用于显示与 BEM 判定**，控制逻辑仍走 BCL/BCS/BSM，
 * 不因为收到 CCS 就改变充电流程 —— 避免引入第二套并行控制。 */
static uint8_t  s_ccs_seen    = 0;      /* 1 = 至少收到过一次 CCS */
static uint16_t s_ccs_out_v_x10 = 0;    /* 电压输出值 0.1 V */
static uint16_t s_ccs_out_i_x10 = 0;    /* 电流输出值 0.1 A（**已扣掉 -400 A 偏移**） */
static uint16_t s_ccs_minutes   = 0;    /* 累计充电时间 1 min */
static uint8_t  s_ccs_permit    = 0;    /* SPN3929：0 暂停 / 1 允许 */

/* ---- BEM 错误报文的发送控制（标准表 28） ----
 * 挂在**已有的超时/故障路径**上：进入故障态时记下要上报的错误类别，
 * 然后在故障态里按 250 ms 周期连发，最多 GB_T_BEM_MAX_TX 次后停手
 * （对端始终不来就没必要无限刷屏，故障态本身 10 s 后也会自动复位）。 */
static uint8_t  s_bem_mask     = 0;     /* 本次要上报的错误类别位掩码 */
static uint8_t  s_bem_sent     = 0;     /* 已发送次数 */
static uint32_t s_next_bem_ms  = 0;

/* 周期发送时刻 */
static uint32_t s_next_bhm_ms = 0;
static uint32_t s_next_bro_ms = 0;
static uint32_t s_next_bcl_ms = 0;
static uint32_t s_next_bcs_ms = 0;
static uint32_t s_next_bsm_ms = 0;
static uint32_t s_next_sim_ms = 0;
static uint32_t s_next_log_ms = 0;
static uint32_t s_next_hb_ms  = 0;      /* 下一次发心跳的时刻 */

/* 结束态（BST / BSD）的发送控制。
 *
 * 原来 BST 的判据是"进入结束态后 50 ms 内"、BSD 是"500~540 ms 之间"，
 * 而 StateMachine() 是在主循环里被**高频**调用的 ——
 * 那两个时间窗里会连续发出上千帧，串口直接刷屏，
 * 对端也会被这一串 BST 淹没。
 * 改成"按周期发固定次数"，BST 发 3 次（750 ms），BSD 只发 1 次。 */
#define BMS_BST_TIMES       3u
static uint8_t  s_bst_sent    = 0;
static uint8_t  s_bsd_sent    = 0;
static uint32_t s_next_bst_ms = 0;
static uint32_t s_hb_count    = 0;      /* 累计发出的心跳帧数 */
static uint32_t s_passive_since_ms = 0; /* 进入错误被动的时刻，用于自动恢复 */

/* 报文缓冲 */
static uint8_t s_brm_buf[BRM_LEN];
static uint8_t s_bcp_buf[BCP_LEN];

/* 流程标志 */
static uint8_t s_brm_sent = 0;
static uint8_t s_bcp_sent = 0;
static uint8_t s_cro_ready = 0;      /* 充电机输出准备就绪标志 */
static uint32_t s_session_id = 0;

/* 统计 */
static uint32_t s_rx_count = 0;
static uint32_t s_tx_count = 0;

/* 缓解 USART1 日志与主循环的耦合 */
static uint8_t s_need_status_log = 0;

/*==============================================================================
 * §4  J1939 TP 发送状态机
 *============================================================================*/

typedef enum
{
    TP_IDLE = 0,
    TP_WAIT_CTS,      /* 已发 RTS，等待对端 CTS */
    TP_SENDING,       /* 正在发送 DT */
    TP_DONE,          /* 传输完成 */
    TP_ABORT          /* 传输失败 */
} TP_State_t;

typedef struct
{
    TP_State_t     state;
    const uint8_t *buf;
    uint16_t       total;
    uint16_t       sent;
    uint32_t       pgn;          /* 目标 PGN，写入 TP.CM 的 B6-B8 */
    uint8_t        packets;      /* 总包数 */
    uint8_t        next_seq;     /* 下一个 DT 序号，1 基 */
    uint8_t        bs;           /* 对端允许的包数，0/0xFF 表示不限 */
    uint8_t        block_sent;   /* 当前块已发包数 */
    uint16_t       stmin_ms;     /* 最小包间隔 */
    uint32_t       t_last;       /* 最近一次动作时刻 */
    uint8_t        retry;        /* 已重试次数 */
    uint8_t        ff_sent;      /* RTS 是否已发出 */
} TP_Ctx_t;

static TP_Ctx_t s_tp;

static void TP_Reset(void)
{
    memset(&s_tp, 0, sizeof(s_tp));
    s_tp.state = TP_IDLE;
}

/**
  * @brief  启动一次多帧发送
  * @param  pgn   目标 PGN（放入 TP.CM 的 B6-B8，小端 24 位）
  * @param  buf   数据缓冲（调用期间必须保持有效）
  * @param  len   数据长度（<= 1785）
  */
static void TP_Start(uint32_t pgn, const uint8_t *buf, uint16_t len)
{
    s_tp.state      = TP_WAIT_CTS;
    s_tp.buf        = buf;
    s_tp.total      = len;
    s_tp.sent       = 0;
    s_tp.pgn        = pgn;
    s_tp.packets    = (uint8_t)((len + 6u) / 7u);
    s_tp.next_seq   = 1;
    s_tp.bs         = 0;
    s_tp.block_sent = 0;
    s_tp.stmin_ms   = 0;
    s_tp.t_last     = s_tick_ms;
    s_tp.retry      = 0;
    s_tp.ff_sent    = 0;

    BMS_LOG("[TP] 启动多帧发送: PGN=0x%04lX 长度=%u 共 %u 包\r\n",
            (unsigned long)pgn, (unsigned int)len, (unsigned int)s_tp.packets);
}

/** 发送 RTS（请求发送） */
static void TP_SendRts(void)
{
    uint8_t d[8];

    memset(d, 0xFF, sizeof(d));
    d[0] = 0x10;                                     /* RTS */
    le16_put(&d[1], s_tp.total);                     /* B2-B3 总长度（小端！） */
    d[3] = s_tp.packets;                             /* B4 总包数 */
    d[4] = 0xFF;                                     /* B5 最大响应时间（0xFF=不限） */
    d[5] = (uint8_t)(s_tp.pgn & 0xFF);               /* B6-B8 目标 PGN（小端 24 位） */
    d[6] = (uint8_t)((s_tp.pgn >> 8) & 0xFF);
    d[7] = (uint8_t)((s_tp.pgn >> 16) & 0xFF);

    CAN_SendFrame(GB_ID_TPCM_FROM_BMS, d, 8);

#if (BMS_DEBUG_FRAME_LOG)
    BMS_LOG("[TP] -> RTS  10 %02X %02X %02X FF %02X %02X %02X\r\n",
            d[1], d[2], d[3], d[5], d[6], d[7]);
#endif
}

/** 发送一包 DT（数据传输） */
static void TP_SendDt(void)
{
    uint8_t  d[8];
    uint16_t remain = (uint16_t)(s_tp.total - s_tp.sent);
    uint8_t  n = (remain < 7u) ? (uint8_t)remain : 7u;

    memset(d, 0xFF, sizeof(d));
    d[0] = s_tp.next_seq;
    memcpy(&d[1], &s_tp.buf[s_tp.sent], n);

    CAN_SendFrame(GB_ID_TPDT_FROM_BMS, d, 8);

#if (BMS_DEBUG_FRAME_LOG)
    BMS_LOG("[TP] -> DT   %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
            d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
#endif

    s_tp.sent = (uint16_t)(s_tp.sent + n);
    s_tp.next_seq++;
    if (s_tp.next_seq == 0u)
    {
        s_tp.next_seq = 1u;   /* J1939 序号 1~255 回绕 */
    }
    s_tp.block_sent++;
    s_tp.t_last = s_tick_ms;
}

/**
  * @brief  处理充电机发来的 TP.CM 帧（CTS / EndOfMsgACK / Abort）
  */
static void TP_OnCm(const CAN_Frame_t *frame)
{
    uint8_t ctrl = frame->data[0];

    switch (ctrl)
    {
        case 0x11:   /* CTS —— 允许发送 */
            s_tp.bs         = frame->data[1];                        /* 允许包数 */
            s_tp.stmin_ms   = (uint16_t)(frame->data[3] | ((uint16_t)frame->data[4] << 8));
            s_tp.block_sent = 0;
            s_tp.retry      = 0;
            s_tp.ff_sent    = 1;
            s_tp.state      = TP_SENDING;
            s_tp.t_last     = s_tick_ms;

            BMS_LOG("[TP] <- CTS  允许 %u 包, 从第 %u 包开始, 间隔 %u ms\r\n",
                    (unsigned int)frame->data[1],
                    (unsigned int)frame->data[2],
                    (unsigned int)s_tp.stmin_ms);
            break;

        case 0x13:   /* EndOfMsgACK —— 对端确认收妥 */
            BMS_LOG("[TP] <- EndOfMsgACK 对端已收妥 %u 字节\r\n",
                    (unsigned int)(frame->data[1] | ((uint16_t)frame->data[2] << 8)));
            break;

        case 0xFF:   /* Abort —— 对端中止 */
            BMS_LOG("[TP] <- Abort 对端中止传输, 原因码 0x%02X\r\n",
                    (unsigned int)frame->data[1]);
            s_tp.state = TP_ABORT;
            break;

        default:
            break;
    }
}

/**
  * @brief  TP 状态机推进（每个主循环调用一次）
  */
static void TP_Poll(void)
{
    uint32_t now = s_tick_ms;

    switch (s_tp.state)
    {
        case TP_WAIT_CTS:
            if (!s_tp.ff_sent)
            {
                TP_SendRts();
                s_tp.ff_sent = 1;
                s_tp.t_last  = now;
                break;
            }
            if ((now - s_tp.t_last) > GB_T_TP_TIMEOUT)
            {
                if (s_tp.retry < GB_T_TP_RETRY)
                {
                    s_tp.retry++;
                    s_tp.ff_sent = 0;
                    BMS_LOG("[TP] 等待 CTS 超时, 第 %u 次重试\r\n",
                            (unsigned int)s_tp.retry);
                }
                else
                {
                    BMS_LOG("[TP] 等待 CTS 超时, 重试耗尽, 传输失败\r\n");
                    s_tp.state = TP_ABORT;
                }
                s_tp.t_last = now;
            }
            break;

        case TP_SENDING:
            /* 遵守对端要求的包间隔 */
            if (s_tp.stmin_ms != 0u && (now - s_tp.t_last) < s_tp.stmin_ms)
            {
                break;
            }

            if (s_tp.sent >= s_tp.total)
            {
                /* 全部数据包已发出。
                 * J1939-21 规定发送方应等待 EndOfMsgACK，但国标联调实践中
                 * 接收方（充电机）通常在组包完成后才回 ACK；为缩短握手时间，
                 * 本工程在发完最后一包后立即进入完成态，收到 ACK 时仅打印日志。*/
                BMS_LOG("[TP] 多帧发送完成: 共 %u 包 %u 字节\r\n",
                        (unsigned int)s_tp.packets, (unsigned int)s_tp.total);
                s_tp.state = TP_DONE;
                break;
            }

            TP_SendDt();

            /* 块发送完毕且对端限定了块大小 —— 需要重新等待 CTS */
            if (s_tp.bs != 0u && s_tp.bs != 0xFFu && s_tp.block_sent >= s_tp.bs)
            {
                s_tp.block_sent = 0;
                s_tp.ff_sent    = 0;
                s_tp.retry      = 0;
                s_tp.state      = TP_WAIT_CTS;
                s_tp.t_last     = now;
            }
            break;

        case TP_IDLE:
        case TP_DONE:
        case TP_ABORT:
        default:
            break;
    }
}

/*==============================================================================
 * §5  定点数打印辅助（Keil MicroLIB 下避免使用 %f）
 *============================================================================*/

/** 打印 x.y 格式（输入为放大 10 倍的整数） */
static void print_x10(int32_t v)
{
    int32_t ip;
    int32_t fp;

    if (v < 0)
    {
        ip = v / 10;
        fp = (-v) % 10;
    }
    else
    {
        ip = v / 10;
        fp = v % 10;
    }
    printf("%d.%d", (int)ip, (int)fp);
}

/** 把 CAN_ESR 的 LEC 错误码转成可读字符串 */
static const char *LecStr(uint8_t lec)
{
    switch (lec)
    {
        case 0: return "无错";
        case 1: return "填充错误";
        case 2: return "格式错误";
        case 3: return "ACK错误(无人应答)";
        case 4: return "位隐性错误";
        case 5: return "位显性错误";
        case 6: return "CRC错误";
        default: return "软件置位";
    }
}

/** 打印 x.yy 格式（输入为放大 100 倍的整数） */
static void print_x100(int32_t v)
{
    int32_t ip;
    int32_t fp;

    if (v < 0)
    {
        ip = v / 100;
        fp = (-v) % 100;
    }
    else
    {
        ip = v / 100;
        fp = v % 100;
    }
    if (fp < 10)
    {
        printf("%d.0%d", (int)ip, (int)fp);
    }
    else
    {
        printf("%d.%d", (int)ip, (int)fp);
    }
}

/*==============================================================================
 * §6  报文组包
 *============================================================================*/

/* 电池模型里被组包函数用到的状态量。
 * 【为什么在这里定义】ARM Compiler 5 是 C90：不允许先使用后声明。
 * 组包函数（Build_BCS / Build_BSM）要用 SOC、电流和最高单体电压，
 * 所以把这三个**定义**提到 §6 之前；§7 里只留仿真逻辑，不再重复定义。
 *   注意：不要写成"先 static 空声明、后面再带初值定义一次" —— 那是
 *   暂定定义 + 定义，C90 下属于重复定义，AC5 会报错。 */
static uint16_t s_soc_x10     = SIM_START_SOC_X10;   /* SOC，0.1 % */
static uint16_t s_current_x10 = 0;                   /* 实际电流（未加偏移），0.1 A */
static uint16_t Sim_CellMaxVoltage_X100(void);       /* 函数声明：定义在 §7 */

/**
  * @brief  组装 BHM 车辆握手报文
  * @note   标准表 9：BHM 数据域 **2 字节** —— B1-B2「最高允许充电总电压」
  *         （SPN2601，0.1 V/位，0 偏移，**小端**，标准 4.4 低字节先发送）。
  *         ★ 标准 BHM 里**没有版本号字段** —— 协议版本号只在 CHM 里。
  *         这两字节都是有效字段，报文内没有需要填 1 的保留位。
  *         本端上报的是 BCP 里那个"最高允许充电总电压"，保证前后一致。
  */
static void Build_BHM(uint8_t *d)
{
    le16_put(&d[BHM_OFF_MAX_TOTAL_V], (uint16_t)SIM_MAX_TOTAL_V_X10);
    /* 注：BMS 源地址由 CAN ID 的 SA 字段(0xF4)承载，标准规定数据域中不重复。 */
}

/**
  * @brief  组装 BRM BMS 和车辆辨识报文（41 字节，多字节字段全部小端）
  * @note   字段顺序严格按标准表 11，见文件顶部 BRM 段的说明。
  *         ? 标准原文长度不自洽（表 5 声明 41、表 11 相加得 49），
  *         本工程保持 41 字节，没有 SPN2576（BMS 软件版本号）。
  */
static void Build_BRM(uint8_t *d)
{
    /* 先按标准 7.9 把整段填 1，再把有效字段逐个写回。
     * B24 是标准未规定的预留字节，B25-B41 是 VIN（本工程填满），
     * 因此没有残留的填充字节。 */
    fill_reserved(d, BRM_LEN);

    /* B1-B3 BMS 通信协议版本号：V1.1 = byte3,byte2—0001H；byte1—01H
     *        ? 小端 3 字节 01 01 00 */
    d[BRM_OFF_VERSION + 0] = 0x01;
    d[BRM_OFF_VERSION + 1] = 0x01;
    d[BRM_OFF_VERSION + 2] = 0x00;
    /* B4 电池类型：03 = 磷酸铁锂（标准表 11 的 SPN2566） */
    d[BRM_OFF_BATT_TYPE] = BMS_BATT_TYPE_LFP;
    /* B5-B6 额定容量 0.1 Ah */
    le16_put(&d[BRM_OFF_RATED_CAP], SIM_RATED_CAP_X10);
    /* B7-B8 额定总电压 0.1 V */
    le16_put(&d[BRM_OFF_RATED_V], SIM_RATED_V_X10);
    /* B9-B12 生产厂商 4 字节标准 ASCII */
    memcpy(&d[BRM_OFF_MANUFACTURER], BMS_MANUFACTURER, BRM_LEN_MANUFACTURER);
    /* B13-B16 电池组序号（预留，厂商自定义） */
    memcpy(&d[BRM_OFF_PACK_SERIAL], BMS_PACK_SERIAL, BRM_LEN_PACK_SERIAL);
    /* B17 生产年份：1 年/位，**1985 年偏移**
     *        所以 2024 年要存 2024 - 1985 = 39，不是 2024 */
    d[BRM_OFF_PROD_YEAR] = (uint8_t)(BMS_PROD_YEAR - BRM_YEAR_OFFSET);
    /* B18 月份 B19 日 */
    d[BRM_OFF_PROD_MONTH] = BMS_PROD_MONTH;
    d[BRM_OFF_PROD_DAY]   = BMS_PROD_DAY;
    /* B20-B22 电池组充电次数（小端 3 字节） */
    le24_put(&d[BRM_OFF_CHARGE_CNT], BMS_CHARGE_COUNT);
    /* B23 电池组产权标识：0 租赁 / 1 车自有 */
    d[BRM_OFF_OWNERSHIP] = BMS_OWNERSHIP;
    /* B24 预留：保持 fill_reserved() 填的 1（标准 7.9） */
    /* B25-B41 车辆识别码 VIN 17 字节 ASCII */
    memcpy(&d[BRM_OFF_VIN], BMS_VIN, BRM_LEN_VIN);
}

/** 组装 BCP 动力蓄电池充电参数（13 字节） */
static void Build_BCP(uint8_t *d, uint16_t soc_x10, uint16_t voltage_x10)
{
    /* 13 字节全部是标准规定的有效字段，填 1 后逐个写回（无残留填充位）。 */
    fill_reserved(d, BCP_LEN);

    /* B1-B2 单体最高允许充电电压 0.01 V */
    le16_put(&d[BCP_OFF_MAX_CELL_V], SIM_MAX_CELL_V_X100);
    /* B3-B4 最高允许充电电流 0.1 A, -400 A 偏移 */
    le16_put(&d[BCP_OFF_MAX_I], (uint16_t)(SIM_MAX_CURRENT_X10 + OFF_I_400_X10));
    /* B5-B6 动力蓄电池标称总能量 0.1 kWh */
    le16_put(&d[BCP_OFF_ENERGY], SIM_ENERGY_X10);
    /* B7-B8 最高允许充电总电压 0.1 V */
    le16_put(&d[BCP_OFF_MAX_TOTAL_V], SIM_MAX_TOTAL_V_X10);
    /* B9 最高允许温度 1 C, -50 C 偏移 */
    d[BCP_OFF_MAX_TEMP] = (uint8_t)(SIM_MAX_TEMP_C + OFF_T_50);
    /* B10-B11 整车 SOC **0.1 %/位**（注意 BCS 的 SOC 是 1 %/位） */
    le16_put(&d[BCP_OFF_SOC], soc_x10);
    /* B12-B13 整车动力蓄电池当前电压 0.1 V */
    le16_put(&d[BCP_OFF_CUR_V], voltage_x10);
}

/**
  * @brief  组装 BCS 电池充电总状态（标准 9 字节 / 本工程发 8 字节）
  * @param  d            输出缓冲，至少 BCS_LEN(8) 字节
  * @param  voltage_x10  充电电压测量值，0.1 V
  * @param  current_x10  充电电流测量值，0.1 A（**未加偏移**，函数内自行加）
  * @param  cell_x100    最高单体电压，0.01 V
  * @param  cell_no      最高单体编号（1 基），用于算所在组号
  * @param  soc_x10      当前 SOC，0.1 %
  *
  * @note   B5-B6 是**位打包**字段（标准表 18 的 SPN3077）：
  *           1-12 位 = 电压 0.01 V/位；13-16 位 = 所在组号 1/位
  *         本工程是 160 串 1 并（SIM_CELL_COUNT），没有分组，
  *         所以按标准表 21 的注"若无分组号，则按 256 个单体为一组发送"，
  *         组号 = (编号 - 1) / 256，落在 0~15 内。
  *         **这一条属于"标准允许的取值约定"，如需按实际分组号上报请告知。**
  */
static void Build_BCS(uint8_t *d, uint16_t voltage_x10, uint16_t current_x10,
                      uint16_t cell_x100, uint8_t cell_no, uint16_t soc_x10)
{
    uint16_t packed;
    uint8_t  group;

    fill_reserved(d, BCS_LEN);

    /* B1-B2 充电电压测量值 0.1 V */
    le16_put(&d[BCS_OFF_MEASURE_V], voltage_x10);
    /* B3-B4 充电电流测量值 0.1 A, -400 A 偏移 */
    le16_put(&d[BCS_OFF_MEASURE_I], (uint16_t)(current_x10 + OFF_I_400_X10));

    /* B5-B6 最高单体电压(1-12 位) + 所在组号(13-16 位) */
    if (cell_x100 > BCS_CELL_V_MAX_X100)
    {
        cell_x100 = (uint16_t)BCS_CELL_V_MAX_X100;   /* 标准上限 24 V */
    }
    group  = (uint8_t)(((uint16_t)(cell_no - 1u) / 256u) & BCS_CELL_GROUP_MASK);
    packed = (uint16_t)((cell_x100 & BCS_CELL_V_MASK) |
                        ((uint16_t)group << BCS_CELL_GROUP_SHIFT));
    le16_put(&d[BCS_OFF_CELL_V_GROUP], packed);

    /* B7 当前 SOC：**1 %/位**（BCP 里是 0.1 %/位，别搞混） */
    d[BCS_OFF_SOC] = (uint8_t)(soc_x10 / 10u);

    /* B8 估算剩余充电时间（标准表 18 的 SPN3079：1 min/位，0~600，超 600 按 600 发）
     *
     * 【为什么发 0xFF 而不是 600】
     *   600 的语义是"**至少还要 600 分钟**"，那是标准留给"估算值溢出"的上限。
     *   本工程**没有剩余时间估算能力**，填 600 等于凭空编造一个具体数值；
     *   填 0 更糟 —— 会被读成"还剩 0 分钟、马上充满"，是明确的错误信息。
     *   所以按标准 7.9「本标准未规定的位或字段填充 1」，
     *   用 0xFF 表示"该项未规定 / 无有效值"。
     *
     * 【已知偏差】B8-B9 本应是 2 字节，本工程只发 8 字节单帧，只有 B8。
     *   0xFF 在这里同时覆盖"未估算"和"高字节缺失"两种情况，语义不冲突。 */
    d[BCS_OFF_REMAIN_LO] = (uint8_t)GB_FILL_RESERVED;   /* 0xFF = 未规定 / 无估算值 */
}

/**
  * @brief  组装 BSM 动力蓄电池状态信息（标准 7 字节）
  * @param  d          输出缓冲，至少 BSM_LEN(7) 字节
  * @param  cell_no    最高单体电压所在编号（1 基，1~256）
  * @param  t_max_c    最高温度 ℃（函数内加 -50 ℃ 偏移）
  * @param  t_max_no   最高温度检测点编号（1 基，1~128）
  * @param  t_min_c    最低温度 ℃
  * @param  t_min_no   最低温度检测点编号
  * @param  charge_ok  充电允许：BSM_CHARGE_ALLOWED / BSM_CHARGE_FORBIDDEN
  *
  * @note   **标准 BSM 里没有电压字段** —— B1 只是"最高单体电压所在编号"，
  *         电压本身在 BCS 的 B5-B6 里。
  *
  *         【状态位的填法 —— 不知道的一律填"不可信"，不要填"正常"】
  *           SPN3090 单体电压过高/过低：拿最高单体电压与 BCP 的单体上限比
  *           SPN3091 整车 SOC 过高/过低：SOC >= 100% 填 01；SOC <= 0% 填 10
  *           SPN3092 充电过电流：拿充电电流与 BCP 的电流上限比
  *           SPN3093 温度过高：拿最高温度与 BCP 的温度上限比
  *           SPN3094 绝缘状态：**本工程没有绝缘检测 → 如实填 10（不可信）**
  *           SPN3095 输出连接器：**本工程没有连接器检测 → 如实填 10（不可信）**
  *           SPN3096 充电允许：充电阶段填 01；不能充电时填 00
  *           B7.7-7.8 标准未定义 → 按 7.9 填 1
  */
static void Build_BSM(uint8_t *d, uint8_t cell_no,
                      uint8_t t_max_c, uint8_t t_max_no,
                      uint8_t t_min_c, uint8_t t_min_no,
                      uint8_t charge_ok)
{
    uint8_t  b6 = 0;
    uint8_t  b7;
    uint8_t  st;
    uint16_t cell_v_x100 = Sim_CellMaxVoltage_X100();

    fill_reserved(d, BSM_LEN);

    /* --- B1-B5：编号与温度（温度都要加 -50 ℃ 偏移） --- */
    d[BSM_OFF_CELL_V_NO]   = cell_no;
    d[BSM_OFF_MAX_TEMP]    = (uint8_t)(t_max_c + OFF_T_50);
    d[BSM_OFF_MAX_TEMP_NO] = (t_max_no == 0u) ? 1u : t_max_no;   /* 1 偏移，0 视为 1 */
    d[BSM_OFF_MIN_TEMP]    = (uint8_t)(t_min_c + OFF_T_50);
    d[BSM_OFF_MIN_TEMP_NO] = (t_min_no == 0u) ? 1u : t_min_no;

    /* --- B6：4 个 2 位状态字段 --- */
    /* SPN3090 单体电压过高/过低：
     *   电压 > 单体最高允许充电电压 → 01 过高
     *   电压 < 单体最低允许放电电压（本工程取 2.00 V）→ 10 过低
     *   否则 00 正常 */
    if (cell_v_x100 > SIM_MAX_CELL_V_X100)
    {
        st = BSM_ST_HIGH;
    }
    else if (cell_v_x100 < SIM_MIN_CELL_V_X100)
    {
        st = BSM_ST_LOW;
    }
    else
    {
        st = BSM_ST_NORMAL;
    }
    b6 |= (uint8_t)((st & 0x03u) << BSM_B6_CELL_V_SHIFT);

    /* SPN3091 整车 SOC 过高/过低 */
    if (s_soc_x10 >= 1000u)
    {
        st = BSM_ST_HIGH;
    }
    else if (s_soc_x10 == 0u)
    {
        st = BSM_ST_LOW;
    }
    else
    {
        st = BSM_ST_NORMAL;
    }
    b6 |= (uint8_t)((st & 0x03u) << BSM_B6_SOC_SHIFT);

    /* SPN3092 充电过电流：充电电流超过 BCP 里的最高允许充电电流 → 01 过流
     *   （枚举只有 00 正常 / 01 过流 / 10 不可信，没有"过低"） */
    st = (s_current_x10 > SIM_MAX_CURRENT_X10) ? BSM_ST_HIGH : BSM_ST_NORMAL;
    b6 |= (uint8_t)((st & 0x03u) << BSM_B6_OVER_I_SHIFT);

    /* SPN3093 温度过高：最高温度达到 BCP 里的最高允许温度 → 01 过高 */
    st = (t_max_c >= SIM_MAX_TEMP_C) ? BSM_ST_HIGH : BSM_ST_NORMAL;
    b6 |= (uint8_t)((st & 0x03u) << BSM_B6_OVER_T_SHIFT);

    d[BSM_OFF_STATUS_B6] = b6;

    /* --- B7：3 个 2 位状态字段 + 未定义位 --- */
    b7 = 0;
    /* SPN3094 绝缘状态：没有检测能力 → 10 不可信（**不要填 00 正常冒充**） */
    b7 = GB_FIELD2_SET(b7, BSM_B7_INSULATION_SHIFT, BSM_ST_UNRELIABLE);
    /* SPN3095 输出连接器连接状态：同上 → 10 不可信 */
    b7 = GB_FIELD2_SET(b7, BSM_B7_CONNECTOR_SHIFT, BSM_ST_UNRELIABLE);
    /* SPN3096 充电允许：00 禁止 / 01 允许 */
    b7 = GB_FIELD2_SET(b7, BSM_B7_PERMIT_SHIFT,
                       (charge_ok != 0u) ? BSM_CHARGE_ALLOWED : BSM_CHARGE_FORBIDDEN);
    /* B7.7-7.8 标准未定义 → 按 7.9 填 1 */
    b7 |= (uint8_t)(0x03u << BSM_B7_UNDEFINED_SHIFT);

    d[BSM_OFF_STATUS_B7] = b7;
}

/**
  * @brief  组装 BSD BMS 统计数据（标准表 26：7 字节）
  * @note   B1 中止 SOC(1 %/位)；B2-B3 单体**最低**电压(0.01 V)；
  *         B4-B5 单体**最高**电压(0.01 V)；B6 **最低**温度；B7 **最高**温度。
  */
static void Build_BSD(uint8_t *d, uint8_t soc_pct,
                      uint16_t min_cell_x100, uint16_t max_cell_x100,
                      uint8_t t_min_c, uint8_t t_max_c)
{
    fill_reserved(d, BSD_LEN);

    d[BSD_OFF_SOC] = soc_pct;
    le16_put(&d[BSD_OFF_MIN_CELL_V], min_cell_x100);
    le16_put(&d[BSD_OFF_MAX_CELL_V], max_cell_x100);
    d[BSD_OFF_MIN_TEMP] = (uint8_t)(t_min_c + OFF_T_50);
    d[BSD_OFF_MAX_TEMP] = (uint8_t)(t_max_c + OFF_T_50);
}

/*==============================================================================
 * §7  电池模型仿真
 *============================================================================*/

/* s_soc_x10 / s_current_x10 的**定义**在 §6 之前（组包函数要先用到），
 * 这里不再重复定义。 */
static uint16_t s_voltage_x10 = SIM_START_V_X10;     /* 总压，0.1 V */
static uint8_t  s_temp_max_c  = SIM_TEMP_BASE_C;     /* 最高温度，℃（已加偏移前） */
static uint8_t  s_temp_min_c  = SIM_TEMP_BASE_C;     /* 最低温度，℃ */
static uint8_t  s_cell_max_no = 1;                   /* 最高单体电压编号 */

/* 累计统计 */
static uint32_t s_charge_seconds = 0;                /* 累计充电秒数 */
static uint32_t s_charge_energy_x10 = 0;             /* 累计电量，0.1 kWh */
static uint8_t  s_stop_reason = 0;                   /* 中止原因 */

/*==============================================================================
 *                        充电阶段（SOC 的去向）
 *
 *   待机   上电后什么都不做，电量保持不变（不自动充电）
 *   充电中 收到 CHM（i.MX 按了充电按钮）后，SOC 按 SIM_SOC_STEP_X10 上升
 *   充满   SOC 到 100% 后停止充电（发 BST 收尾），两边都显示充满
 *   放电中 停止充电后，SOC 按充电时同样的速率下降，掉到 0% 就停住
 *
 *   注意：进入新会话时不再把 SOC 复位成初始值 —— 电量是电池的属性，
 *         必须跨会话、跨重启保持（存在 Flash 里）。
 *============================================================================*/
typedef enum
{
    SIM_PHASE_STANDBY = 0,      /* 待机：不充也不放 */
    SIM_PHASE_CHARGING,         /* 充电中 */
    SIM_PHASE_FULL,             /* 充满：停止充电 */
    SIM_PHASE_DISCHARGE         /* 放电中：按充电同速率掉电 */
} Sim_Phase_t;

static Sim_Phase_t s_phase        = SIM_PHASE_STANDBY;
static uint16_t    s_full_ticks   = 0;      /* 处于充满阶段已经过了几拍 */
static uint16_t    s_saved_soc    = 0xFFFFu;
static uint32_t    s_last_save_ms = 0;

/*--------------------------- Flash 掉电记忆 ---------------------------
 * 用 STM32F103ZE 的最后一页（2 KB）存 SOC 与充电阶段。
 * 记录带魔数和异或校验：Flash 全 0xFF（从未写过）或校验不过时按没有记录处理，
 * 这样第一次上电会退回 SIM_START_SOC_X10。
 *---------------------------------------------------------------------*/
#define BMS_FLASH_MAGIC     0x424DU                 /* BM */
#define BMS_FLASH_ADDR      0x0807F800UL            /* 最后一页基址 */

typedef struct
{
    uint16_t magic;
    uint16_t soc_x10;
    uint16_t phase;
    uint16_t chk;
} bms_flash_rec_t;

static uint16_t flash_chk(uint16_t magic, uint16_t soc, uint16_t phase)
{
    return (uint16_t)(magic ^ soc ^ phase);
}

/** 从 Flash 读回上次的电量；没有有效记录时保持当前值不变 */
static void Sim_FlashLoad(void)
{
    const bms_flash_rec_t *rec = (const bms_flash_rec_t *)BMS_FLASH_ADDR;

    if (rec->magic != (uint16_t)BMS_FLASH_MAGIC) { return; }
    if (rec->chk != flash_chk(rec->magic, rec->soc_x10, rec->phase)) { return; }
    if (rec->soc_x10 > 1000u || rec->phase > (uint16_t)SIM_PHASE_DISCHARGE) { return; }

    s_soc_x10 = rec->soc_x10;
    s_phase   = (Sim_Phase_t)rec->phase;

    /* 上电时不可能正在充电。上次存的是充电中，说明运行中被断电了，
     * 恢复成放电中更符合真实电池的行为。 */
    if (s_phase == SIM_PHASE_CHARGING)
    {
        s_phase = SIM_PHASE_DISCHARGE;
    }

    BMS_LOG("[BMS] 掉电记忆: 读回上次电量 %u.%u%%, 阶段=%u\r\n",
            (unsigned int)(s_soc_x10 / 10u), (unsigned int)(s_soc_x10 % 10u),
            (unsigned int)s_phase);
}

/** 把当前电量写进 Flash（整页擦除 + 4 个半字） */
static void Sim_FlashSave(void)
{
    bms_flash_rec_t rec;
    const uint16_t *p;
    uint8_t i;

    rec.magic   = (uint16_t)BMS_FLASH_MAGIC;
    rec.soc_x10 = s_soc_x10;
    rec.phase   = (uint16_t)s_phase;
    rec.chk     = flash_chk(rec.magic, rec.soc_x10, rec.phase);

    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    (void)FLASH_ErasePage(BMS_FLASH_ADDR);

    p = (const uint16_t *)&rec;
    for (i = 0; i < 4u; i++)
    {
        (void)FLASH_ProgramHalfWord(BMS_FLASH_ADDR + (uint32_t)i * 2u, p[i]);
    }
    FLASH_Lock();

    s_saved_soc = s_soc_x10;
}

/**
  * @brief  复位电池模型的会话统计
  * @note   刻意不复位 SOC 与充电阶段：电量是电池的属性，要跨会话、跨重启保持。
  *         SOC 只在 Flash 里没有有效记录时，才由 BMS_Protocol_Init() 设成
  *         SIM_START_SOC_X10。
  */
static void Sim_Reset(void)
{
    s_charge_seconds    = 0;
    s_charge_energy_x10 = 0;
    s_stop_reason       = 0;
    s_full_ticks        = 0;

    /* 电压/温度跟着当前 SOC 重新算一遍，避免残留上一次会话的数值 */
    s_current_x10 = 0;
}

/**
  * @brief  电池模型推进一步（每 GB_T_SIM_PERIOD 毫秒调用一次）
  * @note   模拟恒流-恒压两段式充电：
  *           恒流段：电流维持 100 A，电压随 SOC 线性上升；
  *           恒压段：电压到达 584 V 后电流按 SOC 反比衰减。
  */
static void Sim_Step(void)
{
    int32_t v;
    int32_t i;
    int32_t t;

    /* ---------------- 1. 电量推进：充电 / 充满 / 掉电 ---------------- */
    switch (s_phase)
    {
        case SIM_PHASE_CHARGING:
            /* 保险：会话已经**彻底结束或出错**时才转入掉电。
             *
             * 【踩过的坑】这里原来写的是 `s_state != BMS_ST_CHARGING` ——
             * 而握手 / 辨识 / 参数配置 / 充电准备这几个中间阶段本来就不等于
             * CHARGING，于是握手刚一开始阶段就被打回"放电中"；
             * 等真正进到充电阶段时阶段已经是放电，表现就是
             * "点了充电、i.MX 显示充电中，电量却一直往下掉"。
             * 只有下面这三个状态才算真的结束了。 */
            if (s_state == BMS_ST_IDLE ||
                s_state == BMS_ST_STOPPING ||
                s_state == BMS_ST_FAULT)
            {
                s_phase = SIM_PHASE_DISCHARGE;
                BMS_LOG("[BMS] 充电已停止，转入掉电\r\n");
                break;
            }

            /* 会话进行中但还没走到充电阶段（握手/辨识/配置/准备）：
             * 阶段保持"充电中"，但这一拍先不涨电 —— 真正的充电从 CHARGING 开始。 */
            if (s_state != BMS_ST_CHARGING)
            {
                break;
            }

            s_charge_seconds++;
            if (s_soc_x10 < 1000u)
            {
                s_soc_x10 = (uint16_t)(s_soc_x10 + SIM_SOC_STEP_X10);
                if (s_soc_x10 > 1000u)
                {
                    s_soc_x10 = 1000u;
                }
            }
            if (s_soc_x10 >= 1000u)
            {
                /* 充满：切到充满阶段，状态机那边会发 BST 收尾 */
                s_phase      = SIM_PHASE_FULL;
                s_full_ticks = 0;
                BMS_LOG("[BMS] *** 电池已充满, 停止充电, 转入掉电 ***\r\n");
            }
            break;

        case SIM_PHASE_FULL:
            /* 先保持几拍，等状态机把 BST / BSD 发完，再开始掉电 */
            s_full_ticks++;
            if (s_full_ticks >= SIM_FULL_HOLD_TICKS)
            {
                s_phase = SIM_PHASE_DISCHARGE;
                BMS_LOG("[BMS] 开始按充电同速率掉电\r\n");
            }
            break;

        case SIM_PHASE_DISCHARGE:
            /* 按充电时的同样速率掉电；掉到 0% 就停住 */
            if (s_soc_x10 >= SIM_DISCHARGE_STEP_X10)
            {
                s_soc_x10 = (uint16_t)(s_soc_x10 - SIM_DISCHARGE_STEP_X10);
            }
            else
            {
                s_soc_x10 = 0u;
            }
            break;

        default:    /* SIM_PHASE_STANDBY：待机，电量保持不变 */
            break;
    }

    /* ---------------- 2. 电压：始终跟随当前 SOC 的开路电压 ----------------
     * SOC 可能低于初始值（掉电阶段），所以必须用有符号运算，
     * 不能像原来那样直接做无符号减法 —— 那会下溢成一个巨大的数。 */
    {
        int32_t d    = (int32_t)s_soc_x10 - (int32_t)SIM_START_SOC_X10;
        int32_t span = (int32_t)(1000u - SIM_START_SOC_X10);

        v = (int32_t)SIM_START_V_X10 +
            (d * ((int32_t)SIM_END_V_X10 - (int32_t)SIM_START_V_X10)) / span;

        /* 电压钳位在模型允许的区间内：下限用 SIM_FLOOR_V_X10（宏），
         * BSM 判"单体电压过低"的 SIM_MIN_CELL_V_X100 就是由这个下限折算来的，
         * 两者共用同一个来源，避免"模型下限改了、状态位门限没跟着改"。 */
        if (v < (int32_t)SIM_FLOOR_V_X10) { v = (int32_t)SIM_FLOOR_V_X10; }
        if (v > (int32_t)SIM_END_V_X10)   { v = (int32_t)SIM_END_V_X10; }
    }

    /* ---------------- 3. 电流：只有真正在充电时才有电流 ---------------- */
    if (s_phase == SIM_PHASE_CHARGING && s_state == BMS_ST_CHARGING)
    {
        if (s_soc_x10 >= BMS_CV_SOC_X10)
        {
            i = ((int32_t)SIM_MAX_CURRENT_X10 * (int32_t)(1000u - s_soc_x10)) / 50;
            if (i < 20)
            {
                i = 20;    /* 涓流下限 */
            }
        }
        else
        {
            i = (int32_t)SIM_MAX_CURRENT_X10;
        }
    }
    else
    {
        i = 0;          /* 待机 / 充满 / 掉电：没有充电电流 */
    }

    /* 4. 温度：随 SOC 缓慢变化（每 50 个 0.1% SOC 变化 1 ℃） */
    t = SIM_TEMP_BASE_C +
        (((int32_t)s_soc_x10 - (int32_t)SIM_START_SOC_X10) / 50);
    if (t > SIM_MAX_TEMP_C - 5)   { t = SIM_MAX_TEMP_C - 5; }
    if (t < SIM_TEMP_BASE_C - 15) { t = SIM_TEMP_BASE_C - 15; }

    s_voltage_x10 = (uint16_t)v;
    s_current_x10 = (uint16_t)i;
    s_temp_max_c  = (uint8_t)t;
    s_temp_min_c  = (uint8_t)(t - 6);

    /* 5. 最高单体电压编号：随 SOC 缓慢漂移，模拟热点位置变化 */
    s_cell_max_no = (uint8_t)(((s_soc_x10 / 7u) % SIM_CELL_COUNT) + 1u);

    /* 6. 累计电量：P = U * I，单位换算 0.1V * 0.1A / 100 = W，
     *    再乘 1 秒 / 3600 / 1000 得到 kWh，最后放大 10 倍存为 0.1 kWh */
    if (s_phase == SIM_PHASE_CHARGING)
    {
        s_charge_energy_x10 += (uint32_t)(((uint32_t)s_voltage_x10 *
                                           (uint32_t)s_current_x10) / 3600000u);
    }

    /* ---------------- 7. 掉电记忆：变化够大才写 Flash ----------------
     * 每 50 个 0.1%（5%）写一次，或者每 10 秒写一次。
     * 一次 1 秒的仿真拍里 SOC 只动 0.5%，所以正常情况下约 10 秒写一次，
     * Flash 擦写寿命完全够用。 */
    if ((s_soc_x10 > s_saved_soc && (uint16_t)(s_soc_x10 - s_saved_soc) >= 50u) ||
        (s_soc_x10 < s_saved_soc && (uint16_t)(s_saved_soc - s_soc_x10) >= 50u) ||
        (s_saved_soc == 0xFFFFu))
    {
        Sim_FlashSave();
        s_last_save_ms = s_tick_ms;
    }
    else if ((s_tick_ms - s_last_save_ms) >= 10000u)
    {
        Sim_FlashSave();
        s_last_save_ms = s_tick_ms;
    }
}

/** 计算当前最高单体电压（单位 0.01 V） */
static uint16_t Sim_CellMaxVoltage_X100(void)
{
    /* 单体电压(0.01V) = 总压(0.1V) * 10 / 单体数 */
    return (uint16_t)(((uint32_t)s_voltage_x10 * 10u) / SIM_CELL_COUNT);
}

/*==============================================================================
 * §8  CAN 报文接收处理
 *============================================================================*/

/** 打印一帧报文的十六进制内容（调试用） */
static void LogFrame(const char *dir, uint32_t id, const uint8_t *d, uint8_t len)
{
#if (BMS_DEBUG_FRAME_LOG)
    uint8_t i;

    BMS_LOG("%s 0x%08lX [%u] ", dir, (unsigned long)id, (unsigned int)len);
    for (i = 0; i < len; i++)
    {
        BMS_LOG("%02X ", d[i]);
    }
    BMS_LOG("\r\n");
#else
    (void)dir; (void)id; (void)d; (void)len;
#endif
}

/** 状态迁移：更新时间戳并打印日志 */
static void SetState(BMS_State_t st)
{
    if (s_state == st)
    {
        return;
    }

    BMS_LOG("[BMS] 状态切换: %s -> %s\r\n", BMS_StateStr(s_state), BMS_StateStr(st));
    s_state    = st;
    s_state_ms = s_tick_ms;

    /* 进入握手 = 收到 CHM = i.MX 那边按下了"充电"按钮。
     * 到这一刻才真正开始充电：之前无论是待机还是掉电，都不动 SOC。 */
    if (st == BMS_ST_HANDSHAKE)
    {
        s_phase      = SIM_PHASE_CHARGING;
        s_full_ticks = 0;
    }

    /* 进入结束态：复位 BST / BSD 的发送计数，让它们从头发一遍 */
    if (st == BMS_ST_STOPPING)
    {
        s_bst_sent    = 0;
        s_bsd_sent    = 0;
        s_next_bst_ms = s_tick_ms;
    }

    /* 【关键】充电一停下来，"充电阶段"就必须跟着离开 CHARGING。
     *
     * 屏幕上显示的不是协议状态，而是这个 s_phase；Sim_Step() 也是靠它
     * 决定 SOC 涨还是跌。以前只改协议状态、不改阶段，后果是：
     *   充电机按了「停止」（或 BMS 自己中止），协议层确实停了、也发了 BST，
     *   但 s_phase 还停在 CHARGING —— 于是屏幕一直显示「充电中」、
     *   SOC 还继续往上爬。现场看到的就是"点了停止，STM32 还在继续充电"。
     *
     * 停下来的去向：已经满了就是"充满"，否则按用户要求进入"放电中"
     * （停止充电后电量按充电同速率往下掉）。 */
    if (st == BMS_ST_STOPPING || st == BMS_ST_IDLE)
    {
        if (s_phase == SIM_PHASE_CHARGING)
        {
            s_phase      = (s_soc_x10 >= 1000u) ? SIM_PHASE_FULL : SIM_PHASE_DISCHARGE;
            s_full_ticks = 0;
        }
    }
}

/** 进入错误态：置位错误标志，并按错误性质决定去哪个状态
 *
 * 【两类错误要分开走，否则 BMS_ST_FAULT 是死代码】
 *   硬件类故障（BMS_ERR_BUS_OFF）：总线已经被控制器关掉了，本节点此刻谁也
 *     联系不上。进"故障态"驻留，等下面的总线恢复逻辑把控制器拉回来，
 *     10 秒后状态机自己复位重试。这条路径以前走不到 —— Bus-Off 检测块是
 *     直接给 s_error 赋值、没经过这里，所以 BMS_ST_FAULT 分支和它那套
 *     "故障态持续 10 秒，自动复位重试"逻辑一直没人触发。
 *   协议类错误（各类超时、多帧失败）：只是这一轮对话没谈成，回空闲态等
 *     充电机重新发 CHM 即可，不需要"自动重试"的语义，也不能把总线占着。
 */
/**
  * @brief  进入错误态：置位错误标志，并按错误性质决定去哪个状态
  *
  * 【两类错误要分开走，否则 BMS_ST_FAULT 是死代码】
  *   硬件类故障（BMS_ERR_BUS_OFF）：总线已经被控制器关掉了，本节点此刻谁也
  *     联系不上。进"故障态"驻留，等下面的总线恢复逻辑把控制器拉回来，
  *     10 秒后状态机自己复位重试。这条路径以前走不到 —— Bus-Off 检测块是
  *     直接给 s_error 赋值、没经过这里，所以 BMS_ST_FAULT 分支和它那套
  *     "故障态持续 10 秒，自动复位重试"逻辑一直没人触发。
  *   协议类错误（各类超时、多帧失败）：只是这一轮对话没谈成，回空闲态等
  *     充电机重新发 CHM 即可，不需要"自动重试"的语义，也不能把总线占着。
  *
  * 【BEM 错误掩码】各类超时都要按标准表 28 映射到一个 SPN 字段上报
  *   （SPN3901~3907），映射表见函数体内注释。调用点不必自己设置 s_bem_mask。
  */
static uint8_t BemMaskOf(BMS_Error_t err)
{
    /* ---- 类别 1：需要充电机来"辨识自己"的报文没收到 ----
     * SPN3901 接收 SPN2560=0x00 的充电机辨识报文超时
     * SPN3902 接收 SPN2560=0xAA 的充电机辨识报文超时
     *   本端是模拟 BMS，始终能辨识充电机，因此只上报 0xAA 那一项。
     *   **不把 SPN3901 也填成超时** —— 标准区分这两个字段的原意是让 BMS
     *   说明"是没收到 0x00 还是没收到 0xAA"，两个都填会在联调时误导排查方向。 */
    switch (err)
    {
        case BMS_ERR_CRM_TIMEOUT:
            return BEM_ERR_CRM_IDENT;

        /* ---- 类别 2：充电机应主动发来的周期/参数报文 ----
         * SPN3903 接收充电机的时间同步和最大输出能力报文超时（CTS / CML）
         * SPN3904 接收充电机完成充电准备报文超时（CRO）
         * SPN3905 接收充电机充电状态报文超时（CCS，标准逐报文规定 1 s） */
        case BMS_ERR_CML_TIMEOUT:
            return BEM_ERR_CTS_CML;
        case BMS_ERR_CRO_TIMEOUT:
            return BEM_ERR_CRO;
        case BMS_ERR_CCS_TIMEOUT:
            return BEM_ERR_CCS;

        /* ---- 类别 3：充电阶段对端整体失联 ----
         * 标准没给"什么都没收到"这一项，取语义最接近的
         * SPN3906 接收充电机中止充电报文超时 —— 对端若在，早就该发 CST 了 */
        case BMS_ERR_CHARGE_TIMEOUT:
            return BEM_ERR_CST;

        default:
            return 0u;   /* 总线类/多帧类错误不属于表 28 的任何一个字段 */
    }
}

static void EnterError(BMS_Error_t err, const char *note)
{
    s_error = err;
    BMS_LOG("\r\n*** [BMS] 告警: %s (%s) ***\r\n\r\n", BMS_ErrorStr(err), note);
    LED_R(ON);                 /* 红灯常亮表示故障 */

    /* 把错误类别映射成 BEM 的错误掩码，并复位发送计数（每次故障只连发一轮） */
    s_bem_mask    = BemMaskOf(err);
    s_bem_sent    = 0;
    s_next_bem_ms = s_tick_ms;

    /* 去哪一类状态：
     *   BUS_OFF            -> 故障态驻留，等总线恢复逻辑把它拉回来
     *   CCS_TIMEOUT        -> 故障态：标准要求"立即结束充电"并持续上报 BEM，
     *                         故障态里会按 250 ms 连发 BEM（BMS_ST_FAULT 分支）
     *   其它协议类超时      -> 回空闲态等 CHM 重新握手。
     * 注：**不要**在 CCS 超时时走 STOPPING —— 本端此刻已经收不到充电机的
     *     任何报文，发 BST/BSD 只是对空喊话；而且 STOPPING 里不会发 BEM。 */
    if (err == BMS_ERR_BUS_OFF || err == BMS_ERR_CCS_TIMEOUT)
    {
        SetState(BMS_ST_FAULT);
    }
    else
    {
        SetState(BMS_ST_IDLE);
    }
}

/**
  * @brief  处理一帧 CAN 报文
  */
void BMS_Protocol_OnFrame(const CAN_Frame_t *frame)
{
    uint32_t id;

    if (frame == NULL)
    {
        return;
    }

    id = frame->id;
    s_rx_count++;

    /* 所有来自充电机的报文都刷新一次接收时刻（用于超时判定） */
    if (id != GB_ID_TPCM_TO_BMS &&
        id != GB_ID_TPDT_TO_BMS)
    {
        s_last_rx_ms = s_tick_ms;
    }

    LogFrame("<-", id, frame->data, frame->len);

    switch (id)
    {
        /*---------------------------------------------------------------
         * CHM 充电机握手报文：空闲态收到后立即回复 BHM，进入握手阶段
         *-------------------------------------------------------------*/
        case GB_ID_CHM:
            if (s_state == BMS_ST_IDLE)
            {
                uint8_t d[8];
                uint16_t chm_ver;

                /* 标准表 8：CHM 的 B1-B3 是协议版本号，小端 24 位。
                 * V1.1 = byte3,byte2—0001H；byte1—01H ? 数据域 01 01 00，
                 * 按小端读回就是 0x000101。 */
                chm_ver = (uint16_t)((uint16_t)frame->data[CHM_OFF_VERSION]
                                   | ((uint16_t)frame->data[CHM_OFF_VERSION + 1] << 8));

                Build_BHM(d);
                CAN_SendFrame(GB_ID_BHM, d, BHM_LEN);   /* 标准表 9：2 字节 */
                s_tx_count++;
                LogFrame("->", GB_ID_BHM, d, BHM_LEN);

                BMS_LOG("[BMS] 收到 CHM 充电机握手(协议版本 V%u.%u), 已回复 BHM"
                        "(最高允许充电总电压 %u.%u V)\r\n",
                        (unsigned int)((chm_ver >> 8) & 0xFFu),
                        (unsigned int)(chm_ver & 0xFFu),
                        (unsigned int)(SIM_MAX_TOTAL_V_X10 / 10u),
                        (unsigned int)(SIM_MAX_TOTAL_V_X10 % 10u));

                s_session_id++;
                BMS_LOG("[BMS] 会话 #%lu 开始\r\n", (unsigned long)s_session_id);

                Sim_Reset();
                s_brm_sent = 0;
                s_bcp_sent = 0;
                s_cro_ready = 0;
                /* 新会话开始：清掉上一轮的 CCS 状态与 BEM 错误掩码，
                 * 否则 BEM 会把上一次会话的错误又报一遍。 */
                s_ccs_seen    = 0;
                s_ccs_permit  = 0;
                s_last_ccs_ms = s_tick_ms;
                s_bem_mask    = 0;
                s_bem_sent    = 0;
                TP_Reset();

                SetState(BMS_ST_HANDSHAKE);
            }
            break;

        /*---------------------------------------------------------------
         * CRM 充电机辨识报文：握手阶段收到后进入辨识阶段，开始发送 BRM
         *-------------------------------------------------------------*/
        /*---------------------------------------------------------------
         * CRM 充电机辨识报文（标准表 10：8 字节）
         *   B1    辨识结果 0x00 = BMS 不能辨识 / 0xAA = BMS 能辨识
         *   B2-B5 充电机编号（小端 32 位）
         *   B6-B8 区域编码（标准 ASCII，可选项）
         *-------------------------------------------------------------*/
        case GB_ID_CRM:
            if (s_state == BMS_ST_HANDSHAKE)
            {
                uint32_t charger_no = (uint32_t)frame->data[CRM_OFF_ID]
                                    | ((uint32_t)frame->data[CRM_OFF_ID + 1] << 8)
                                    | ((uint32_t)frame->data[CRM_OFF_ID + 2] << 16)
                                    | ((uint32_t)frame->data[CRM_OFF_ID + 3] << 24);

                BMS_LOG("[BMS] 收到 CRM 充电机辨识: 结果=0x%02X 编号=%lu 区域=[%c%c%c]\r\n",
                        (unsigned int)frame->data[CRM_OFF_RESULT],
                        (unsigned long)charger_no,
                        (char)frame->data[CRM_OFF_REGION + 0],
                        (char)frame->data[CRM_OFF_REGION + 1],
                        (char)frame->data[CRM_OFF_REGION + 2]);
                SetState(BMS_ST_IDENTIFY);
            }
            break;

        /*---------------------------------------------------------------
         * CML 充电机最大输出能力（标准表 14：8 字节）
         *   B1-B2 最高输出电压 0.1 V / B3-B4 最低输出电压 0.1 V
         *   B5-B6 最大输出电流 0.1 A / B7-B8 最小输出电流 0.1 A
         *   ★ 两个电流字段都带 **-400 A 偏移**，打印时要减掉才是真实值。
         *-------------------------------------------------------------*/
        case GB_ID_CML:
            if (frame->len >= CML_LEN)
            {
                BMS_LOG("[BMS] 收到 CML 充电机最大输出能力: 电压 ");
                print_x10((int32_t)le16_get(&frame->data[CML_OFF_MAX_V]));
                BMS_LOG(" ~ ");
                print_x10((int32_t)le16_get(&frame->data[CML_OFF_MIN_V]));
                BMS_LOG(" V, 电流 ");
                print_x10((int32_t)le16_get(&frame->data[CML_OFF_MAX_I]) - OFF_I_400_X10);
                BMS_LOG(" ~ ");
                print_x10((int32_t)le16_get(&frame->data[CML_OFF_MIN_I]) - OFF_I_400_X10);
                BMS_LOG(" A (已扣除 -400 A 偏移)\r\n");
            }
            break;

        /*---------------------------------------------------------------
         * CTS 充电机时间同步（标准表 13：7 字节）
         *   ★ 顺序是「秒、分、时、日、月、年」，**全部压缩 BCD 码**，
         *     年占 2 字节（B6 高位、B7 低位）。不是常见的年月日时分秒。
         *-------------------------------------------------------------*/
        case GB_ID_CTS:
            if (frame->len >= CTS_LEN)
            {
                uint8_t  sec = bcd2bin(frame->data[0]);
                uint8_t  min = bcd2bin(frame->data[1]);
                uint8_t  hrs = bcd2bin(frame->data[2]);
                uint8_t  day = bcd2bin(frame->data[3]);
                uint8_t  mon = bcd2bin(frame->data[4]);
                uint16_t yr  = (uint16_t)((uint16_t)bcd2bin(frame->data[5]) * 100u
                                        + (uint16_t)bcd2bin(frame->data[6]));

                BMS_LOG("[BMS] 收到 CTS 时间同步(压缩BCD): %04u-%02u-%02u %02u:%02u:%02u\r\n",
                        (unsigned int)yr, (unsigned int)mon, (unsigned int)day,
                        (unsigned int)hrs, (unsigned int)min, (unsigned int)sec);
            }
            break;

        /*---------------------------------------------------------------
         * CRO 充电机输出准备就绪：0xAA 表示 K1/K2 已闭合，可以充电
         *-------------------------------------------------------------*/
        case GB_ID_CRO:
            if (frame->len >= 1)
            {
                s_cro_ready = (frame->data[0] == GB_READY_READY) ? 1u : 0u;

                if (s_state == BMS_ST_CHARGING_READY && s_cro_ready)
                {
                    BMS_LOG("[BMS] 收到 CRO=0xAA 充电机输出准备就绪, 进入充电阶段\r\n");
                    s_next_bcl_ms = s_tick_ms;   /* 立即开始上报 */
                    s_next_bcs_ms = s_tick_ms;
                    s_next_bsm_ms = s_tick_ms;
                    SetState(BMS_ST_CHARGING);
                }
            }
            break;

        /*---------------------------------------------------------------
         * CST 充电机中止充电：立即进入结束阶段
         *-------------------------------------------------------------*/
        /*---------------------------------------------------------------
         * CCS 充电机充电状态（标准表 19：8 字节，周期 50 ms）
         *   B1-B2 SPN3081 电压输出值 0.1 V
         *   B3-B4 SPN3082 电流输出值 0.1 A（**-400 A 偏移**）
         *   B5-B6 SPN3083 累计充电时间 1 min
         *   B7.1  SPN3929 充电允许：00 暂停 / 01 允许
         * 【超时】本报文有逐报文规定：**1 s**（见充电态的判据）。
         * 这里只解析与记录，**不改变充电控制流程**（控制仍走 BCL/BCS/BSM），
         * 避免引入第二套并行控制逻辑。
         *-------------------------------------------------------------*/
        case GB_ID_CCS:
            if (frame->len >= CCS_LEN)
            {
                uint8_t permit = GB_FIELD2(frame->data[CCS_OFF_PERMIT], CCS_PERMIT_SHIFT);
                uint8_t first  = (uint8_t)(s_ccs_seen ? 0u : 1u);

                s_ccs_out_v_x10 = le16_get(&frame->data[CCS_OFF_OUT_V]);
                /* 电流字段带 -400 A 偏移，存之前先扣掉，方便打印与判定 */
                s_ccs_out_i_x10 = (uint16_t)((int32_t)le16_get(&frame->data[CCS_OFF_OUT_I])
                                             - OFF_I_400_X10);
                s_ccs_minutes   = le16_get(&frame->data[CCS_OFF_MINUTES]);
                /* 【只在状态变化时打印】CCS 是 50 ms 周期报文，每帧都打会把
                 * 串口刷满。标准表 19 表下注：SPN3929=0 表示充电机将停止输出、
                 * =1 表示将继续开始充电 —— 这个状态变化值得留一条记录，
                 * 否则充电机那边暂停了输出，BMS 侧完全无感。
                 * 注意仍**不改变控制流程**（控制走 BCL/BCS/BSM）。 */
                if (permit != s_ccs_permit)
                {
                    BMS_LOG("[BMS] 充电机%s输出（CCS SPN3929=%u）\r\n",
                            (permit != 0u) ? "允许" : "暂停", (unsigned int)permit);
                    s_ccs_permit = permit;
                }
                s_last_ccs_ms   = s_tick_ms;   /* CCS 专用 1 s 超时的基准 */
                s_ccs_seen      = 1;

                /* 只在第一次收到时打印一行，避免 50 ms 周期的报文把串口刷满 */
                if (first)
                {
                    BMS_LOG("[BMS] 收到 CCS 充电机充电状态: 输出 %d.%d V / %d.%d A, "
                            "累计 %u min, 充电允许=%s(SPN3929=%u)\r\n",
                            (int)(s_ccs_out_v_x10 / 10u), (int)(s_ccs_out_v_x10 % 10u),
                            (int)(s_ccs_out_i_x10 / 10u), (int)(s_ccs_out_i_x10 % 10u),
                            (unsigned int)s_ccs_minutes,
                            (permit == CCS_OUTPUT_ALLOW) ? "允许" : "暂停",
                            (unsigned int)permit);
                }
            }
            break;

        /*---------------------------------------------------------------
         * CST 充电机中止充电（标准表 25：4 字节，逐字节是 2 位字段）
         *   B1    SPN3521 中止原因（4 个 2 位）
         *   B2-B3 SPN3522 故障原因（6 个 2 位 + 未定义位）
         *   B4    SPN3523 错误原因（2 个 2 位）
         *-------------------------------------------------------------*/
        case GB_ID_CST:
            BMS_LOG("[BMS] 收到 CST 充电机中止充电: B1 原因=0x%02X "
                    "(充电机设定条件=%u 人工中止=%u 故障中止=%u BMS主动中止=%u) "
                    "B4 错误=0x%02X\r\n",
                    (unsigned int)((frame->len > CST_OFF_REASON) ? frame->data[CST_OFF_REASON] : 0u),
                    (unsigned int)GB_FIELD2(((frame->len > CST_OFF_REASON) ? frame->data[CST_OFF_REASON] : 0u), CST_R_CHARGER_COND_SHIFT),
                    (unsigned int)GB_FIELD2(((frame->len > CST_OFF_REASON) ? frame->data[CST_OFF_REASON] : 0u), CST_R_MANUAL_SHIFT),
                    (unsigned int)GB_FIELD2(((frame->len > CST_OFF_REASON) ? frame->data[CST_OFF_REASON] : 0u), CST_R_FAULT_SHIFT),
                    (unsigned int)GB_FIELD2(((frame->len > CST_OFF_REASON) ? frame->data[CST_OFF_REASON] : 0u), CST_R_BMS_SHIFT),
                    (unsigned int)((frame->len > CST_OFF_ERROR) ? frame->data[CST_OFF_ERROR] : 0u));
            if (s_state == BMS_ST_CHARGING || s_state == BMS_ST_CHARGING_READY)
            {
                s_error = BMS_ERR_CHARGER_ABORT;
                /* BST 的原因字节：1-2 位"达到所需 SOC 目标值"= 01 → 0x40。
                 * （本端是被充电机中止的，按标准填"充电机主动中止"= 01 也行，
                 *  这里保留原来的语义：记录"充电机侧中止"这件事。） */
                s_stop_reason = GB_FIELD2_SET(0u, BST_R_CHARGER_STOP_SHIFT, BSM_ST_HIGH);
                SetState(BMS_ST_STOPPING);
            }
            break;

        /*---------------------------------------------------------------
         * TP.CM / TP.DT：J1939 多帧传输的流控与应答
         *-------------------------------------------------------------*/
        case GB_ID_TPCM_TO_BMS:
            TP_OnCm(frame);
            break;

        case GB_ID_TPDT_TO_BMS:
            /* 本工程充电机侧不向 BMS 发送长报文，此处仅记录 */
            break;

        default:
            break;
    }
}

/*==============================================================================
 * §9  状态机与周期任务
 *============================================================================*/

/** 发送一帧数据并统计 */
static void SendOne(uint32_t id, const uint8_t *d, uint8_t len)
{
    if (CAN_SendFrame(id, d, len) != 0)
    {
        s_tx_count++;
    }
}

/**
  * @brief  发一帧「周期遥测」——帧照发，但**不计数、不打印**
  *
  * 待机时 BMS 每秒广播一次电池状态（还要发心跳）。如果照常统计和打印：
  *   · 两块板子的串口每秒被刷好几行，真正有用的日志全被冲走；
  *   · LCD 上的"发送帧"开机没多久就涨到几千，完全看不出**这一次充电**
  *     到底交互了多少报文。
  *
  * 所以周期遥测单独走这条静默通道：帧照样发出去（i.MX 要靠它显示
  * 待机时的实时电量），但不进统计、不上日志。
  * 充电会话里的报文仍然走 SendOne / LogFrame，该统计的照统计。
  */
static void SendOneQuiet(uint32_t id, const uint8_t *d, uint8_t len)
{
    (void)CAN_SendFrame(id, d, len);
}

/** 状态机主逻辑 */
static void StateMachine(void)
{
    uint32_t now      = s_tick_ms;
    uint32_t in_state = now - s_state_ms;
    uint8_t  d[BRM_LEN];
    uint8_t  small[8];

    switch (s_state)
    {
        /*--------------------------------------------------------------
         * 空闲态：等待 CHM（本工程不做 BMS 主动上线，完全由充电机触发）
         *------------------------------------------------------------*/
        case BMS_ST_IDLE:
            /* 进空闲态时把"充电阶段才发"的周期计时器清零，好让下次会话立即开始上报。
             *
             * 【千万注意】这里**不能**再重置 s_next_bsm_ms！
             *
             * StateMachine() 是在主循环里被高频调用的（实测约 1400 次/秒），
             * 而这几个赋值每一拍都会执行。BSM 现在是"不分状态的全局广播"，
             * 如果这里把它重置成 now，BMS_Protocol_Tick() 里的
             *     if (s_tick_ms >= s_next_bsm_ms)   ->  now >= now 恒成立
             * 就会让 BSM **每一拍都发**，直接把总线灌满
             * （实测 181756F4 —— 这是当时的 BSM ID，本轮按标准表 5 已改为 181356F4 ——
             *   以 ~1400 帧/秒刷屏，充电机侧连 CST 都插不进去，
             *   表现为"按了停止没反应、STM32 继续充电"）。
             * 这个坑踩过一次，务必记住。 */
            s_next_bhm_ms = now;
            s_next_bro_ms = now;
            s_next_bcl_ms = now;
            s_next_bcs_ms = now;

#if (BMS_HEARTBEAT_ENABLE)
            /* 空闲态周期发心跳：让对端 candump 能确认本方向是否通。
             * 收到 CHM 进入握手态后本分支不再执行，心跳自然停止。 */
            if (now >= s_next_hb_ms)
            {
                uint8_t hb[8];

                GB_SCHED_NEXT(s_next_hb_ms, now, BMS_HEARTBEAT_PERIOD);
                s_hb_count++;
                hb[0] = 0xA5;
                hb[1] = (uint8_t)(s_hb_count & 0xFFu);
                hb[2] = 0x5A;
                hb[3] = (uint8_t)(CAN_GetRxQueueCount() & 0xFFu);
                hb[4] = 0x00;
                hb[5] = 0x00;
                hb[6] = 0x00;
                hb[7] = 0x00;
                /* 心跳只是"我在线"的提示，走静默通道：
                 * 照样发，但不进统计、不上日志 —— 否则待机时串口每秒多一行。 */
                SendOneQuiet(GB_ID_BMS_HEARTBEAT, hb, 8);
            }
#endif
            break;

        /*--------------------------------------------------------------
         * 握手态：周期重发 BHM（250 ms），等待 CRM
         *------------------------------------------------------------*/
        case BMS_ST_HANDSHAKE:
            if (now >= s_next_bhm_ms)
            {
                GB_SCHED_NEXT(s_next_bhm_ms, now, GB_T_BHM_PERIOD);
                Build_BHM(d);
                SendOne(GB_ID_BHM, d, BHM_LEN);   /* 标准表 3：2 字节 */
            }
            if (in_state > GB_T_HANDSHAKE_TIMEOUT)
            {
                EnterError(BMS_ERR_CRM_TIMEOUT, "5 秒内未收到 CRM");
            }
            break;

        /*--------------------------------------------------------------
         * 辨识态：发送 BRM（41 字节，J1939 多帧），等待 CTS/CML
         *------------------------------------------------------------*/
        case BMS_ST_IDENTIFY:
            if (!s_brm_sent)
            {
                Build_BRM(s_brm_buf);
                TP_Start(GB_PGN_BRM, s_brm_buf, BRM_LEN);
                s_brm_sent = 1;
                s_state_ms = now;   /* 从真正开始发送时起算超时 */
            }

            if (s_tp.state == TP_DONE)
            {
                TP_Reset();
                s_bcp_sent = 0;
                BMS_LOG("[BMS] BRM 发送完成, 进入参数配置阶段\r\n");
                SetState(BMS_ST_PARAM_CONFIG);
                break;
            }
            if (s_tp.state == TP_ABORT)
            {
                TP_Reset();
                EnterError(BMS_ERR_TP_FAIL, "BRM 多帧传输失败");
                break;
            }
            if (in_state > GB_T_IDENTIFY_TIMEOUT)
            {
                EnterError(BMS_ERR_CML_TIMEOUT, "5 秒内未收到 CML/CTS");
            }
            break;

        /*--------------------------------------------------------------
         * 参数配置态：发送 BCP（13 字节，J1939 多帧），等待 CRO
         *------------------------------------------------------------*/
        case BMS_ST_PARAM_CONFIG:
            if (!s_bcp_sent)
            {
                Build_BCP(s_bcp_buf, s_soc_x10, s_voltage_x10);
                TP_Start(GB_PGN_BCP, s_bcp_buf, BCP_LEN);
                s_bcp_sent = 1;
                s_state_ms = now;
            }

            if (s_tp.state == TP_DONE)
            {
                TP_Reset();
                BMS_LOG("[BMS] BCP 发送完成, 等待充电机准备就绪\r\n");
                SetState(BMS_ST_CHARGING_READY);
                break;
            }
            if (s_tp.state == TP_ABORT)
            {
                TP_Reset();
                EnterError(BMS_ERR_TP_FAIL, "BCP 多帧传输失败");
                break;
            }
            if (in_state > GB_T_PARAM_TIMEOUT)
            {
                EnterError(BMS_ERR_CRO_TIMEOUT, "5 秒内未收到 CRO");
            }
            break;

        /*--------------------------------------------------------------
         * 充电准备态：周期发送 BRO（250 ms），等待 CRO=0xAA
         *------------------------------------------------------------*/
        case BMS_ST_CHARGING_READY:
            if (now >= s_next_bro_ms)
            {
                GB_SCHED_NEXT(s_next_bro_ms, now, GB_T_BHM_PERIOD);
                /* 标准表 4：BRO 数据域 1 字节，全部是有效字段 */
                small[0] = GB_READY_READY;      /* BMS 侧已准备就绪 */
                SendOne(GB_ID_BRO, small, 1);
            }
            if (s_cro_ready)
            {
                BMS_LOG("[BMS] 充电机已就绪, 进入充电阶段\r\n");
                SetState(BMS_ST_CHARGING);
            }
            if (in_state > GB_T_PARAM_TIMEOUT)
            {
                EnterError(BMS_ERR_CRO_TIMEOUT, "5 秒内未收到 CRO=0xAA");
            }
            break;

        /*--------------------------------------------------------------
         * 充电态：周期发送 BCL / BCS / BSM
         * 超时：5 秒内没有收到任何充电机报文 -> 中止
         *------------------------------------------------------------*/
        case BMS_ST_CHARGING:
            /* --- BCL 电池充电需求 --- */
            if (now >= s_next_bcl_ms)
            {
                int32_t demand_v;

                GB_SCHED_NEXT(s_next_bcl_ms, now, GB_T_BCL_PERIOD);

                demand_v = (int32_t)s_voltage_x10 + 20;    /* 需求电压 = 实测 + 2.0 V */
                if (demand_v > (int32_t)SIM_MAX_TOTAL_V_X10)
                {
                    demand_v = (int32_t)SIM_MAX_TOTAL_V_X10;
                }

                /* 标准 7.9：先把整段填 1，再写有效字段（5 字节全部有效）。
                 * ★ 标准表 17 的 BCL **只有这 3 个字段**，没有"允许充电电压/
                 *   电流"（那两个量在 BCP 的 SPN2819 / SPN2817 里）。 */
                fill_reserved(small, sizeof(small));
                /* B1-B2 电压需求 0.1 V */
                le16_put(&small[BCL_OFF_V_DEMAND], (uint16_t)demand_v);
                /* B3-B4 电流需求 0.1 A, -400 A 偏移 */
                le16_put(&small[BCL_OFF_I_DEMAND], (uint16_t)(s_current_x10 + OFF_I_400_X10));
                /* B5 充电模式：0x01 = 恒压，0x02 = 恒流（标准表 17） */
                small[BCL_OFF_MODE] = (s_soc_x10 >= BMS_CV_SOC_X10) ? 0x01 : 0x02;

                SendOne(GB_ID_BCL, small, BCL_LEN);
                LogFrame("->", GB_ID_BCL, small, BCL_LEN);
            }

            /* --- BCS 电池充电总状态 --- */
            if (now >= s_next_bcs_ms)
            {
                GB_SCHED_NEXT(s_next_bcs_ms, now, GB_T_BCS_PERIOD);

                Build_BCS(small, s_voltage_x10, s_current_x10,
                          Sim_CellMaxVoltage_X100(), s_cell_max_no, s_soc_x10);

                SendOne(GB_ID_BCS, small, BCS_LEN);
                LogFrame("->", GB_ID_BCS, small, BCS_LEN);
            }

            /* BSM 已改为全局周期广播（见 SendBsmTelemetry + BMS_Protocol_Tick），
             * 这里不再单独发送 —— 待机 / 充满 / 掉电阶段同样需要让 i.MX
             * 看到实时电量。 */

            /* --- 充电阶段超时判定（两条，都要挂 BEM 错误上报） ---
             * 标准第 8 章：通用超时除特殊规定外均为 5 s。
             * 但 CCS 有**逐报文**的特殊规定，必须单独判：
             *   标准表 19 下注：「如果 BMS 在 **1 s** 内没有收到该报文，
             *   即为超时错误，BMS 应立即结束充电。」
             * （BCL 的 1 s 超时是**充电机侧**的判据，不在这里。）
             * BEM 的错误掩码由 EnterError -> BemMaskOf() 统一映射，
             * 这里不用手工设置。 */
            if ((now - s_last_ccs_ms) > GB_T_CCS_TIMEOUT)
            {
                s_stop_reason = GB_FIELD2_SET(0u, BST_R_CHARGER_STOP_SHIFT, BSM_ST_HIGH);
                EnterError(BMS_ERR_CCS_TIMEOUT,
                           "1 秒内未收到 CCS 充电机充电状态(标准规定应立即结束充电)");
                break;
            }

            if ((now - s_last_rx_ms) > GB_T_CHARGE_TIMEOUT)
            {
                /* BST 的 B1：7-8 位"充电机主动中止"= 01 → 0x01
                 * （本端是被对端停掉的） */
                s_stop_reason = GB_FIELD2_SET(0u, BST_R_CHARGER_STOP_SHIFT, BSM_ST_HIGH);
                EnterError(BMS_ERR_CHARGE_TIMEOUT, "5 秒内未收到充电机报文");
            }

            /* --- 充满自动结束 --- */
            if (s_phase == SIM_PHASE_FULL || s_soc_x10 >= 1000u)
            {
                BMS_LOG("[BMS] SOC 已达 100%%, 自动结束充电\r\n");
                /* BST 的 B1：1-2 位"达到所需 SOC 目标值"= 01 → 0x40 */
                s_stop_reason = GB_FIELD2_SET(0u, BST_R_SOC_SHIFT, BSM_ST_HIGH);
                SetState(BMS_ST_STOPPING);
            }
            break;

        /*--------------------------------------------------------------
         * 结束态：发送 BST 与 BSD 统计报文，3 秒后回空闲
         *------------------------------------------------------------*/
        case BMS_ST_STOPPING:
            /* BST：10 ms 一次，只发 BMS_BST_TIMES 次。
             * 原来的时间窗判据会在窗内连续发上千帧 —— 见上面 s_bst_sent 的说明。 */
            if (s_bst_sent < BMS_BST_TIMES && now >= s_next_bst_ms)
            {
                GB_SCHED_NEXT(s_next_bst_ms, now, GB_T_BST_PERIOD);
                s_bst_sent++;

                /* BST（标准表 24：4 字节，逐字节是 2 位字段）
                 *   B1    SPN3511 中止原因
                 *   B2-B3 SPN3512 故障原因（本端不做 BMS 侧故障检测，
                 *         6 个字段如实填 **10 不可信**，未定义位按 7.9 填 1）
                 *   B4    SPN3523 错误原因（电压/电流均正常 = 00） */
                fill_reserved(small, sizeof(small));
                small[BST_OFF_REASON] = s_stop_reason;
                small[BST_OFF_FAULT_LO] = BST_FAULT_UNRELIABLE_HI;
                small[BST_OFF_FAULT_HI] = BST_FAULT_UNRELIABLE_LO;
                small[BST_OFF_ERROR] = GB_FIELD2_SET(
                    GB_FIELD2_SET(0u, BST_E_VOLTAGE_SHIFT, BSM_ST_NORMAL),
                    BST_E_CURRENT_SHIFT, BSM_ST_NORMAL);
                SendOne(GB_ID_BST, small, BST_LEN);
                LogFrame("->", GB_ID_BST, small, BST_LEN);
            }

            /* BSD：只发一遍 */
            if (!s_bsd_sent && in_state > 500u)
            {
                uint16_t cell_x100 = Sim_CellMaxVoltage_X100();

                s_bsd_sent = 1;

                /* BSD（标准表 26：7 字节）
                 *   B1 中止 SOC / B2-B3 单体最低压 / B4-B5 单体最高压 /
                 *   B6 最低温 / B7 最高温
                 * 本工程只建模了一路单体电压，最低压按最高压减 40 mV 上报
                 * （**属估算，不是实测**，如需真实最低单体电压请提供数据源）。 */
                Build_BSD(small, (uint8_t)(s_soc_x10 / 10u),
                          (uint16_t)((cell_x100 > 4u) ? (cell_x100 - 4u) : 0u),
                          cell_x100,
                          s_temp_min_c, s_temp_max_c);

                SendOne(GB_ID_BSD, small, BSD_LEN);
                LogFrame("->", GB_ID_BSD, small, BSD_LEN);

                BMS_LOG("[BMS] 本次会话 #%lu 结束: BST 原因字节=0x%02X 充电 %lu 秒, 累计电量 ",
                        (unsigned long)s_session_id,
                        (unsigned int)s_stop_reason,
                        (unsigned long)s_charge_seconds);
                print_x10((int32_t)s_charge_energy_x10);
                BMS_LOG(" kWh, SOC=%u%%\r\n", (unsigned int)(s_soc_x10 / 10u));
            }

            if (in_state > 3000u)
            {
                LED_R(OFF);
                s_error = BMS_ERR_NONE;
                TP_Reset();
                BMS_LOG("[BMS] 回到空闲态, 等待下一次 CHM 握手\r\n");
                SetState(BMS_ST_IDLE);
            }
            break;

        /*--------------------------------------------------------------
         * 故障态：周期发送 BEM 错误报文，10 秒后自动复位（实际产品需人工确认）
         *
         * 【为什么把 BEM 放在这里】标准表 28 原文：「当 BMS 检测到错误时，
         *   发送给充电机充电错误报文，直到 BMS 收到充电机发送的充电机辨识
         *   报文(CRM)或拔掉充电插头为止。」
         * 本端把"检测到错误"落在**已有的超时分支**上（它们会设置 s_bem_mask），
         * 故障态里按 250 ms 周期把 BEM 发出去，最多 GB_T_BEM_MAX_TX 次。
         * 转出故障态（人工复位 / 自动复位 / 收到 CHM 重新握手）后自然停止，
         * 不新建任何并行机制。
         *------------------------------------------------------------*/
        case BMS_ST_FAULT:
            if (s_bem_mask != 0u && s_bem_sent < GB_T_BEM_MAX_TX && now >= s_next_bem_ms)
            {
                GB_SCHED_NEXT(s_next_bem_ms, now, GB_T_BEM_PERIOD);
                s_bem_sent++;

                Build_BEM(small, s_bem_mask);
                SendOne(GB_ID_BEM, small, BEM_LEN);
                LogFrame("->", GB_ID_BEM, small, BEM_LEN);

                if (s_bem_sent == 1u)
                {
                    BMS_LOG("[BMS] 进入故障态, 开始发送 BEM 错误报文"
                            "(ID 0x%08lX, 优先权2, 错误掩码=0x%02X)\r\n",
                            (unsigned long)GB_ID_BEM, (unsigned int)s_bem_mask);
                }
            }

            if (in_state > 10000u)
            {
                BMS_Protocol_Reset();
            }
            break;

        default:
            BMS_Protocol_Reset();
            break;
    }
}

/*==============================================================================
 * §10  对外接口
 *============================================================================*/

void BMS_Protocol_Init(void)
{
    /* 1. 使用 SysTick 建立 1 ms 时基（SystemCoreClock 由 system_stm32f10x.c 设定） */
    if (SysTick_Config(SystemCoreClock / 1000u) != 0)
    {
        /* 配置失败：SystemCoreClock 过大或中断优先级设置异常 */
        while (1)
        {
        }
    }

    /* 2. 复位内部状态 */
    memset(s_brm_buf, 0, sizeof(s_brm_buf));
    memset(s_bcp_buf, 0, sizeof(s_bcp_buf));

    TP_Reset();
    Sim_Reset();

    /* ---- 掉电记忆：把上次的电量读回来 ----
     * 顺序很重要：Sim_Reset() 只清会话统计、不动 SOC，
     * 所以先把 SOC 置成出厂初值，再由 Sim_FlashLoad() 用 Flash 里的值覆盖它。
     * Flash 里没有有效记录（第一次上电）时就保持出厂初值。 */
    s_soc_x10     = SIM_START_SOC_X10;
    s_voltage_x10 = SIM_START_V_X10;
    s_temp_max_c  = SIM_TEMP_BASE_C;
    s_temp_min_c  = SIM_TEMP_BASE_C - 6;
    s_cell_max_no = 1;
    s_phase       = SIM_PHASE_STANDBY;
    Sim_FlashLoad();

    s_tick_ms     = 0;
    s_state       = BMS_ST_IDLE;
    s_error       = BMS_ERR_NONE;
    s_state_ms    = 0;
    s_last_rx_ms  = 0;
    s_rx_count    = 0;
    s_tx_count    = 0;
    s_brm_sent    = 0;
    s_bcp_sent    = 0;
    s_cro_ready   = 0;
    s_session_id  = 0;
    /* CCS / BEM 的新增状态 */
    s_last_ccs_ms = 0;
    s_ccs_seen    = 0;
    s_ccs_permit  = 0;
    s_bem_mask    = 0;
    s_bem_sent    = 0;
    s_next_bem_ms = 0;

    BMS_LOG("\r\n");
    BMS_LOG("========================================================\r\n");
    BMS_LOG(" GB/T 27930-2015  BMS 报文模拟器 (STM32F103ZET6)\r\n");
    BMS_LOG(" 充电机地址 0x%02X   BMS 地址 0x%02X\r\n",
            (unsigned int)GB_ADDR_CHARGER, (unsigned int)GB_ADDR_BMS);
    BMS_LOG(" 电池: %s %s  %d.%d V / %d.%d Ah  %d 串\r\n",
            BMS_MANUFACTURER, BMS_MODEL,
            (int)(SIM_RATED_V_X10 / 10), (int)(SIM_RATED_V_X10 % 10),
            (int)(SIM_RATED_CAP_X10 / 10), (int)(SIM_RATED_CAP_X10 % 10),
            (int)SIM_CELL_COUNT);
    BMS_LOG(" 初始 SOC %d.%d%%   初始总压 %d.%d V\r\n",
            (int)(SIM_START_SOC_X10 / 10), (int)(SIM_START_SOC_X10 % 10),
            (int)(SIM_START_V_X10 / 10), (int)(SIM_START_V_X10 % 10));
    BMS_LOG(" 当前状态: %s\r\n", BMS_StateStr(s_state));
    BMS_LOG("========================================================\r\n\r\n");
}

void BMS_SysTick_Handler(void)
{
    s_tick_ms++;
}

/**
  * @brief  周期广播动力蓄电池状态（BSM）
  *
  * 为什么放在这里、而且要**不分状态**地发：
  *   i.MX 相当于车机上的电池监控软件，它必须随时能看到电量 ——
  *   包括待机、充满、掉电这几个阶段。国标里 BSM 只在充电阶段发，
  *   但本工程的 STM32 同时充当"电池遥测源"，所以放宽成始终广播。
  *
  * 周期按国标取 GB_T_BSM_PERIOD（250 ms），待机时也保持这个周期
  * （4 帧/秒、总线负载约 0.24%，可以忽略；而且待机帧不入库）。
  */
static void SendBsmTelemetry(void)
{
    uint8_t buf[BSM_LEN];
    uint8_t charge_ok;

    /* 充电允许（SPN3096）：只有"充电机已就绪、本端也确实在充电流程里"才允许。
     *   已就绪(CRO=0xAA) 且 状态在充电中 / 充电准备 → 01 允许
     *   其余（待机 / 结束 / 故障 / 只发了 BRO 还没等到 CRO）→ 00 禁止
     * 这是**真实状态判断**，不是为了让对端好看而写死的常量。 */
    charge_ok = (s_cro_ready != 0u &&
                 (s_state == BMS_ST_CHARGING || s_state == BMS_ST_CHARGING_READY))
                ? BSM_CHARGE_ALLOWED : BSM_CHARGE_FORBIDDEN;

    /* 标准表 20：BSM 数据域 7 字节，字段与状态位全部在 Build_BSM 里处理，
     * 这里只负责"发"与"用什么温度检测点编号"。
     * 本工程只建模了一路温度，最高/最低检测点编号按 1 基的固定位置上报
     * （1 偏移：0 在标准里不是合法编号，Build_BSM 内已兜底成 1）。 */
    Build_BSM(buf, s_cell_max_no,
              s_temp_max_c, 7,          /* 最高温度检测点编号 */
              s_temp_min_c, 2,          /* 最低温度检测点编号 */
              charge_ok);

    /* 充电会话中：BSM 是国标规定的周期报文，正常统计 + 打印。
     * 待机 / 充满 / 掉电：它只是给 i.MX 看的电量遥测，静默发出去就行。 */
    if (s_state == BMS_ST_CHARGING || s_state == BMS_ST_CHARGING_READY)
    {
        SendOne(GB_ID_BSM, buf, BSM_LEN);
        LogFrame("->", GB_ID_BSM, buf, BSM_LEN);
    }
    else
    {
        SendOneQuiet(GB_ID_BSM, buf, BSM_LEN);
    }
}

void BMS_Protocol_Tick(void)
{
    /* 1. J1939 多帧传输流控推进（优先级最高，避免对端流控超时） */
    TP_Poll();

    /* 2. 状态机推进 */
    StateMachine();

    /* 3. 电池模型仿真 */
    if ((s_tick_ms - s_next_sim_ms) >= GB_T_SIM_PERIOD)
    {
        s_next_sim_ms = s_tick_ms;
        Sim_Step();

        /* 指示灯：充电中绿灯常亮，其它状态熄灭 */
        if (s_state == BMS_ST_CHARGING)
        {
            LED_G(ON);
        }
        else
        {
            LED_G(OFF);
        }
    }

    /* 3.5 电池状态广播：不分状态，周期由 GB_T_BSM_PERIOD 决定（国标 250 ms）。
     *     这样 i.MX 在待机 / 充满 / 掉电阶段也能一直看到实时 SOC。
     *     早先这里写的是"固定 1 秒一次"，那是周期放宽时期的说法，已随
     *     周期对齐国标一并改掉。 */
    if (s_tick_ms >= s_next_bsm_ms)
    {
        GB_SCHED_NEXT(s_next_bsm_ms, s_tick_ms, GB_T_BSM_PERIOD);
        SendBsmTelemetry();
    }

    /* 4. CAN 总线异常检测与自动恢复
     *    (a) Bus-Off：控制器已被硬件关闭，立即重新初始化；
     *    (b) 错误被动（TEC >= 128）持续 5 秒：说明本节点一直在发，
     *        却始终收不到 ACK。此时三个发送邮箱会被「等待重传」的帧占满
     *        （现象：心跳计数在涨，但「CAN 发 N 帧」不再增加，
     *         并持续打印 [发送失败]），之后就再也发不出任何帧。
     *        重新初始化控制器可以把邮箱释放掉，让节点重新上线。
     *        好处是：现场把线接好之后，STM32 会在 5 秒内自动恢复通信，
     *        不需要按复位键。 */
    if (CAN_IsBusOff())
    {
        if (s_state != BMS_ST_FAULT)
        {
            /* 走 EnterError 而不是直接给 s_error 赋值 —— 这样才会真正切到
             * BMS_ST_FAULT，让"故障态 10 秒后自动复位重试"那条路径生效。 */
            EnterError(BMS_ERR_BUS_OFF, "CAN 总线 Bus-Off, 重新初始化控制器");
        }
        CAN_RecoverBusOff();
        s_passive_since_ms = 0;
        return;
    }

    {
        uint8_t tec = 0;
        uint8_t rec = 0;
        uint8_t lec = 0;

        CAN_GetBusCounters(&tec, &rec, &lec);

        if (tec >= 128u)
        {
            if (s_passive_since_ms == 0u)
            {
                s_passive_since_ms = s_tick_ms;
                BMS_LOG("\r\n*** [BMS] CAN 进入错误被动 TEC=%u LEC=%u(%s): "
                        "本节点在发但收不到任何 ACK, 请检查接线/对端是否在线 ***\r\n\r\n",
                        (unsigned int)tec, (unsigned int)lec, LecStr(lec));
            }
            else if ((s_tick_ms - s_passive_since_ms) > 5000u)
            {
                BMS_LOG("[BMS] 错误被动已持续 5 秒, 重新初始化 CAN 控制器以释放发送邮箱\r\n");
                CAN_RecoverBusOff();
                s_passive_since_ms = 0;
            }
        }
        else
        {
            s_passive_since_ms = 0;
        }
    }

    /* 5. 每秒打印一次状态摘要 */
    if ((s_tick_ms - s_next_log_ms) >= 1000u)
    {
        s_next_log_ms = s_tick_ms;
        s_need_status_log = 1;
    }
}

void BMS_Protocol_Reset(void)
{
    TP_Reset();
    Sim_Reset();

    /* ---- 掉电记忆：把上次的电量读回来 ----
     * 顺序很重要：Sim_Reset() 只清会话统计、不动 SOC，
     * 所以先把 SOC 置成出厂初值，再由 Sim_FlashLoad() 用 Flash 里的值覆盖它。
     * Flash 里没有有效记录（第一次上电）时就保持出厂初值。 */
    s_soc_x10     = SIM_START_SOC_X10;
    s_voltage_x10 = SIM_START_V_X10;
    s_temp_max_c  = SIM_TEMP_BASE_C;
    s_temp_min_c  = SIM_TEMP_BASE_C - 6;
    s_cell_max_no = 1;
    s_phase       = SIM_PHASE_STANDBY;
    Sim_FlashLoad();

    s_state       = BMS_ST_IDLE;
    s_error       = BMS_ERR_NONE;
    s_state_ms    = s_tick_ms;
    s_last_rx_ms  = s_tick_ms;
    s_brm_sent    = 0;
    s_bcp_sent    = 0;
    s_cro_ready   = 0;
    s_next_bhm_ms = s_tick_ms;
    s_next_bro_ms = s_tick_ms;
    /* CCS / BEM 的新增状态：复位时一并清掉，避免把上一轮的错误再报一遍 */
    s_last_ccs_ms = s_tick_ms;
    s_ccs_seen    = 0;
    s_ccs_permit  = 0;
    s_bem_mask    = 0;
    s_bem_sent    = 0;
    s_next_bem_ms = s_tick_ms;

    LED_R(OFF);
    LED_G(OFF);

    BMS_LOG("[BMS] 协议层复位, 回到空闲态\r\n");
}

/**
  * @brief  取一份 UI 显示快照
  *
  *   界面只要这一份数据就能画出整屏，不必知道协议层的任何内部细节。
  *   这里刻意只做「值拷贝」：不调用任何发送函数、不改任何状态，
  *   因此在主循环里以任意频率调用都是安全的。
  */
void BMS_Protocol_GetUiSnapshot(BMS_UiSnapshot_t *out)
{
    if (out == 0)
    {
        return;
    }

    out->state          = s_state;
    out->error          = s_error;
    out->session_id     = s_session_id;

    out->soc_x10        = s_soc_x10;
    out->phase          = (uint8_t)s_phase;
    out->voltage_x10    = s_voltage_x10;
    out->current_x10    = s_current_x10;
    out->cell_max_mv    = (uint16_t)(Sim_CellMaxVoltage_X100() * 10u);
    out->cell_max_no    = s_cell_max_no;
    out->temp_max_c     = s_temp_max_c;
    out->temp_min_c     = s_temp_min_c;

    out->limit_v_x10    = (uint16_t)SIM_MAX_TOTAL_V_X10;
    out->limit_i_x10    = (uint16_t)SIM_MAX_CURRENT_X10;

    out->charge_seconds = s_charge_seconds;
    out->energy_x10     = s_charge_energy_x10;

    out->charge_mode    = (uint8_t)((s_soc_x10 >= BMS_CV_SOC_X10) ? 1u : 2u);
    out->cro_ready      = s_cro_ready;
    out->rx_count       = s_rx_count;
    out->tx_count       = s_tx_count;
}

const char *BMS_StateStr(BMS_State_t st)
{
    switch (st)
    {
        case BMS_ST_IDLE:           return "空闲(IDLE)";
        case BMS_ST_HANDSHAKE:      return "握手(HANDSHAKE)";
        case BMS_ST_IDENTIFY:       return "辨识(IDENTIFY)";
        case BMS_ST_PARAM_CONFIG:   return "参数配置(PARAM_CONFIG)";
        case BMS_ST_CHARGING_READY: return "充电准备(READY)";
        case BMS_ST_CHARGING:       return "充电中(CHARGING)";
        case BMS_ST_STOPPING:       return "结束(STOPPING)";
        case BMS_ST_FAULT:          return "故障(FAULT)";
        default:                    return "未知";
    }
}

const char *BMS_ErrorStr(BMS_Error_t err)
{
    switch (err)
    {
        case BMS_ERR_NONE:             return "无";
        case BMS_ERR_CRM_TIMEOUT:      return "等待 CRM 超时";
        case BMS_ERR_CML_TIMEOUT:      return "等待 CML 超时";
        case BMS_ERR_CRO_TIMEOUT:      return "等待 CRO 超时";
        case BMS_ERR_CHARGE_TIMEOUT:   return "充电阶段报文超时";
        case BMS_ERR_CCS_TIMEOUT:      return "CCS 超时(标准 1 s)";
        case BMS_ERR_TP_FAIL:          return "J1939 多帧传输失败";
        case BMS_ERR_CHARGER_ABORT:    return "充电机主动中止";
        case BMS_ERR_BUS_OFF:          return "CAN 总线 Bus-Off";
        default:                       return "未知";
    }
}

void BMS_Protocol_PrintStatus(void)
{
    uint32_t elapsed;

    if (!s_need_status_log)
    {
        return;
    }
    s_need_status_log = 0;

    elapsed = (s_tick_ms - s_state_ms) / 1000u;

    printf("[%6lu s] 状态=%-24s 驻留=%3lu s  错误=%-20s\r\n",
           (unsigned long)(s_tick_ms / 1000u),
           BMS_StateStr(s_state),
           (unsigned long)elapsed,
           BMS_ErrorStr(s_error));

    printf("         电压=");
    print_x10((int32_t)s_voltage_x10);
    printf(" V  电流=");
    print_x10((int32_t)s_current_x10);
    printf(" A  SOC=");
    print_x10((int32_t)s_soc_x10);
    printf(" %%  单体=");
    print_x100((int32_t)Sim_CellMaxVoltage_X100());
    printf(" V  温度=%d~%d C\r\n",
           (int)s_temp_min_c, (int)s_temp_max_c);

    printf("         累计充电 %lu 秒, 累计电量 ",
           (unsigned long)s_charge_seconds);
    print_x10((int32_t)s_charge_energy_x10);
    printf(" kWh; CAN 收 %lu 帧 / 发 %lu 帧",
           (unsigned long)s_rx_count,
           (unsigned long)s_tx_count);

    if (CAN_GetRxOverflowCount() != 0u)
    {
        printf("  [接收队列溢出 %lu 帧]", (unsigned long)CAN_GetRxOverflowCount());
    }
    if (CAN_GetTxFailCount() != 0u)
    {
        printf("  [发送失败 %lu 次]", (unsigned long)CAN_GetTxFailCount());
    }
    printf("\r\n");

    /* --- CAN 物理层诊断行 -------------------------------------------------
     * 排查「收不到任何报文」时的判据：
     *   REC 持续增长        -> 总线上有电平，但解不出帧（波特率/极性/接线错）
     *   REC=0 且收不到帧    -> 本节点根本没看到总线活动（收发器/接线/终端电阻）
     *   LEC=ACK错误         -> 本节点发帧无人应答（对端不在线或没在 ACK）
     * --------------------------------------------------------------------*/
    {
        uint8_t tec = 0;
        uint8_t rec = 0;
        uint8_t lec = 0;

        CAN_GetBusCounters(&tec, &rec, &lec);

        printf("         CAN总线: TEC=%u REC=%u LEC=%u(%s) 错误中断 %lu 次%s",
               (unsigned int)tec, (unsigned int)rec, (unsigned int)lec,
               LecStr(lec), (unsigned long)CAN_GetErrorCount(),
               CAN_IsBusOff() ? "  *** Bus-Off! ***" : "");
#if (BMS_HEARTBEAT_ENABLE)
        printf("   [心跳已发 %lu 帧]", (unsigned long)s_hb_count);
#endif
        printf("\r\n");
    }
    printf("\r\n");
}

/******************* (C) COPYRIGHT 2025 GB27930 BMS Simulator *****END OF FILE***/
