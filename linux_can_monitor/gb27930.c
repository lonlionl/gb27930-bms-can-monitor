/**
 * @file    gb27930.c
 * @brief   GB/T 27930-2015 协议解析与充电机侧状态机实现
 *
 * 文件结构
 * --------
 *   §1  协议布局常量（字节偏移 / 分辨率 / 偏移量）——所有魔数集中在此
 *   §2  小端读写工具函数 + 保留位填 1 辅助
 *   §3  硬件滤波 ID 表
 *   §6  组包：充电机侧各报文的构造与发送
 *   §7  解析：BMS 侧各报文的拆包与物理量换算
 *   §6  状态机：初始化 / 复位 / 超时判定 / 状态迁移
 *   §7  对外接口：process_frame / tick / 字符串转换 / 存储记录导出
 *
 * 字节序约定
 * ----------
 *   GB/T 27930-2015 标准 4.4 原文：「数据信息传输采用低字节先发送的格式。」
 *   因此数据域内的所有多字节字段一律 **小端（低字节在前）**，
 *   由 §4 的 le16_put / le16_get / le24_get 统一处理。
 *   J1939 传输协议 TP.CM / TP.DT 里的长度、PGN、包间隔字段按 J1939-21
 *   同样是小端，与本标准数据域一致 —— 两者**不再"相反"**。
 *
 * 填充约定
 * ----------
 *   标准 7.9：「本标准未规定的无效位或字段填充 1。本标准未规定的位或预留位
 *   填充 1。」因此发送路径先 fill_reserved()（memset 0xFF）再写真实字段。
 *
 * 物理量换算
 * ----------
 *   物理量 = 原始无符号整数 × 分辨率(Resolution) + 偏移量(Offset)
 *   组包时反向：原始整数 = round((物理量 - 偏移量) / 分辨率)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "gb27930.h"
#include "ui.h"

#include <stdio.h>
extern int g_verbose;   /* 详细启动日志开关，定义在 main.c */
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

/*==============================================================================
 * §1  协议布局常量
 * ============================================================================
 * 说明：所有字节偏移均以 0 为起点（即 B1 对应偏移 0），与标准文档的
 *       「起始字节」相差 1。集中定义便于适配不同厂家的实现差异。
 *============================================================================*/

/*------------------------- 通用分辨率与偏移量 -----------------------------*/
#define L_RES_V_0_1        0.1f      /* 电压   0.1 V/位 */
#define L_RES_V_0_01       0.01f     /* 电压   0.01 V/位 */
#define L_RES_I_0_1        0.1f      /* 电流   0.1 A/位 */
#define L_OFF_I_400        (-400.0f) /* 电流偏移 -400 A（BCL/BCS/CML 都有） */
#define L_RES_T_1          1.0f      /* 温度   1 ℃/位 */
#define L_OFF_T_50         (-50.0f)  /* 温度偏移 -50 ℃ */
#define L_RES_SOC_0_1      0.1f      /* SOC    0.1 %/位（**BCP 专用**） */
#define L_RES_SOC_1        1.0f      /* SOC    1 %/位  （**BCS / BSD 专用**） */
#define L_RES_ENERGY_0_1   0.1f      /* 能量   0.1 kWh/位 */
#define L_RES_CAP_0_1      0.1f      /* 容量   0.1 Ah/位 */
#define L_RES_TIME_1       1.0f      /* 时间   1 min/位 */

/*------------------------- BCP 布局（13 字节） ----------------------------*/
#define BCP_OFF_MAX_CELL_V     0     /* B1-B2  单体最高允许充电电压 0.01 V */
#define BCP_OFF_MAX_I          2     /* B3-B4  最高允许充电电流     0.1 A, -400 */
#define BCP_OFF_ENERGY         4     /* B5-B6  标称总能量           0.1 kWh */
#define BCP_OFF_MAX_TOTAL_V    6     /* B7-B8  最高允许充电总电压   0.1 V */
#define BCP_OFF_MAX_TEMP       8     /* B9     最高允许温度         1 ℃, -50 */
#define BCP_OFF_SOC            9     /* B10-B11 整车 SOC            **0.1 %** */
#define BCP_OFF_CUR_V          11    /* B12-B13 当前电池电压        0.1 V */
#define BCP_LEN                13

/*------------------------- BCL 布局（5 字节，标准表 17） ------------------
 * ★ 标准 BCL 只有 3 个字段共 5 字节：电压需求 / 电流需求 / 充电模式。
 *   「允许充电电压 / 允许充电电流」是 **BCP** 的字段，BCL 里没有 ——
 *   本文件原先在 B6-B8 读写这两个量，已按标准删除。
 *--------------------------------------------------------------------------*/
#define BCL_OFF_V_DEMAND       0     /* B1-B2  电压需求      0.1 V */
#define BCL_OFF_I_DEMAND       2     /* B3-B4  电流需求      0.1 A, -400 */
#define BCL_OFF_MODE           4     /* B5     充电模式      1 恒压 / 2 恒流 */
#define BCL_LEN                5     /* 标准表 5 / 表 17：BCL 数据域 5 字节 */

/*------------------------- BCS 布局（标准 9 字节 / 本工程 8 字节单帧） ----
 *   B5-B6 是**位打包**字段（SPN3077）：
 *     1-12 位 = 最高单体电压  0.01 V/位, 0 偏移, 0~24 V
 *     13-16 位 = 该单体所在组号 1/位, 0 偏移, 0~15
 *   B7 = SOC（**1 %/位**，注意与 BCP 的 0.1 %/位 不同）
 *   B8-B9 = 估算剩余充电时间 1 min/位, 0~600
 *
 * 【已知偏差】标准表 5 / 表 18 声明 BCS 为 **9 字节**，超过 CAN 单帧 8 字节
 * 上限，按标准 6.2 注 7 应经 J1939 传输协议（TP）传输。
 * 本轮**有意不改 TP 架构**（改动面会牵动 BRM/BCP 既有 TP 逻辑），
 * 因此仍按 8 字节单帧发送：B1~B8 送出，**缺 B9（剩余充电时间高字节）**。
 * 解析侧按 DLC >= 8 容错，并把"只剩低字节"记进 remain_charge_lo_byte。
 *--------------------------------------------------------------------------*/
#define BCS_OFF_MEASURE_V      0     /* B1-B2  充电电压测量值 0.1 V */
#define BCS_OFF_MEASURE_I      2     /* B3-B4  充电电流测量值 0.1 A, -400 */
#define BCS_OFF_CELL_V_GROUP   4     /* B5-B6  最高单体电压(1-12 位) + 组号(13-16 位) */
#define BCS_OFF_SOC            6     /* B7     当前 SOC       1 % */
#define BCS_OFF_REMAIN_LO      7     /* B8     估算剩余充电时间低字节（B9 为高字节） */
#define BCS_LEN                8     /* 当前实现 8 字节（标准 9，见上） */
#define BCS_LEN_STD            9     /* 标准长度，仅供注释与上报使用 */

#define BCS_CELL_V_MASK        0x0FFFu  /* 低 12 位 = 电压 */
#define BCS_CELL_GROUP_MASK    0x000Fu  /* 高 4 位  = 组号 */
#define BCS_CELL_GROUP_SHIFT   12u
#define BCS_CELL_V_MAX_X100    2400u    /* 24.00 V 上限 */

/*------------------------- BSM 布局（7 字节，标准表 20） -----------------
 * ★ 标准 BSM **没有电压字段**，B1 只是"最高单体电压所在编号"。
 *   本文件原先把「最高单体电压」放在 B2-B3，把温度挤到了 B4/B5，
 *   且完全没有 B6/B7 的 6 个状态位 —— 已按标准整体重写。
 *--------------------------------------------------------------------------*/
#define BSM_OFF_CELL_V_NO      0     /* B1     最高单体电压所在编号 1/位, 1 偏移 */
#define BSM_OFF_MAX_TEMP       1     /* B2     最高温度  1 ℃, -50 */
#define BSM_OFF_MAX_TEMP_NO    2     /* B3     最高温度检测点编号 1/位, 1 偏移 */
#define BSM_OFF_MIN_TEMP       3     /* B4     最低温度  1 ℃, -50 */
#define BSM_OFF_MIN_TEMP_NO    4     /* B5     最低温度检测点编号 1/位, 1 偏移 */
#define BSM_OFF_STATUS_B6      5     /* B6     SPN3090~3093 四个 2 位状态字段 */
#define BSM_OFF_STATUS_B7      6     /* B7     SPN3094~3096 + 未定义位 */
#define BSM_LEN                7     /* 标准表 5 / 表 20：BSM 数据域 7 字节 */

/* B6/B7 的 2 位字段位移宏统一定义在 gb27930.h 的「状态位枚举」段
 * （BSM_B6_* / BSM_B7_*），与 STM32 端 bms_protocol.c 同名同值，
 * 这里不再重复定义，避免两处取值漂移。 */

/* BSM 的检测点编号上限（标准 1~128） */
#define BSM_TEMP_NO_MAX        128u
#define BSM_CELL_NO_MAX        256u

/*------------------------- BRM 布局（41 字节，标准表 11） ----------------
 * ★ 标准原文长度不自洽：表 5 声明 41，表 11 逐行相加得 49。
 *   本工程**保持 41 字节**，字段顺序严格按表 11 排列：
 *     B1-B3 版本 / B4 电池类型 / B5-B6 额定容量 / B7-B8 额定总电压 /
 *     B9-B12 厂商 ASCII 4B / B13-B16 电池组序号 4B /
 *     B17 年(1985 偏移) B18 月 B19 日 / B20-B22 充电次数 3B /
 *     B23 产权标识 / B24 预留 / B25-B41 VIN 17B
 *   SPN2576（BMS 软件版本号 8 字节）落在 41 字节之外，本工程不发送，
 *   B37-B41 因此**不存在**（VIN 一直占到 B41）。
 *--------------------------------------------------------------------------*/
#define BRM_OFF_VERSION        0     /* B1-B3  BMS 通信协议版本号（小端 24 位） */
#define BRM_LEN_VERSION        3
#define BRM_OFF_BATT_TYPE      3     /* B4     电池类型 */
#define BRM_OFF_RATED_CAP      4     /* B5-B6  额定容量   0.1 Ah */
#define BRM_OFF_RATED_V        6     /* B7-B8  额定总电压 0.1 V */
#define BRM_OFF_MANUFACTURER   8     /* B9-B12 生产厂商 4 字节 ASCII */
#define BRM_LEN_MANUFACTURER   4
#define BRM_OFF_PACK_SERIAL    12    /* B13-B16 电池组序号（预留，厂商自定义） */
#define BRM_LEN_PACK_SERIAL    4
#define BRM_OFF_PROD_YEAR      16    /* B17    生产年份（1 年/位，**1985 偏移**） */
#define BRM_OFF_PROD_MONTH     17    /* B18    生产月份 */
#define BRM_OFF_PROD_DAY       18    /* B19    生产日 */
#define BRM_OFF_CHARGE_CNT     19    /* B20-B22 电池组充电次数（小端 3 字节） */
#define BRM_OFF_OWNERSHIP      22    /* B23    电池组产权标识 0 租赁 / 1 车自有 */
#define BRM_OFF_RESERVED       23    /* B24    预留 */
#define BRM_OFF_VIN            24    /* B25-B41 车辆识别码 VIN 17 字节 */
#define BRM_LEN_VIN            17
#define BRM_LEN                41
#define BRM_YEAR_OFFSET        1985  /* 生产年份偏移量 */

/*------------------------- CML 布局（8 字节） ----------------------------
 * ★ B5-B8 的电流字段都带 **-400 A 偏移**（标准表 14），
 *   原先按 0 偏移编码，会与真实充电机解析结果差 400 A。
 *--------------------------------------------------------------------------*/
#define CML_OFF_MAX_V          0     /* B1-B2 最高输出电压 0.1 V, 0 偏移 */
#define CML_OFF_MIN_V          2     /* B3-B4 最低输出电压 0.1 V, 0 偏移 */
#define CML_OFF_MAX_I          4     /* B5-B6 最大输出电流 0.1 A, -400 A 偏移 */
#define CML_OFF_MIN_I          6     /* B7-B8 最小输出电流 0.1 A, -400 A 偏移 */
#define CML_LEN                8

/*------------------------- CHM / BHM / CRM / CTS / CSD 长度 --------------
 * CHM 3 字节：B1 主版本 + B2-B3 次版本（小端）
 *   ★ CHM 才有版本号。标准表 9 的 BHM **2 字节全是"最高允许充电总电压"**，
 *     没有版本号 —— 原先按版本号解析 BHM 是错的。
 * CRM 8 字节：B1 辨识结果 + B2-B5 充电机编号 + B6-B8 区域编码（ASCII）
 * CTS 7 字节：秒/分/时/日/月/年，全部压缩 BCD
 *--------------------------------------------------------------------------*/
#define CHM_LEN                3
#define BHM_LEN                2
#define CRM_LEN                8
#define CRM_OFF_RESULT         0     /* B1    辨识结果 0x00 / 0xAA */
#define CRM_OFF_ID             1     /* B2-B5 充电机编号（小端 32 位） */
#define CRM_OFF_REGION         5     /* B6-B8 区域编码（标准 ASCII，可选项） */
#define CRM_LEN_REGION         3     /* 区域编码定长 3 字节 */
#define CTS_LEN                7
#define CSD_LEN                8
#define CSD_OFF_MINUTES        0     /* B1-B2 累计充电时间 1 min/位 */
#define CSD_OFF_ENERGY         2     /* B3-B4 输出能量 0.1 kWh/位 */
#define CSD_OFF_CHARGER_ID     4     /* B5-B8 充电机编号（小端 32 位，1 偏移） */
#define CSD_MAX_MINUTES        600u  /* 标准 0~600 min */
#define CSD_MAX_ENERGY_X10     1000u /* 标准 0~1000（即 100.0 kWh） */

/*------------------------- CCS / BEM / CEM 布局（标准表 19 / 28 / 29） ----
 * ★ 这三个报文的长度、偏移、位移与枚举**统一定义在 gb27930.h 里**，
 *   本文件不再重复一份 —— 同一族宏在两处各写一遍，看起来"值一样就行"，
 *   实际上改一处忘一处就是静默的字段错位（两端还都能编过）。
 *   校验脚本 tools/gb27930_layout_check.js 现在会把"同名宏定义两次"
 *   直接报出来，就是为了堵这个口子。
 *
 *   回顾：CCS 是本工程原先**完全没发**的报文，而标准规定 BMS 侧 1 s
 *   收不到就"立即结束充电" —— 等于对端必然超时。详见 gb27930.h 的说明。 */

/* 本端（BMS 侧）对 BEM 的用法：**只解析、不上报**
 *   BEM 是 BMS 发给充电机的错误报文；本端是模拟 BMS，会**发送** BEM，
 *   同时也会**解析**对端可能发来的 BEM（用于日志与界面展示）。
 *   发送用的错误类别掩码 BEM_ERR_* 定义在本文件（见 §1 上面的 BEM 段），
 *   因为它只服务于本文件的 BemMaskOf()，不属于跨文件接口。 */

/* 错误报文的 2 位枚举（标准表 28 / 表 29 同用） */
#define ERR_ST_NORMAL          0u    /* 00 正常 */
#define ERR_ST_TIMEOUT         1u    /* 01 超时 */
#define ERR_ST_UNRELIABLE      2u    /* 10 不可信状态 */

/** CEM 的每轮最大发送次数（250 ms x 20 = 5 s） */
#define T_CEM_MAX_TX           20u

/*------------------------- BSD 布局（7 字节，标准表 26） ----------------*/
#define BSD_OFF_SOC            0     /* B1    中止荷电状态 SOC 1 %/位 */
#define BSD_OFF_MIN_CELL_V     1     /* B2-B3 单体**最低**电压 0.01 V/位 */
#define BSD_OFF_MAX_CELL_V     3     /* B4-B5 单体**最高**电压 0.01 V/位 */
#define BSD_OFF_MIN_TEMP       5     /* B6    **最低**温度 1 ℃, -50 偏移 */
#define BSD_OFF_MAX_TEMP       6     /* B7    **最高**温度 1 ℃, -50 偏移 */
#define BSD_LEN                7

/*------------------------- 准备就绪 / 充电模式取值 ------------------------
 * ★ GB_READY_* / GB_CRM_ID_* 这组**协议取值常量**定义在 gb27930.h 里 ——
 *   它们不只本文件用：selftest.c / uitest.c 也要拿它们做断言。
 *   放在 .c 里别的编译单元就看不到，编译直接报 undeclared。
 *   【教训】协议常量一律进头文件；.c 顶部只放本文件私有的实现细节。
 *--------------------------------------------------------------------------*/

/*==============================================================================
 * §2  周期与超时参数（GB/T 27930-2015）
 *============================================================================*/

#define T_CHM_PERIOD_MS        250u    /* 充电机握手报文周期 */
#define T_CRM_PERIOD_MS        250u    /* 充电机辨识报文周期 */
#define T_CTS_PERIOD_MS        500u    /* 时间同步报文周期：标准表 4 规定 500 ms。
                                          曾被误按"国标 250 ms"改成 250，此处改回
                                          标准值 500 ms。 */
#define T_CML_PERIOD_MS        250u    /* 最大输出能力周期   */
#define T_CRO_PERIOD_MS        250u    /* 输出准备就绪周期   */
#define T_CCS_PERIOD_MS         50u    /* 充电机充电状态周期：标准表 5 规定 **50 ms** */
#define T_CSD_PERIOD_MS        250u    /* 充电机统计数据周期（结束阶段发一次即可） */
#define T_CEM_PERIOD_MS        250u    /* 充电机错误报文周期：标准表 5 规定 250 ms */

#define T_HANDSHAKE_MS         5000u   /* 握手阶段通用超时（标准第 8 章：5 s） */
#define T_IDENTIFY_MS          5000u   /* 辨识阶段通用超时 */
#define T_PARAM_MS             5000u   /* 参数配置阶段通用超时（BCP 标准 5 s） */

/* 【逐报文超时，标准原文分散在第 10 章，务必不要统一成 5 s】
 *   BRO  标准 p.11：BMS 在 **60 s** 内未准备好，则充电机进行等待
 *   CRO  标准 p.11：充电机在 **60 s** 内未准备好，则 BMS 进行等待
 *   BCL  标准 p.12：**如果充电机在 1 s 内没有收到该报文，即为超时错误，
 *                   充电机应立即结束充电。**
 *   BCS  标准 p.12：如果充电机在 5 s 内没有收到该报文，即为超时错误
 *   CCS  标准 p.13：如果 BMS 在 1 s 内没有收到该报文……（本工程未实现 CCS）
 * 通用超时（第 8 章）：除特殊规定外均为 5 s。 */
#define T_READY_MS             60000u  /* 准备就绪等待：标准 60 s（不是 5 s！） */
#define T_BCL_MS               1000u   /* BCL 专用超时：标准 1 s */
#define T_BCS_MS               5000u   /* BCS 专用超时：标准 5 s */
#define T_CHARGE_MS            5000u   /* 充电阶段通用刷新超时（BCS/BSM 等） */

/* 多帧组包超时（J1939 TP 规定 1 s） */
#define T_ISOTP_MS             1000u

/*==============================================================================
 * §3  充电机默认输出能力（CML 报文内容，可按实际桩参数修改）
 *============================================================================*/

#define CHARGER_MAX_OUTPUT_V   750.0f   /* 最高输出电压 750.0 V */
#define CHARGER_MIN_OUTPUT_V   200.0f   /* 最低输出电压 200.0 V */
#define CHARGER_MAX_OUTPUT_I   250.0f   /* 最大输出电流 250.0 A */
#define CHARGER_MIN_OUTPUT_I   0.0f     /* 最小输出电流 0.0 A   */

/* 区域编码：标准表 10 的 SPN2562 是 **3 字节标准 ASCII**（可选项）。
 * 原先只发 1 个字节（0x01），既不是 ASCII 也不足 3 字节，已按标准改为
 * 3 字节 ASCII 字符串。 */
#define CHARGER_REGION_ASCII   "001"    /* 3 字节标准 ASCII 区域编码 */

/* 充电机编号：SPN2561 写入 CRM（0 偏移），SPN3613 写入 CSD（**1 偏移**）。
 * 标准对 SPN3613 规定"1/位，1 偏移量"，所以 CSD 里要发 charger_id + 1。 */
#define CHARGER_ID_VALUE       0x00000001u

#define GB_PROTOCOL_VERSION_H  0x01u    /* 协议版本 V1.1 主版本 */
#define GB_PROTOCOL_VERSION_L  0x0001u  /* 协议版本 V1.1 次版本（小端发送） */

/*==============================================================================
 * §4  小端读写工具（标准 4.4：低字节先发送）
 *============================================================================*/

/** 未使用的位 / 保留位填充值：标准 7.9 规定填 1 */
#define GB_FILL_RESERVED        0xFFu

/**
 * @brief  把整段缓冲按标准 7.9 填充为 1
 * @param  p 缓冲首地址
 * @param  n 要填充的字节数：填充范围是 [p, p+n)，即**从首字节起 n 个字节**
 * @note   只能在写真实字段**之前**调用；真实字段随后覆盖，不会被冲掉。
 *         长度小于 8 的报文，其数据域内用不到的字节都靠它填 1。
 */
static inline void fill_reserved(uint8_t *p, uint8_t n)
{
    memset(p, (int)GB_FILL_RESERVED, (size_t)n);
}

/** 小端 16 位读取（低字节在前） */
static inline uint16_t le16_get(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/** 小端 16 位写入（低字节在前） */
static inline void le16_put(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/** 小端 24 位读取（低字节在前） */
static inline uint32_t le24_get(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/** 组包辅助：物理量 -> 原始整数（四舍五入，负值钳位到 0） */
static inline uint16_t encode_u16(float value, float res, float offset)
{
    float raw = (value - offset) / res;
    if (raw < 0.0f)
    {
        raw = 0.0f;
    }
    if (raw > 65535.0f)
    {
        raw = 65535.0f;
    }
    return (uint16_t)(raw + 0.5f);
}

/** 解析辅助：原始整数 -> 物理量 */
static inline float decode_u16(uint16_t raw, float res, float offset)
{
    return (float)raw * res + offset;
}

/*------------------------------------------------------------------------------
 * 压缩 BCD 工具（**CTS 专用**）
 *
 *   标准表 13 的 SPN2823 明确写着每个字节是「压缩 BCD 码」：
 *     高 4 位 = 十位，低 4 位 = 个位。
 *   例如 2025 年 → 0x20 0x25；9 月 → 0x09；25 日 → 0x25。
 *
 *   【为什么不能直接发二进制】原先 send_cts() 把小端的 tm_year、以及
 *   二进制的月/日/时/分/秒原样塞进数据域，年份占 2 字节、其余 1 字节，
 *   顺序还是"年 月 日 时 分 秒"。两处都与标准不符：
 *     · 顺序应为 **秒 分 时 日 月 年**（反直觉，但原文如此）
 *     · 编码应为**压缩 BCD**
 *---------------------------------------------------------------------------*/

/** 二进制 0~99 -> 压缩 BCD（超出范围按 0 处理，避免产生非法 BCD） */
static inline uint8_t bin2bcd(uint8_t v)
{
    if (v > 99u)
    {
        return 0u;
    }
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

/** 压缩 BCD -> 二进制（非法 BCD 返回 0） */
static inline uint8_t bcd2bin(uint8_t v)
{
    uint8_t hi = (uint8_t)((v >> 4) & 0x0Fu);
    uint8_t lo = (uint8_t)(v & 0x0Fu);

    if (hi > 9u || lo > 9u)
    {
        return 0u;
    }
    return (uint8_t)(hi * 10u + lo);
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
 * §4c  CTS 组包的对外可见实现（供 selftest 直接验证字节顺序与 BCD 编码）
 *============================================================================*/

uint8_t gb27930_bcd_encode(uint8_t bin)
{
    return bin2bcd(bin);
}

uint8_t gb27930_bcd_decode(uint8_t bcd)
{
    return bcd2bin(bcd);
}

/**
 * @brief  按标准表 13 把时间打包进 CTS 的 7 字节数据域
 * @note   **顺序是「秒、分、时、日、月、年」，且全部为压缩 BCD 码。**
 *         年占 2 字节（B6 = 世纪+千位/百位，B7 = 十位/个位），同样是压缩 BCD。
 *         参数顺序刻意按"协议字节顺序"排，这样函数体一眼能对上行号。
 */
void gb27930_cts_pack(uint8_t out[CTS_LEN],
                      uint16_t year, uint8_t month, uint8_t day,
                      uint8_t hour, uint8_t minute, uint8_t second)
{
    out[0] = bin2bcd(second);                          /* B1 秒 */
    out[1] = bin2bcd(minute);                          /* B2 分 */
    out[2] = bin2bcd(hour);                            /* B3 时 */
    out[3] = bin2bcd(day);                             /* B4 日 */
    out[4] = bin2bcd(month);                           /* B5 月 */
    out[5] = bin2bcd((uint8_t)((year / 100u) % 100u)); /* B6 年高位（20） */
    out[6] = bin2bcd((uint8_t)(year % 100u));          /* B7 年低位（25） */
}

/** CSD 的 SPN3613 充电机编号"1 偏移"编码：线上值 = 编号 + 1 */
uint32_t gb27930_csd_encode_id(uint32_t charger_id)
{
    return charger_id + 1u;
}

/**
 * @brief  按标准表 29 打包 CEM 的 4 字节数据域（供 selftest 直接验证字段位移）
 * @param  out      输出缓冲，至少 4 字节
 * @param  err_mask 要上报的错误类别位掩码（CEM_ERR_* 按位或）
 * @note   CEM 的字段错位与 BEM **不同**：B1 只有一个字段（SPN3921），
 *         B2/B3 各两个，B3 的第三个字段（SPN3926）落在 B3.5-3.6（位移 4）。
 *         所以这两张表必须各写各的位移，不能互相套用。
 */
void gb27930_cem_pack(uint8_t out[CEM_LEN], uint8_t err_mask)
{
    uint8_t b1 = 0;
    uint8_t b2 = 0;
    uint8_t b3 = 0;
    uint8_t b4 = 0;

    b1 = GB_FIELD2_SET(b1, CEM_B1_IDENT_SHIFT,
                       ((err_mask & CEM_ERR_IDENT) ? ERR_ST_TIMEOUT : ERR_ST_NORMAL));
    b2 = GB_FIELD2_SET(b2, CEM_B2_BCP_SHIFT,
                       ((err_mask & CEM_ERR_BCP) ? ERR_ST_TIMEOUT : ERR_ST_NORMAL));
    b2 = GB_FIELD2_SET(b2, CEM_B2_BRO_SHIFT,
                       ((err_mask & CEM_ERR_BRO) ? ERR_ST_TIMEOUT : ERR_ST_NORMAL));
    b3 = GB_FIELD2_SET(b3, CEM_B3_BCS_SHIFT,
                       ((err_mask & CEM_ERR_BCS) ? ERR_ST_TIMEOUT : ERR_ST_NORMAL));
    b3 = GB_FIELD2_SET(b3, CEM_B3_BCL_SHIFT,
                       ((err_mask & CEM_ERR_BCL) ? ERR_ST_TIMEOUT : ERR_ST_NORMAL));
    b3 = GB_FIELD2_SET(b3, CEM_B3_BST_SHIFT,
                       ((err_mask & CEM_ERR_BST) ? ERR_ST_TIMEOUT : ERR_ST_NORMAL));
    b4 = GB_FIELD2_SET(b4, CEM_B4_BSD_SHIFT,
                       ((err_mask & CEM_ERR_BSD) ? ERR_ST_TIMEOUT : ERR_ST_NORMAL));
    /* B4.3-4.8 "其他"（6 位，标准标为可选项）：按 7.9 填 1 */
    b4 |= CEM_B4_OTHER_FILL;

    out[CEM_OFF_B1] = b1;
    out[CEM_OFF_B2] = b2;
    out[CEM_OFF_B3] = b3;
    out[CEM_OFF_B4] = b4;
}

/*==============================================================================
 * §5  硬件滤波
 *============================================================================*/

/** 充电机侧需要接收的 BMS→充电机 报文 ID（精确匹配）
 *  ★ 加了 BEM（0x081E56F4，优先权 2）后由 10 条变 11 条，与
 *    gb27930.h 的 GB_RX_FILTER_COUNT 保持一致。 */
static const uint32_t s_gb_rx_ids[GB_RX_FILTER_COUNT] = {
    GB_ID_BHM,        /* BMS 握手              */
    GB_ID_BCP,        /* 充电参数              */
    GB_ID_BRO,        /* 充电准备就绪          */
    GB_ID_BCL,        /* 电池充电需求          */
    GB_ID_BCS,        /* 电池充电总状态        */
    GB_ID_BSM,        /* 动力蓄电池状态信息    */
    GB_ID_BST,        /* BMS 中止充电          */
    GB_ID_BSD,        /* BMS 中止充电统计      */
    GB_ID_BEM,        /* BMS 错误报文（优先权2）*/
    GB_ID_BRM_TPCM,   /* BMS 辨识 多帧连接管理 */
    GB_ID_BRM_TPDT    /* BMS 辨识 多帧数据传输 */
};

int gb27930_apply_rx_filter(can_layer_t *cl)
{
    int rc = can_apply_filter_ids(cl, s_gb_rx_ids, GB_RX_FILTER_COUNT);
    int i;

    /* 只报个数。以前把 10 条 ID 全列出来，启动日志被刷得看不清重点；
     * 想看完整列表加 -v。 */
    if (rc == 0 && g_verbose)
    {
        fprintf(stdout, "[GB27930] 硬件滤波已放行 %d 条报文 ID\n",
                (int)GB_RX_FILTER_COUNT);
        for (i = 0; i < (int)GB_RX_FILTER_COUNT; i++)
        {
            fprintf(stdout, "          [%2d] 0x%08X\n", i + 1, s_gb_rx_ids[i]);
        }
    }
    return rc;
}

/*==============================================================================
 * §6  时间与事件辅助
 *============================================================================*/

/** 取单调时间戳（毫秒） */
static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/** 记录错误码并触发回调 */
static void raise_error(gb_context_t *ctx, gb_error_t err, const char *detail)
{
    ctx->error = err;
    if (ctx->cb.on_error != NULL)
    {
        ctx->cb.on_error(ctx->cb.arg, err, detail);
    }
    else
    {
        ui_log("ERROR", "%s%s%s", gb27930_error_str(err),
               (detail != NULL) ? " | " : "", (detail != NULL) ? detail : "");
    }
}

/** 状态迁移：更新时间戳并触发回调 */
static void set_state(gb_context_t *ctx, gb_state_t new_state)
{
    gb_state_t old = ctx->state;

    if (old == new_state)
    {
        return;
    }

    ctx->state          = new_state;
    ctx->state_enter_ms = ctx->now_ms;

    if (ctx->cb.on_state_change != NULL)
    {
        ctx->cb.on_state_change(ctx->cb.arg, old, new_state);
    }
    else
    {
        ui_log("STATE", "%s -> %s", gb27930_state_str(old), gb27930_state_str(new_state));
    }
}

/** 刷新报文接收时间戳（用于充电阶段的周期超时判定） */
static void touch_rx(gb_context_t *ctx, const can_item_t *item)
{
    if (ctx->now_ms == 0)
    {
        ctx->now_ms = mono_ms();
    }
    ctx->last_rx_ms = ctx->now_ms;
    (void)item;
}

/*==============================================================================
 * §7  组包与发送（充电机侧）
 *============================================================================*/

/** 发送 CHM 充电机握手报文
 *  标准表 3：CHM 数据域 **3 字节** —— B1 版本主版本、B2-B3 版本次版本（小端）。
 *  这 3 字节都是有效字段，报文内没有需要填 1 的保留位。 */
static int send_chm(can_layer_t *cl)
{
    uint8_t d[CHM_LEN];

    d[0] = GB_PROTOCOL_VERSION_H;                     /* B1    版本主版本 */
    le16_put(&d[1], GB_PROTOCOL_VERSION_L);           /* B2-B3 版本次版本（小端） */

    return can_layer_send(cl, GB_ID_CHM, d, CHM_LEN);
}

/** 发送 CRM 充电机辨识报文（标准表 10：8 字节）
 *   B1    辨识结果（0x00 = BMS 不能辨识；0xAA = BMS 能辨识）
 *   B2-B5 充电机编号，1/位，0 偏移，0~0xFFFFFFFF（小端 32 位）
 *   B6-B8 充电机/充电站所在区域编码，**标准 ASCII 码**（可选项，定长 3 字节）
 *  ★ 原先把 B1-B7 当作 7 字节 ASCII 编号、B8 当 1 字节区域编号，与标准不符。 */
static int send_crm(can_layer_t *cl)
{
    uint8_t d[CRM_LEN];

    fill_reserved(d, sizeof(d));

    /* B1 辨识结果：本工程模拟的 BMS 始终能辨识，故取 0xAA */
    d[CRM_OFF_RESULT] = GB_CRM_ID_OK;

    /* B2-B5 充电机编号（小端 32 位） */
    {
        uint32_t id = CHARGER_ID_VALUE;
        d[CRM_OFF_ID + 0] = (uint8_t)(id & 0xFFu);
        d[CRM_OFF_ID + 1] = (uint8_t)((id >> 8) & 0xFFu);
        d[CRM_OFF_ID + 2] = (uint8_t)((id >> 16) & 0xFFu);
        d[CRM_OFF_ID + 3] = (uint8_t)((id >> 24) & 0xFFu);
    }

    /* B6-B8 区域编码：3 字节标准 ASCII（可选项，本工程发送） */
    memcpy(&d[CRM_OFF_REGION], CHARGER_REGION_ASCII, CRM_LEN_REGION);

    return can_layer_send(cl, GB_ID_CRM, d, CRM_LEN);
}

/** 发送 CTS 充电机时间同步报文（标准表 13：7 字节）
 *
 *  ★ 两个反直觉之处，都按标准原文实现：
 *    1. **顺序是「秒、分、时、日、月、年」**，不是常见的年月日时分秒：
 *         B1 秒  B2 分  B3 时  B4 日  B5 月  B6-B7 年
 *    2. **全部是压缩 BCD 码**（高 4 位十位、低 4 位个位），年占 2 字节
 *       也同样是压缩 BCD，例如 2025 年 → B6=0x20 B7=0x25。
 *
 *  原实现按「年(小端 2B) 月 日 时 分 秒」发二进制值，顺序与编码都错。 */
static int send_cts(can_layer_t *cl)
{
    uint8_t  d[CTS_LEN];
    time_t   t = time(NULL);
    struct tm tmv;

    localtime_r(&t, &tmv);

    gb27930_cts_pack(d,
                     (uint16_t)(tmv.tm_year + 1900),
                     (uint8_t)(tmv.tm_mon + 1),
                     (uint8_t)tmv.tm_mday,
                     (uint8_t)tmv.tm_hour,
                     (uint8_t)tmv.tm_min,
                     (uint8_t)tmv.tm_sec);

    return can_layer_send(cl, GB_ID_CTS, d, CTS_LEN);
}

/** 发送 CML 充电机最大输出能力报文（标准表 14：8 字节，全部是有效字段）
 *  ★ B5-B8 的两个电流字段都带 **-400 A 偏移**，不是 0 偏移。 */
static int send_cml(can_layer_t *cl)
{
    uint8_t d[CML_LEN];

    le16_put(&d[CML_OFF_MAX_V], encode_u16(CHARGER_MAX_OUTPUT_V, L_RES_V_0_1, 0.0f));
    le16_put(&d[CML_OFF_MIN_V], encode_u16(CHARGER_MIN_OUTPUT_V, L_RES_V_0_1, 0.0f));
    le16_put(&d[CML_OFF_MAX_I], encode_u16(CHARGER_MAX_OUTPUT_I, L_RES_I_0_1, L_OFF_I_400));
    le16_put(&d[CML_OFF_MIN_I], encode_u16(CHARGER_MIN_OUTPUT_I, L_RES_I_0_1, L_OFF_I_400));

    return can_layer_send(cl, GB_ID_CML, d, CML_LEN);
}

/** 发送 BRO/CRO 准备就绪报文（标准表 15 / 表 16：1 字节，无保留位）
 *   0x00 = 未做好/未完成；0xAA = 完成准备；0xFF = 无效 */
static int send_ready(can_layer_t *cl, uint32_t can_id, int ready)
{
    uint8_t d[1];

    d[0] = ready ? GB_READY_READY : GB_READY_NOT_READY;
    return can_layer_send(cl, can_id, d, 1);
}

/** 发送 CST 充电机中止充电报文（标准表 25：4 字节）
 *   B1    中止原因（4 个 2 位字段，SPN3521）
 *   B2-B3 故障原因（SPN3522 用到 6 个 2 位字段，空余位按 7.9 填 1）
 *   B4    错误原因（2 个 2 位字段，SPN3523；空余位按 7.9 填 1）
 *  ★ 每个字节内部都是 2 位字段，**不是整字节位掩码**。
 *    参数取值请用 gb27930.h 的 GB_CST_*_SHIFT 宏拼装。 */
static int send_cst(can_layer_t *cl, uint8_t reason_flags,
                    uint8_t fault_lo, uint8_t fault_hi, uint8_t error_flags)
{
    uint8_t d[4];

    fill_reserved(d, sizeof(d));   /* 未定义位 / 保留位按标准 7.9 填 1 */
    d[0] = reason_flags;
    d[1] = fault_lo;                /* B2 故障原因（SPN3522 的高 4 个 2 位字段） */
    d[2] = fault_hi;                /* B3 故障原因（SPN3522 的第 5-6 个字段 + 填充） */
    d[3] = error_flags;             /* B4 错误原因（2 个 2 位字段 + 填充） */

    return can_layer_send(cl, GB_ID_CST, d, 4);
}

/** 发送 CSD 充电机统计数据报文（标准表 27：8 字节）
 *   B1-B2 累计充电时间 1 min/位，0 偏移，0~600
 *   B3-B4 输出能量     0.1 kWh/位，0 偏移，0~1000
 *   B5-B8 充电机编号   1/位，**1 偏移**，0~0xFFFFFFFF
 *  ★ 编号是"1 偏移"：协议里发的是 id + 1，收到 0 表示无效。 */
static int send_csd(can_layer_t *cl, uint32_t charge_minutes,
                    uint32_t energy_x10, uint32_t charger_id)
{
    uint8_t  d[CSD_LEN];
    uint32_t id_off = gb27930_csd_encode_id(charger_id);   /* SPN3613 的 1 偏移 */

    if (charge_minutes > CSD_MAX_MINUTES)   { charge_minutes = CSD_MAX_MINUTES; }
    if (energy_x10 > CSD_MAX_ENERGY_X10)    { energy_x10 = CSD_MAX_ENERGY_X10; }

    le16_put(&d[CSD_OFF_MINUTES], (uint16_t)charge_minutes);
    le16_put(&d[CSD_OFF_ENERGY], (uint16_t)energy_x10);
    d[CSD_OFF_CHARGER_ID + 0] = (uint8_t)(id_off & 0xFFu);
    d[CSD_OFF_CHARGER_ID + 1] = (uint8_t)((id_off >> 8) & 0xFFu);
    d[CSD_OFF_CHARGER_ID + 2] = (uint8_t)((id_off >> 16) & 0xFFu);
    d[CSD_OFF_CHARGER_ID + 3] = (uint8_t)((id_off >> 24) & 0xFFu);

    return can_layer_send(cl, GB_ID_CSD, d, CSD_LEN);
}

/** 发送 CCS 充电机充电状态报文（标准表 19：**8 字节，周期 50 ms**）
 *   B1-B2 SPN3081 电压输出值    0.1 V/位，0 偏移
 *   B3-B4 SPN3082 电流输出值    0.1 A/位，**-400 A 偏移**
 *   B5-B6 SPN3083 累计充电时间  1 min/位，0~600（超 600 按 600 发）
 *   B7.1  SPN3929 充电允许      00 暂停 / 01 允许
 *   B7.3-7.8 与第 8 字节标准未定义 → 按 7.9 填 1
 *
 * 【为什么必须发】标准原文：「如果 BMS 在 **1 s** 内没有收到该报文，即为超时
 *   错误，BMS 应立即结束充电。」本工程原先完全没发 CCS，对端的 1 s 判据
 *   必然触发 —— 这是"按国标实现"在充电阶段的一个硬缺口，本次补上。 */
static int send_ccs(can_layer_t *cl, float out_v, float out_a,
                    uint32_t minutes, int permit)
{
    uint8_t d[CCS_LEN];

    fill_reserved(d, sizeof(d));   /* B7.3-7.8 与 B8 未定义 → 填 1 */

    le16_put(&d[CCS_OFF_OUT_V], encode_u16(out_v, L_RES_V_0_1, 0.0f));
    /* ★ 电流字段带 -400 A 偏移 */
    le16_put(&d[CCS_OFF_OUT_I], encode_u16(out_a, L_RES_I_0_1, L_OFF_I_400));

    if (minutes > CCS_MAX_MINUTES)
    {
        minutes = CCS_MAX_MINUTES;
    }
    le16_put(&d[CCS_OFF_MINUTES], (uint16_t)minutes);

    /* B7：低两位是 SPN3929，其余按 7.9 保持填 1 */
    d[CCS_OFF_PERMIT] = (uint8_t)((d[CCS_OFF_PERMIT] & (uint8_t)~(uint8_t)(0x03u << CCS_PERMIT_SHIFT))
                                | (uint8_t)(((permit ? CCS_OUTPUT_ALLOW : CCS_OUTPUT_PAUSE) & 0x03u)
                                            << CCS_PERMIT_SHIFT));

    return can_layer_send(cl, GB_ID_CCS, d, CCS_LEN);
}

/** 发送 CEM 充电机错误报文（标准表 29：4 字节，周期 250 ms，优先权 2）
 * @param err_mask 要上报的错误类别位掩码（CEM_ERR_* 按位或）
 *
 *   B1.1  SPN3921 接收 BMS 和车辆的辨识报文超时
 *   B2.1  SPN3922 接收电池充电参数报文超时
 *   B2.3  SPN3923 接收 BMS 完成充电准备报文超时
 *   B3.1  SPN3924 接收电池充电总状态报文超时
 *   B3.3  SPN3925 接收电池充电要求报文超时
 *   B3.5  SPN3926 接收 BMS 中止充电报文超时
 *   B4.1  SPN3927 接收 BMS 充电统计报文超时
 *   B4.3  其他（6 位，可选项）→ 按 7.9 填 1
 *
 * 枚举统一 00 正常 / 01 超时 / 10 不可信状态。本端只区分"超时"与"正常":
 * 每一类错误都由"规定时间内没收到 BMS 报文"触发，不存在"收到了但不可信"
 * 的判据，所以**不填 10 冒充**。 */
static int send_cem(can_layer_t *cl, uint8_t err_mask)
{
    uint8_t d[CEM_LEN];

    fill_reserved(d, sizeof(d));
    gb27930_cem_pack(d, err_mask);

    return can_layer_send(cl, GB_ID_CEM, d, CEM_LEN);
}

/*------------------------------------------------------------------------------
 * J1939 TP 接收侧应答（充电机作为接收方必须回 CTS / EndOfMsgACK）
 *
 *   BMS  -> 充电机  TP.CM 0x1CEC56F4 : 10 29 00 06 FF FF FF FF   (RTS, PGN=0x0200)
 *   充电机 -> BMS   TP.CM 0x1CECF456 : 11 FF 01 00 00 FF FF FF   (CTS, 允许全部包)
 *   BMS  -> 充电机  TP.DT 0x1CEB56F4 : 01 ... 06 ...             (6 包数据)
 *   充电机 -> BMS   TP.CM 0x1CECF456 : 13 29 00 06 FF 00 02 00   (EndOfMsgACK)
 *
 * 若不回 CTS，BMS 侧的 TP 状态机将等到 1 s 超时并重试，最终导致 BRM 传输失败，
 * 因此这两个应答是国标长报文联调能否跑通的关键。
 *---------------------------------------------------------------------------*/

/**
 * @brief  发送 CTS（允许发送）流控帧
 * @param  bs        一次允许发送的包数，0xFF 表示不限制
 * @param  next_seq  要求对端从第几包开始发（1 基）
 * @param  interval_ms 包间隔，0 表示尽可能快
 */
static int send_tp_cts(can_layer_t *cl, uint8_t bs, uint8_t next_seq, uint16_t interval_ms)
{
    uint8_t d[8];

    d[0] = 0x11;                       /* CTS */
    d[1] = bs;
    d[2] = next_seq;
    d[3] = (uint8_t)(interval_ms & 0xFF);        /* 小端 */
    d[4] = (uint8_t)((interval_ms >> 8) & 0xFF);
    d[5] = 0xFF;
    d[6] = 0xFF;
    d[7] = 0xFF;

    return can_layer_send(cl, GB_ID_CRM_TPCM, d, 8);
}

/**
 * @brief  发送 EndOfMsgACK（报文结束应答）
 * @param  total   收到的总字节数
 * @param  packets 总包数
 * @param  pgn     该多帧报文承载的应用层 PGN
 */
static int send_tp_ack(can_layer_t *cl, uint16_t total, uint8_t packets, uint32_t pgn)
{
    uint8_t d[8];

    d[0] = 0x13;                       /* EndOfMsgACK */
    d[1] = (uint8_t)(total & 0xFF);    /* 总字节数，小端 */
    d[2] = (uint8_t)((total >> 8) & 0xFF);
    d[3] = packets;
    d[4] = 0xFF;
    d[5] = (uint8_t)(pgn & 0xFF);      /* 目标 PGN，小端 24 位 */
    d[6] = (uint8_t)((pgn >> 8) & 0xFF);
    d[7] = (uint8_t)((pgn >> 16) & 0xFF);

    return can_layer_send(cl, GB_ID_CRM_TPCM, d, 8);
}

/**
 * @brief  发送 Abort（中止本次多帧传输）
 */
static int send_tp_abort(can_layer_t *cl, uint8_t reason, uint32_t pgn)
{
    uint8_t d[8];

    d[0] = 0xFF;
    d[1] = reason;
    d[2] = 0xFF;
    d[3] = 0xFF;
    d[4] = 0xFF;
    d[5] = (uint8_t)(pgn & 0xFF);
    d[6] = (uint8_t)((pgn >> 8) & 0xFF);
    d[7] = (uint8_t)((pgn >> 16) & 0xFF);

    return can_layer_send(cl, GB_ID_CRM_TPCM, d, 8);
}

/*==============================================================================
 * §8  解析（BMS 侧报文）
 *============================================================================*/

/** 解析 BHM 车辆握手报文（标准表 9：2 字节）
 *   B1-B2 最高允许充电总电压，0.1 V/位，0 偏移（小端）
 *  ★ 标准 BHM **没有版本号字段**。原实现把 B1 当主版本、B2 当次版本解析，
 *    是拿 CHM 的布局套到 BHM 上，已按标准改正。 */
static int parse_bhm(gb_context_t *ctx, const can_item_t *item)
{
    uint16_t raw;

    if (item->dlc < BHM_LEN)
    {
        ui_log("WARN", "BHM 长度不足: 期望 %d 字节，实收 %u 字节", BHM_LEN, item->dlc);
        return -1;
    }

    raw = le16_get(&item->data[0]);
    ctx->bhm_max_total_voltage = decode_u16(raw, L_RES_V_0_1, 0.0f);

    ui_log("GB", "收到 BHM 车辆握手: 最高允许充电总电压 %.1f V（原始值 %u, 0.1 V/位）",
           ctx->bhm_max_total_voltage, raw);

    ctx->has_bhm = 1;
    return 0;
}

/** 解析 BRM BMS 辨识报文（41 字节，已完成多帧重组） */
static int parse_brm(gb_context_t *ctx, const uint8_t *data, uint16_t len)
{
    gb_brm_t *b = &ctx->brm;

    if (len < BRM_LEN)
    {
        ui_log("WARN", "BRM 长度不足: 期望 %d 字节，实收 %u 字节", BRM_LEN, len);
        return -1;
    }

    memset(b, 0, sizeof(*b));

    /* B1-B3 BMS 通信协议版本号（小端 24 位，V1.1 = 0x000101）
     * 标准原文：V1.1 = byte3,byte2—0001H；byte1—01H ⇒ 数据域 `01 01 00`。
     * ★ 必须收全 3 个字节：只取 2 字节会把最高字节 0x00 截掉，
     *   读出来变成 0x0101 —— 那不是"版本号解析错误"，是**取值宽度不够**。 */
    b->version = (uint32_t)data[BRM_OFF_VERSION]
               | ((uint32_t)data[BRM_OFF_VERSION + 1] << 8)
               | ((uint32_t)data[BRM_OFF_VERSION + 2] << 16);
    /* B4 电池类型 */
    b->battery_type = data[BRM_OFF_BATT_TYPE];
    /* B5-B6 额定容量 0.1 Ah/位 */
    b->rated_capacity = decode_u16(le16_get(&data[BRM_OFF_RATED_CAP]), L_RES_CAP_0_1, 0.0f);
    /* B7-B8 额定总电压 0.1 V/位 */
    b->rated_voltage  = decode_u16(le16_get(&data[BRM_OFF_RATED_V]),   L_RES_V_0_1,   0.0f);
    /* B9-B12 生产厂商 4 字节标准 ASCII */
    memcpy(b->manufacturer, &data[BRM_OFF_MANUFACTURER], BRM_LEN_MANUFACTURER);
    b->manufacturer[BRM_LEN_MANUFACTURER] = '\0';
    /* B13-B16 电池组序号（预留，厂商自定义） */
    memcpy(b->pack_serial, &data[BRM_OFF_PACK_SERIAL], BRM_LEN_PACK_SERIAL);
    /* B17 生产年份：1 年/位，**1985 年偏移** */
    b->produce_year = (uint16_t)(data[BRM_OFF_PROD_YEAR] + BRM_YEAR_OFFSET);
    /* B18 月份, B19 日 */
    b->produce_month = data[BRM_OFF_PROD_MONTH];
    b->produce_day   = data[BRM_OFF_PROD_DAY];
    /* B20-B22 电池组充电次数（小端 3 字节） */
    b->charge_count = le24_get(&data[BRM_OFF_CHARGE_CNT]);
    /* B23 电池组产权标识：0 租赁 / 1 车自有 */
    b->ownership = data[BRM_OFF_OWNERSHIP];
    /* B24 预留 */
    b->reserved24 = data[BRM_OFF_RESERVED];
    /* B25-B41 车辆识别码 VIN 17 字节 ASCII */
    memcpy(b->vin, &data[BRM_OFF_VIN], BRM_LEN_VIN);
    b->vin[BRM_LEN_VIN] = '\0';
    b->raw_len = len;

    ctx->has_brm = 1;

    ui_log("GB", "收到 BRM BMS辨识: 协议版本 V%u.%u 厂商[%s] VIN[%s] "
                 "额定 %.1fV/%.1fAh 类型=%u 生产 %04u-%02u-%02u 循环 %u 次 产权=%u",
           (unsigned)((b->version >> 8) & 0xFFu), (unsigned)(b->version & 0xFFu),
           b->manufacturer, b->vin, b->rated_voltage, b->rated_capacity,
           b->battery_type, b->produce_year, b->produce_month, b->produce_day,
           b->charge_count, b->ownership);

    return 0;
}

/** 解析 BCP 动力蓄电池充电参数（13 字节，已完成多帧重组） */
static int parse_bcp(gb_context_t *ctx, const uint8_t *data, uint16_t len)
{
    gb_bcp_t *c = &ctx->bcp;

    if (len < BCP_LEN)
    {
        ui_log("WARN", "BCP 长度不足: 期望 %d 字节，实收 %u 字节", BCP_LEN, len);
        return -1;
    }

    memset(c, 0, sizeof(*c));

    c->max_single_voltage = decode_u16(le16_get(&data[BCP_OFF_MAX_CELL_V]), L_RES_V_0_01, 0.0f);
    c->max_current        = decode_u16(le16_get(&data[BCP_OFF_MAX_I]),      L_RES_I_0_1, L_OFF_I_400);
    c->nominal_energy     = decode_u16(le16_get(&data[BCP_OFF_ENERGY]),     L_RES_ENERGY_0_1, 0.0f);
    c->max_total_voltage  = decode_u16(le16_get(&data[BCP_OFF_MAX_TOTAL_V]),L_RES_V_0_1, 0.0f);
    c->max_temperature    = decode_u16((uint16_t)data[BCP_OFF_MAX_TEMP],    L_RES_T_1,   L_OFF_T_50);
    /* ★ B10-B11 SOC 的分辨率是 **0.1 %/位**（标准表 12），
     *   原先误用 L_RES_V_0_1（0.1 V/位）—— 数值上同为 0.1，看着"对"，
     *   但语义与后续 BCS 的 1 %/位 容易混淆，此处按标准显式用 SOC 常量。 */
    c->soc                = decode_u16(le16_get(&data[BCP_OFF_SOC]),        L_RES_SOC_0_1, 0.0f);
    c->current_voltage    = decode_u16(le16_get(&data[BCP_OFF_CUR_V]),      L_RES_V_0_1, 0.0f);

    clock_gettime(CLOCK_REALTIME, &c->ts);
    ctx->has_bcp = 1;

    ui_log("GB", "收到 BCP 充电参数: 单体上限 %.2fV 电流上限 %.1fA 标称能量 %.1fkWh "
                 "总压上限 %.1fV 温度上限 %.0f℃ SOC %.1f%%（0.1%%/位） 当前电压 %.1fV",
           c->max_single_voltage, c->max_current, c->nominal_energy,
           c->max_total_voltage, c->max_temperature, c->soc, c->current_voltage);

    return 0;
}

/** 解析 BCL 电池充电需求（标准表 17：**5 字节**）
 *   B1-B2 SPN3072 电压需求 0.1 V/位, 0 偏移
 *   B3-B4 SPN3073 电流需求 0.1 A/位, -400 A 偏移
 *   B5    SPN3074 充电模式 0x01 恒压 / 0x02 恒流
 *  ★ 标准 BCL 没有"允许充电电压/电流"字段（那属于 BCP），
 *    原先在 B6-B8 解析这两个量，已按标准删除。 */
static int parse_bcl(gb_context_t *ctx, const can_item_t *item)
{
    gb_bcl_t *c = &ctx->bcl;
    const uint8_t *d = item->data;

    if (item->dlc < BCL_LEN)
    {
        ui_log("WARN", "BCL 长度不足: 期望 %d 字节，实收 %u 字节", BCL_LEN, item->dlc);
        return -1;
    }

    c->voltage_demand = decode_u16(le16_get(&d[BCL_OFF_V_DEMAND]), L_RES_V_0_1, 0.0f);
    c->current_demand = decode_u16(le16_get(&d[BCL_OFF_I_DEMAND]), L_RES_I_0_1, L_OFF_I_400);
    c->charge_mode    = d[BCL_OFF_MODE];

    clock_gettime(CLOCK_REALTIME, &c->ts);

    if (!ctx->has_bcl)
    {
        ui_log("GB", "收到 BCL 电池充电需求: 需求 %.1fV / %.1fA 模式=%u(%s)",
               c->voltage_demand, c->current_demand, c->charge_mode,
               (c->charge_mode == 0x01) ? "恒压" :
               (c->charge_mode == 0x02) ? "恒流" : "未知");
    }
    ctx->has_bcl = 1;
    return 0;
}

/** 解析 BCS 电池充电总状态 */
static int parse_bcs(gb_context_t *ctx, const can_item_t *item)
{
    gb_bcs_t *c = &ctx->bcs;
    const uint8_t *d = item->data;

    if (item->dlc < BCS_LEN)
    {
        ui_log("WARN", "BCS 长度不足: 期望 %d 字节，实收 %u 字节", BCS_LEN, item->dlc);
        return -1;
    }

    c->measure_voltage    = decode_u16(le16_get(&d[BCS_OFF_MEASURE_V]),  L_RES_V_0_1,  0.0f);
    c->measure_current    = decode_u16(le16_get(&d[BCS_OFF_MEASURE_I]),  L_RES_I_0_1,  L_OFF_I_400);

    /* B5-B6 SPN3077：最高单体电压**及其组号**，是位打包字段。
     *   bit 0-11 = 电压（0.01 V/位），bit 12-15 = 所在组号（1/位） */
    {
        uint16_t raw = le16_get(&d[BCS_OFF_CELL_V_GROUP]);

        c->max_single_voltage = decode_u16((uint16_t)(raw & BCS_CELL_V_MASK),
                                           L_RES_V_0_01, 0.0f);
        c->max_single_group   = (uint8_t)((raw >> BCS_CELL_GROUP_SHIFT) & BCS_CELL_GROUP_MASK);
        c->group_number_valid = 1;
    }

    /* B7 当前荷电状态 SOC（**1 %/位**，与 BCP 的 0.1 %/位不同） */
    c->current_soc = decode_u16((uint16_t)d[BCS_OFF_SOC], L_RES_SOC_1, 0.0f);

    /* B8-B9 估算剩余充电时间（1 min/位，0~600）
     * 【已知偏差】本工程按 8 字节单帧发送，只有 B8（低字节），缺 B9。 */
    /* B8 估算剩余充电时间（1 min/位，0~600，超 600 按 600 发）
     * 【已知偏差】本工程按 8 字节单帧发送，只有 B8（低字节），缺 B9。
     * 【取值】本工程没有剩余充电时间估算能力，按标准 7.9「未规定的位或字段
     *   填充 1」发 0xFF，表示该项未规定 / 无有效值。
     *   **刻意不换算成分钟数** —— 600 是"至少还要 600 分钟"的溢出上限，
     *   把 0xFF 换算成 255 min 同样是在编造一个具体数值。 */
    c->remain_charge_raw     = d[BCS_OFF_REMAIN_LO];
    c->remain_charge_absent  = (d[BCS_OFF_REMAIN_LO] == GB_BCS_REMAIN_ABSENT) ? 1 : 0;
    c->remain_charge_lo_byte = 1;

    clock_gettime(CLOCK_REALTIME, &c->ts);
    ctx->has_bcs = 1;
    return 0;
}

/** 解析 BSM 动力蓄电池状态信息（标准表 20：**7 字节**）
 *   ★ 标准 BSM 里**没有电压字段**，B1 只是"最高单体电压所在编号"。
 *   B1    最高单体电压所在编号 1/位, **1 偏移**（协议值 0 = 无效，1 = 第 1 号）
 *   B2    最高温度  1 ℃/位, -50 ℃ 偏移
 *   B3    最高温度检测点编号 1/位, 1 偏移
 *   B4    最低温度  1 ℃/位, -50 ℃ 偏移
 *   B5    最低温度检测点编号 1/位, 1 偏移
 *   B6/B7 6 个 2 位状态字段 + 1 个未定义位（见 gb27930.h 的位段宏） */
static int parse_bsm(gb_context_t *ctx, const can_item_t *item)
{
    gb_bsm_t *c = &ctx->bsm;
    const uint8_t *d = item->data;

    if (item->dlc < BSM_LEN)
    {
        ui_log("WARN", "BSM 长度不足: 期望 %d 字节，实收 %u 字节", BSM_LEN, item->dlc);
        return -1;
    }

    c->max_single_voltage_no = d[BSM_OFF_CELL_V_NO];
    c->max_temp              = decode_u16((uint16_t)d[BSM_OFF_MAX_TEMP],    L_RES_T_1, L_OFF_T_50);
    c->max_temp_no           = d[BSM_OFF_MAX_TEMP_NO];
    c->min_temp              = decode_u16((uint16_t)d[BSM_OFF_MIN_TEMP],    L_RES_T_1, L_OFF_T_50);
    c->min_temp_no           = d[BSM_OFF_MIN_TEMP_NO];
    c->status_b6             = d[BSM_OFF_STATUS_B6];
    c->status_b7             = d[BSM_OFF_STATUS_B7];

    /* 把 2 位字段拆成可读值，后续 UI / 告警直接用，不必再自己做位运算 */
    c->cell_voltage_state = GB_FIELD2(c->status_b6, BSM_B6_CELL_V_SHIFT);
    c->soc_state          = GB_FIELD2(c->status_b6, BSM_B6_SOC_SHIFT);
    c->over_current_state = GB_FIELD2(c->status_b6, BSM_B6_OVER_I_SHIFT);
    c->over_temp_state    = GB_FIELD2(c->status_b6, BSM_B6_OVER_T_SHIFT);
    c->insulation_state   = GB_FIELD2(c->status_b7, BSM_B7_INSULATION_SHIFT);
    c->connector_state    = GB_FIELD2(c->status_b7, BSM_B7_CONNECTOR_SHIFT);
    c->charge_permit      = GB_FIELD2(c->status_b7, BSM_B7_PERMIT_SHIFT);

    clock_gettime(CLOCK_REALTIME, &c->ts);

    if (!ctx->has_bsm)
    {
        ui_log("GB", "收到 BSM 电池状态: 最高单体编号 #%u 温度 %.1f~%.1f℃ "
                     "状态 B6=0x%02X B7=0x%02X（电压状态 %u / SOC 状态 %u / 过流 %u / "
                     "过温 %u / 绝缘 %u / 连接器 %u / 充电允许 %u）",
               c->max_single_voltage_no, c->min_temp, c->max_temp,
               c->status_b6, c->status_b7,
               c->cell_voltage_state, c->soc_state, c->over_current_state,
               c->over_temp_state, c->insulation_state, c->connector_state,
               c->charge_permit);
    }
    ctx->has_bsm = 1;
    return 0;
}

/** 解析 BST BMS 中止充电报文（标准表 24：4 字节，全是 2 位字段）
 *   B1    中止原因 SPN3511（4 个 2 位字段）
 *   B2-B3 故障原因 SPN3512（8 个 2 位字段）
 *   B4    错误原因 SPN3513（2 个 2 位字段）
 *  ★ 原实现把 B3 当成"错误标志位"，实际 B4 才是 —— 已按标准改正。 */
static int parse_bst(gb_context_t *ctx, const can_item_t *item)
{
    gb_stop_t *s = &ctx->bst;

    memset(s, 0, sizeof(*s));
    s->reason_flags   = (item->dlc > 0) ? item->data[0] : 0;
    s->fault_flags_lo = (item->dlc > 1) ? item->data[1] : 0;
    s->fault_flags_hi = (item->dlc > 2) ? item->data[2] : 0;
    s->error_flags    = (item->dlc > 3) ? item->data[3] : 0;
    s->fault_flags    = (uint16_t)(((uint16_t)s->fault_flags_lo << 8) | s->fault_flags_hi);
    clock_gettime(CLOCK_REALTIME, &s->ts);

    ctx->has_bst = 1;

    ui_log("GB", "收到 BST BMS中止充电: B1 原因=0x%02X(达到SOC目标=%u 达到总压=%u "
                 "达到单体压=%u 充电机主动中止=%u) B2-B3 故障=0x%04X B4 错误=0x%02X",
           s->reason_flags,
           GB_FIELD2(s->reason_flags, GB_BST_R_SOC_SHIFT),
           GB_FIELD2(s->reason_flags, GB_BST_R_TOTAL_V_SHIFT),
           GB_FIELD2(s->reason_flags, GB_BST_R_CELL_V_SHIFT),
           GB_FIELD2(s->reason_flags, GB_BST_R_CHARGER_STOP_SHIFT),
           s->fault_flags, s->error_flags);

    return 0;
}

/** 解析 BSD BMS 统计数据（标准表 26：7 字节）
 *   B1    SPN3601 中止荷电状态 SOC，1 %/位
 *   B2-B3 SPN3602 单体**最低**电压，0.01 V/位
 *   B4-B5 SPN3603 单体**最高**电压，0.01 V/位
 *   B6    SPN3604 **最低**温度，1 ℃/位, -50 ℃ 偏移
 *   B7    SPN3605 **最高**温度，1 ℃/位, -50 ℃ 偏移
 *  ★ 原实现按"累计电量/累计时间/充电总电压"解析，与标准完全不符。 */
static int parse_bsd(gb_context_t *ctx, const can_item_t *item)
{
    gb_bsd_t *s = &ctx->bsd;
    const uint8_t *d = item->data;

    if (item->dlc < BSD_LEN)
    {
        ui_log("WARN", "BSD 长度不足: 期望 %d 字节，实收 %u 字节", BSD_LEN, item->dlc);
        return -1;
    }

    memset(s, 0, sizeof(*s));
    s->soc                 = d[BSD_OFF_SOC];
    s->min_single_voltage  = decode_u16(le16_get(&d[BSD_OFF_MIN_CELL_V]), L_RES_V_0_01, 0.0f);
    s->max_single_voltage  = decode_u16(le16_get(&d[BSD_OFF_MAX_CELL_V]), L_RES_V_0_01, 0.0f);
    s->min_temp            = decode_u16((uint16_t)d[BSD_OFF_MIN_TEMP],    L_RES_T_1, L_OFF_T_50);
    s->max_temp            = decode_u16((uint16_t)d[BSD_OFF_MAX_TEMP],    L_RES_T_1, L_OFF_T_50);
    clock_gettime(CLOCK_REALTIME, &s->ts);

    ui_log("GB", "收到 BSD BMS统计: 中止 SOC %u%% 单体 %.3f~%.3fV 温度 %.1f~%.1f℃",
           s->soc, s->min_single_voltage, s->max_single_voltage, s->min_temp, s->max_temp);

    return 0;
}

/** 解析 BEM BMS 错误报文（标准表 28：4 字节，优先权 2）
 *   每一个"错误类别"占 **2 位**，枚举 00 正常 / 01 超时 / 10 不可信状态。
 *   B4 的高 6 位是标准标注的可选项"其他"，不参与判定。
 * ★ 本工程只**解析并展示** BEM，不用它改变状态机走向：BEM 是 BMS 告诉
 *   充电机"我这边没收到什么"，充电机该做的是把对应报文补发/重新握手，
 *   而不是自己直接进故障态（那会形成两端互相甩锅）。
 *   BEM 一到就说明对端已经在超时了，本端把它记进上下文供界面/日志用。 */
static int parse_bem(gb_context_t *ctx, const can_item_t *item)
{
    gb_bem_t *b = &ctx->bem;
    const uint8_t *d = item->data;

    if (item->dlc < BEM_LEN)
    {
        ui_log("WARN", "BEM 长度不足: 期望 %d 字节，实收 %u 字节", BEM_LEN, item->dlc);
        return -1;
    }

    memset(b, 0, sizeof(*b));
    memcpy(b->raw, d, BEM_LEN);

    b->crm_00_timeout  = GB_FIELD2(d[BEM_OFF_B1], BEM_B1_CRM_00_SHIFT);
    b->crm_aa_timeout  = GB_FIELD2(d[BEM_OFF_B1], BEM_B1_CRM_AA_SHIFT);
    b->cts_cml_timeout = GB_FIELD2(d[BEM_OFF_B2], BEM_B2_CTS_CML_SHIFT);
    b->cro_timeout     = GB_FIELD2(d[BEM_OFF_B2], BEM_B2_CRO_SHIFT);
    b->ccs_timeout     = GB_FIELD2(d[BEM_OFF_B3], BEM_B3_CCS_SHIFT);
    b->cst_timeout     = GB_FIELD2(d[BEM_OFF_B3], BEM_B3_CST_SHIFT);
    b->csd_timeout     = GB_FIELD2(d[BEM_OFF_B4], BEM_B4_CSD_SHIFT);

    clock_gettime(CLOCK_REALTIME, &b->ts);

    if (!ctx->has_bem)
    {
        ui_log("GB", "收到 BEM BMS错误报文: [%02X %02X %02X %02X] "
                     "辨识(0x00)=%u 辨识(0xAA)=%u CTS/CML=%u CRO=%u "
                     "CCS=%u CST=%u CSD=%u（0 正常 / 1 超时 / 2 不可信）",
               d[0], d[1], d[2], d[3],
               b->crm_00_timeout, b->crm_aa_timeout, b->cts_cml_timeout,
               b->cro_timeout, b->ccs_timeout, b->cst_timeout, b->csd_timeout);
    }
    ctx->has_bem = 1;
    return 0;
}

/** 解析 BRO 电池充电准备就绪（标准表 15：1 字节）
 *   0x00 = 未做好；0xAA = 完成准备；0xFF = 无效 */
static int parse_bro(gb_context_t *ctx, const can_item_t *item)
{
    uint8_t v = (item->dlc > 0) ? item->data[0] : 0;

    if (!ctx->has_bro)
    {
        ui_log("GB", "收到 BRO 电池充电准备就绪: 0x%02X (%s)",
               v, (v == GB_READY_READY) ? "完成准备" :
                  (v == GB_READY_INVALID) ? "无效" : "未做好");
    }
    ctx->has_bro = (v == GB_READY_READY) ? 1 : 0;
    return 0;
}

/*==============================================================================
 * §9  状态机
 *============================================================================*/

void gb27930_init(gb_context_t *ctx, const gb_callbacks_t *cb)
{
    if (ctx == NULL)
    {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));

    if (cb != NULL)
    {
        ctx->cb = *cb;
    }

    /* BRM 使用 J1939 传输协议：TP.CM 0xEC00 / TP.DT 0xEB00 */
    isotp_rx_init(&ctx->brm_rx, ISOTP_FMT_J1939TP,
                  GB_ID_BRM_TPCM, GB_ID_BRM_TPDT, T_ISOTP_MS);

    /* 发送侧 ISO-TP 上下文（预留：本工程充电机侧暂无长报文，
     * 但保留以便未来发送扩展报文） */
    isotp_tx_init(&ctx->tx, ISOTP_FMT_J1939TP,
                  GB_ID_CRM_TPCM, GB_ID_CRM_TPDT, T_ISOTP_MS);

    ctx->state           = GB_ST_IDLE;
    ctx->error           = GB_ERR_NONE;
    ctx->now_ms          = mono_ms();
    ctx->state_enter_ms  = ctx->now_ms;
    ctx->last_rx_ms      = ctx->now_ms;
    ctx->last_bcl_ms     = ctx->now_ms;
    ctx->last_bcs_ms     = ctx->now_ms;
    ctx->ready_since_ms  = ctx->now_ms;
    ctx->session_id      = 0;
    ctx->dump_mode       = 0;
    ctx->charger_id      = CHARGER_ID_VALUE;
    ctx->bcs_energy_last_ms = 0;
    ctx->next_ccs_ms     = ctx->now_ms;
    ctx->ccs_publish_ms  = ctx->now_ms;
    ctx->next_cem_ms     = ctx->now_ms;
    ctx->cem_mask        = 0;      /* 清掉上一轮的错误掩码，否则复位后接着发 CEM */
    ctx->cem_sent        = 0;
}

void gb27930_reset(gb_context_t *ctx)
{
    gb_callbacks_t cb;
    uint64_t now;
    uint64_t rx_msg;
    uint64_t parse_err;
    uint64_t unknown_id;

    if (ctx == NULL)
    {
        return;
    }

    cb  = ctx->cb;
    now = mono_ms();

    /* 统计量是「进程生命周期内」的累计值，不随单次充电会话复位而清零，
     * 否则会话结束（STOPPING -> 复位）后再看统计就全是 0 了。 */
    rx_msg     = ctx->rx_msg_count;
    parse_err  = ctx->parse_err_count;
    unknown_id = ctx->unknown_id_count;

    memset(ctx, 0, sizeof(*ctx));
    ctx->cb  = cb;
    ctx->now_ms         = now;
    ctx->state_enter_ms = now;
    ctx->last_rx_ms     = now;
    /* 【必须重置】超时基准全部抹成 0 会让 now - 0 变成一个巨大的差值，
     * 于是复位后第一拍就直接判定"BCL 超时"。统一置成 now。 */
    ctx->last_bcl_ms    = now;
    ctx->last_bcs_ms    = now;
    ctx->ready_since_ms = now;
    ctx->charger_id     = CHARGER_ID_VALUE;
    /* 能量积分器归零：新会话要重新计 */
    ctx->charge_energy_x10  = 0.0;
    ctx->bcs_energy_last_ms = 0;
    /* CCS / CEM 的新增状态也要清干净，否则复位后第一拍就会接着上一轮发 */
    ctx->next_ccs_ms   = now;
    ctx->ccs_publish_ms = now;
    ctx->cem_mask      = 0;
    ctx->cem_sent      = 0;
    ctx->next_cem_ms   = now;

    ctx->rx_msg_count     = rx_msg;
    ctx->parse_err_count  = parse_err;
    ctx->unknown_id_count = unknown_id;

    isotp_rx_init(&ctx->brm_rx, ISOTP_FMT_J1939TP,
                  GB_ID_BRM_TPCM, GB_ID_BRM_TPDT, T_ISOTP_MS);
    isotp_tx_init(&ctx->tx, ISOTP_FMT_J1939TP,
                  GB_ID_CRM_TPCM, GB_ID_CRM_TPDT, T_ISOTP_MS);

    ctx->state = GB_ST_IDLE;
    ctx->error = GB_ERR_NONE;

    ui_log("GB", "协议层已复位，回到空闲态，等待 BMS 上线（累计已解析 %llu 条报文）",
           (unsigned long long)rx_msg);
}

/*------------------------------------------------------------------------------
 * CST 故障原因字节的拼装助手（标准表 25：B2-B3 是 6 个 2 位字段）
 *
 * 两个字节合起来 16 位，SPN3522 的 6 个字段占最高的 12 位（2-12 位），
 * 最低 4 位（B3.5-3.8）标准未定义 —— 按 7.9 填 1。
 * 每个字段 2 位，取值用 GB_ST_NORMAL / GB_ST_ABNORMAL / GB_ST_UNRELIABLE。
 *---------------------------------------------------------------------------*/

/** 把 2 位值写进 16 位故障字（shift 见 gb27930.h 的 GB_CST_F_*_SHIFT） */
static inline uint16_t cst_fault_set(uint16_t word, uint8_t shift, uint8_t val)
{
    word = (uint16_t)(word & (uint16_t)~(uint16_t)(0x0003u << shift));
    return (uint16_t)(word | (uint16_t)(((uint16_t)(val & 0x03u)) << shift));
}

/** 未定义的最低 4 位按 7.9 填 1 */
#define CST_FAULT_UNDEFINED     0x000Fu

/** 进入故障态并发送 CST
 *  @param cst_reason  SPN3521 中止原因字节（4 个 2 位字段，见 GB_CST_R_*_SHIFT）
 *  @param fault_word  SPN3522 故障原因 16 位字（见 GB_CST_F_*_SHIFT）
 *  @param cst_error   SPN3523 错误原因字节（2 个 2 位字段，见 GB_CST_E_*_SHIFT）
 *  @param cem_mask    SPN3921~3927 的 CEM 错误掩码（CEM_ERR_* 按位或，0 = 不报）
 *
 * 【关于 cem_mask】为什么做成参数而不是在函数里按 error 猜：
 *   同一个 gb_error_t 在不同阶段对应的 CEM 字段**不一样**。最典型的是
 *   GB_ERR_PARAM_TIMEOUT —— 参数配置阶段收不到 BCP 是 SPN3922，
 *   而充电准备阶段收不到 BRO 是 SPN3923。让调用点（它知道自己在哪个阶段）
 *   显式给掩码，比在函数里读 ctx->state 推断更可靠，也不会有维护时的隐式耦合。
 *   CEM 会在 GB_ST_FAULT 态里按 250 ms 周期连发，最多 T_CEM_MAX_TX 次。 */
static void enter_fault(gb_context_t *ctx, can_layer_t *cl, gb_error_t err,
                        uint8_t cst_reason, uint16_t fault_word, uint8_t cst_error,
                        uint8_t cem_mask, const char *detail)
{
    raise_error(ctx, err, detail);

    /* 记下本轮要上报的错误类别并复位发送计数（每次故障只连发一轮） */
    ctx->cem_mask  = cem_mask;
    ctx->cem_sent  = 0;
    ctx->next_cem_ms = ctx->now_ms;

    if (cl != NULL)
    {
        uint16_t w = (uint16_t)((fault_word & (uint16_t)~CST_FAULT_UNDEFINED)
                                | CST_FAULT_UNDEFINED);

        (void)send_cst(cl, cst_reason,
                       (uint8_t)((w >> 8) & 0xFFu),
                       (uint8_t)(w & 0xFFu),
                       cst_error);
    }
    set_state(ctx, GB_ST_FAULT);
}

/**
 * @brief  状态机主逻辑：根据当前状态发送报文、判定超时、推进迁移
 */
static void state_machine(gb_context_t *ctx, can_layer_t *cl)
{
    uint64_t now = ctx->now_ms;
    uint64_t in_state_ms = now - ctx->state_enter_ms;
    int      dump = ctx->dump_mode;
    /* 【防重复进故障态】充电阶段的三条超时判据在条件仍然成立时会**每拍**
     * 都调用 enter_fault()。set_state() 在 old == new_state 时直接 return，
     * 于是只要 CST 没发出去（send_cst 失败 / cl == NULL）就会反复重发 CST、
     * 日志被刷屏。用这个标志保证一个 tick 内只进一次故障态。 */
    int      faulted = 0;

    switch (ctx->state)
    {
        /*--------------------------------------------------------------
         * 空闲态：等待 BMS 接入（真实桩中此处等待低压辅助上电 + 绝缘检测）
         * 本工程直接周期发送 CHM，开始握手
         *------------------------------------------------------------*/
        case GB_ST_IDLE:
            /* 待机：**不自动开始充电**。
             * 上电后只做界面显示和基础日志，等用户在 i.MX 上按「充电」
             * （相当于把充电枪插上）才往下走。 */
            if (!ctx->start_req)
            {
                /* 保持空闲计时不累积：按下按钮后 1 秒内就会进握手。 */
                break;
            }

            if (!dump && now >= ctx->next_chm_ms)
            {
                GB_SCHED_NEXT(ctx->next_chm_ms, now, T_CHM_PERIOD_MS);
                (void)send_chm(cl);
            }
            if (in_state_ms >= 1000u)   /* 按下后 1 秒进入握手阶段 */
            {
                /* 这行日志专门用来定位"停了又自己充上"：
                 * 正常只应该在用户按「充电」之后出现一次。 */
                ui_log("GB", "开始握手（收到过开始请求，start_req=1）");
                ctx->session_id++;
                ctx->session_start_ms = now;
                ctx->start_req = 0;     /* 消费掉这次请求 */
                ui_log("GB", "开始新的充电会话 #%u，进入握手阶段", ctx->session_id);
                set_state(ctx, GB_ST_HANDSHAKE);
            }
            break;

        /*--------------------------------------------------------------
         * 握手阶段：周期发 CHM(250ms)，等待 BHM
         * 超时：5 s 未收到 BHM → 故障
         *------------------------------------------------------------*/
        case GB_ST_HANDSHAKE:
            if (!dump && now >= ctx->next_chm_ms)
            {
                GB_SCHED_NEXT(ctx->next_chm_ms, now, T_CHM_PERIOD_MS);
                (void)send_chm(cl);
            }
            if (ctx->has_bhm)
            {
                set_state(ctx, GB_ST_IDENTIFY);
            }
            else if (in_state_ms > T_HANDSHAKE_MS)
            {
                enter_fault(ctx, cl, GB_ERR_HANDSHAKE_TIMEOUT,
                            GB_FIELD2_SET(0u, GB_CST_R_FAULT_SHIFT, GB_ST_ABNORMAL),
                            cst_fault_set(0u, GB_CST_F_OTHER_SHIFT, GB_ST_ABNORMAL),
                            GB_FIELD2_SET(0u, GB_CST_E_VOLTAGE_SHIFT, GB_ST_NORMAL),
                            CEM_ERR_IDENT,
                            "5 秒内未收到 BHM 车辆握手报文（通用超时 5 s）");
            }
            break;

        /*--------------------------------------------------------------
         * 辨识阶段：周期发 CRM(250ms)，等待 BRM（多帧重组）
         * 超时：5 s 未收到 BRM → 故障
         *------------------------------------------------------------*/
        case GB_ST_IDENTIFY:
            if (!dump && now >= ctx->next_crm_ms)
            {
                GB_SCHED_NEXT(ctx->next_crm_ms, now, T_CRM_PERIOD_MS);
                (void)send_crm(cl);
            }
            if (ctx->has_brm)
            {
                set_state(ctx, GB_ST_PARAM_CONFIG);
            }
            else if (in_state_ms > T_IDENTIFY_MS)
            {
                enter_fault(ctx, cl, GB_ERR_IDENTIFY_TIMEOUT,
                            GB_FIELD2_SET(0u, GB_CST_R_FAULT_SHIFT, GB_ST_ABNORMAL),
                            cst_fault_set(0u, GB_CST_F_OTHER_SHIFT, GB_ST_ABNORMAL),
                            GB_FIELD2_SET(0u, GB_CST_E_VOLTAGE_SHIFT, GB_ST_NORMAL),
                            CEM_ERR_IDENT,
                            "5 秒内未收到 BRM 辨识报文（通用超时 5 s）");
            }
            break;

        /*--------------------------------------------------------------
         * 参数配置阶段：等待 BCP（多帧），同时下发 CTS / CML
         * 超时：5 s 未收到 BCP → 故障
         *------------------------------------------------------------*/
        case GB_ST_PARAM_CONFIG:
            if (!dump)
            {
                if (now >= ctx->next_cts_ms)
                {
                GB_SCHED_NEXT(ctx->next_cts_ms, now, T_CTS_PERIOD_MS);
                    (void)send_cts(cl);
                }
                if (now >= ctx->next_cml_ms)
                {
                    GB_SCHED_NEXT(ctx->next_cml_ms, now, T_CML_PERIOD_MS);
                    (void)send_cml(cl);
                }
                /* 握手/辨识阶段已结束，停止 CHM/CRM 周期发送 */
                GB_SCHED_NEXT(ctx->next_chm_ms, now, T_CHM_PERIOD_MS);
                GB_SCHED_NEXT(ctx->next_crm_ms, now, T_CRM_PERIOD_MS);
            }
            if (ctx->has_bcp)
            {
                ui_log("GB", "参数配置完成，等待 BMS 充电准备就绪（BRO）");
                set_state(ctx, GB_ST_CHARGING_READY);
            }
            else if (in_state_ms > T_PARAM_MS)
            {
                enter_fault(ctx, cl, GB_ERR_PARAM_TIMEOUT,
                            GB_FIELD2_SET(0u, GB_CST_R_FAULT_SHIFT, GB_ST_ABNORMAL),
                            cst_fault_set(0u, GB_CST_F_OTHER_SHIFT, GB_ST_ABNORMAL),
                            GB_FIELD2_SET(0u, GB_CST_E_VOLTAGE_SHIFT, GB_ST_NORMAL),
                            CEM_ERR_BCP,
                            "5 秒内未收到 BCP 充电参数（标准：BCP 超时 5 s）");
            }
            break;

        /*--------------------------------------------------------------
         * 充电准备阶段：周期发送 CRO（先发未就绪 0x00），
         * 收到 BRO=0xAA 后把 CRO 切换为就绪 0xAA，并进入充电阶段
         *
         * 超时：**标准原文是 60 s，不是 5 s**
         *   p.11「BMS 在 60 s 内未准备好，则充电机进行等待」
         *   p.11「充电机在 60 s 内未准备好，则 BMS 进行等待」
         *   60 s 之内属于正常等待，超过 60 s 才算超时错误。
         *------------------------------------------------------------*/
        case GB_ST_CHARGING_READY:
            if (!dump)
            {
                if (now >= ctx->next_cml_ms)
                {
                    GB_SCHED_NEXT(ctx->next_cml_ms, now, T_CML_PERIOD_MS);
                    (void)send_cml(cl);
                }
                if (now >= ctx->next_cro_ms)
                {
                GB_SCHED_NEXT(ctx->next_cro_ms, now, T_CRO_PERIOD_MS);
                    (void)send_ready(cl, GB_ID_CRO, ctx->has_bro ? 1 : 0);
                }
            }
            if (ctx->has_bro)
            {
                ui_log("GB", "充电机输出准备就绪，进入充电阶段（K1/K2 闭合）");
                ctx->last_rx_ms  = now;   /* 重置充电阶段超时基准 */
                ctx->last_bcl_ms = now;
                ctx->last_bcs_ms = now;
                set_state(ctx, GB_ST_CHARGING);
            }
            else if ((now - ctx->ready_since_ms) > T_READY_MS)
            {
                enter_fault(ctx, cl, GB_ERR_READY_TIMEOUT,
                            GB_FIELD2_SET(0u, GB_CST_R_FAULT_SHIFT, GB_ST_ABNORMAL),
                            cst_fault_set(cst_fault_set(0u, GB_CST_F_CONNECTOR_SHIFT, GB_ST_ABNORMAL),
                                          GB_CST_F_OTHER_SHIFT, GB_ST_NORMAL),
                            GB_FIELD2_SET(0u, GB_CST_E_VOLTAGE_SHIFT, GB_ST_NORMAL),
                            CEM_ERR_BRO,
                            "60 秒内未收到 BRO=0xAA 充电准备就绪（标准：60 s）");
            }
            break;

        /*--------------------------------------------------------------
         * 充电阶段：周期发送 CCS(50 ms) 与 CRO(250 ms)，持续接收 BCL/BCS/BSM
         *
         * 【CCS 是本工程原先完全缺失的报文】标准表 19 规定它由充电机以
         *   **50 ms** 周期发出，而标准同时规定：「如果 BMS 在 **1 s** 内没有
         *   收到该报文，即为超时错误，BMS 应立即结束充电。」
         *   不发 CCS ⇒ 对端的 1 s 判据必然触发 ⇒ 充电根本走不完。
         *   CCS 的电压/电流直接用 BCS 收到的实测值回显（充电机的输出就是
         *   它自己在做的动作），累计时间用会话开始到现在的分钟数，
         *   充电允许恒为 01（本端在充电阶段不会主动停输出）。
         *
         * 【逐报文超时，标准是**每条不同**的，不能共用一个 5 s】
         *   BCL 标准 p.12：如果充电机在 **1 s** 内没有收到该报文，即为超时
         *                  错误，充电机应立即结束充电。
         *   BCS 标准 p.12：如果充电机在 **5 s** 内没有收到该报文，即为超时错误。
         *   CCS 标准 p.13：1 s（本端作为**发送方**，只需按 50 ms 发即可）。
         *   其余按第 8 章通用 5 s。
         *------------------------------------------------------------*/
        case GB_ST_CHARGING:
            if (!dump && now >= ctx->next_ccs_ms)
            {
                /* 【先声明再语句】gnu11 允许混写，但本文件其余部分一律
                 * "块首声明"，保持一致，避免以后被 -Wdeclaration-after-statement 挑出来。 */
                float out_v;
                float out_i;

                /* CCS 的输出值直接用 BCS 收到的实测值回显（充电机的输出就是
                 * 它自己在做的动作）。若此刻还没收到 BCS，用 BCP 的当前电池
                 * 电压兜底；两者都没有才发 0 —— 那种情况本来就进不了本状态
                 * （上一状态要求收到 BRO，充电阶段又要求 BCS 在 5 s 内刷新）。 */
                out_v = ctx->bcs.measure_voltage;
                out_i = ctx->bcs.measure_current;

                if (!ctx->has_bcs)
                {
                    out_v = ctx->has_bcp ? ctx->bcp.current_voltage : 0.0f;
                    out_i = 0.0f;
                }

                GB_SCHED_NEXT(ctx->next_ccs_ms, now, T_CCS_PERIOD_MS);
                (void)send_ccs(cl, out_v, out_i,
                               (uint32_t)((now - ctx->session_start_ms) / 60000u),
                               1);   /* SPN3929 = 01 允许，继续充电 */
            }

            if (!dump && now >= ctx->next_cro_ms)
            {
                GB_SCHED_NEXT(ctx->next_cro_ms, now, T_CRO_PERIOD_MS);
                (void)send_ready(cl, GB_ID_CRO, 1);
            }

            /* 收到 BST 说明 BMS 主动中止充电 */
            if (ctx->has_bst)
            {
                ui_log("GB", "BMS 请求停止充电，进入结束阶段");
                set_state(ctx, GB_ST_STOPPING);
                break;
            }

            /* (1) BCL 专用 1 s 超时（标准原文：应立即结束充电） */
            if (!faulted && (now - ctx->last_bcl_ms) > T_BCL_MS)
            {
                faulted = 1;
                enter_fault(ctx, cl, GB_ERR_BCL_TIMEOUT,
                            GB_FIELD2_SET(0u, GB_CST_R_FAULT_SHIFT, GB_ST_ABNORMAL),
                            cst_fault_set(0u, GB_CST_F_OTHER_SHIFT, GB_ST_ABNORMAL),
                            GB_FIELD2_SET(0u, GB_CST_E_CURRENT_SHIFT, GB_ST_UNRELIABLE),
                            CEM_ERR_BCL,
                            "1 秒内未收到 BCL 电池充电需求（标准原文：应立即结束充电）");
                break;
            }

            /* (2) BCS 专用 5 s 超时（标准原文 p.12） */
            if (!faulted && (now - ctx->last_bcs_ms) > T_BCS_MS)
            {
                faulted = 1;
                enter_fault(ctx, cl, GB_ERR_CHARGE_TIMEOUT,
                            GB_FIELD2_SET(0u, GB_CST_R_FAULT_SHIFT, GB_ST_ABNORMAL),
                            cst_fault_set(0u, GB_CST_F_OTHER_SHIFT, GB_ST_ABNORMAL),
                            GB_FIELD2_SET(0u, GB_CST_E_VOLTAGE_SHIFT, GB_ST_UNRELIABLE),
                            CEM_ERR_BCS,
                            "5 秒内未收到 BCS 电池充电总状态（标准：5 s）");
                break;
            }

            /* (3) 通用兜底：任意 BMS 报文都没有超过 5 s 才算链路活着 */
            if (!faulted && (now - ctx->last_rx_ms) > T_CHARGE_MS)
            {
                faulted = 1;
                enter_fault(ctx, cl, GB_ERR_CHARGE_TIMEOUT,
                            GB_FIELD2_SET(0u, GB_CST_R_FAULT_SHIFT, GB_ST_ABNORMAL),
                            cst_fault_set(0u, GB_CST_F_OTHER_SHIFT, GB_ST_ABNORMAL),
                            GB_FIELD2_SET(0u, GB_CST_E_VOLTAGE_SHIFT, GB_ST_NORMAL),
                            (uint8_t)(CEM_ERR_BCS | CEM_ERR_BCL),
                            "充电阶段 5 秒未收到任何 BMS 报文（通用超时 5 s）");
            }
            break;

        /*--------------------------------------------------------------
         * 结束阶段：发送 CST 与 CSD，等待 BSD，超时后回到空闲态
         *
         * 标准表 25 / 表 27：结束阶段充电机要发
         *   CST 充电机中止充电（4 字节，逐字段是 2 位枚举）
         *   CSD 充电机统计数据（8 字节：累计充电时间 / 输出能量 / 充电机编号）
         * 原实现只发 CST、且 reason 用的是旧的整字节掩码，也没有 CSD —— 已补齐。
         *------------------------------------------------------------*/
        case GB_ST_STOPPING:
            if (in_state_ms >= 1000u)
            {
                if (!dump)
                {
                    /* CST：SPN3521 的 1-2 位"达到充电机设定条件中止"= 01。
                     * B2-B3 故障原因按标准 7.9 填 1（未定义位），
                     * 也就是 6 个 2 位字段全部按"不可信"上报 —— 本工程不做
                     * 充电机侧故障检测，不冒充实测。 */
                    uint16_t fw = CST_FAULT_UNDEFINED;

                    fw = cst_fault_set(fw, GB_CST_F_CHARGER_OVERHEAT_SHIFT, GB_ST_UNRELIABLE);
                    fw = cst_fault_set(fw, GB_CST_F_CONNECTOR_SHIFT,        GB_ST_UNRELIABLE);
                    fw = cst_fault_set(fw, GB_CST_F_INNER_OVERHEAT_SHIFT,   GB_ST_UNRELIABLE);
                    fw = cst_fault_set(fw, GB_CST_F_ENERGY_SHIFT,           GB_ST_UNRELIABLE);
                    fw = cst_fault_set(fw, GB_CST_F_ESTOP_SHIFT,            GB_ST_UNRELIABLE);
                    fw = cst_fault_set(fw, GB_CST_F_OTHER_SHIFT,            GB_ST_NORMAL);

                    (void)send_cst(cl,
                                   GB_FIELD2_SET(0u, GB_CST_R_CHARGER_COND_SHIFT, GB_ST_ABNORMAL),
                                   (uint8_t)((fw >> 8) & 0xFFu),
                                   (uint8_t)(fw & 0xFFu),
                                   GB_FIELD2_SET(0u, GB_CST_E_VOLTAGE_SHIFT, GB_ST_NORMAL));
                }

                /* CSD：本会话的累计充电时长与输出能量。
                 * 时间用"会话开始到现在"的分钟数，能量按 BCS 电压电流积分，
                 * 两者都按标准上限（600 min / 1000）截断。 */
                if (!dump && !ctx->csd_sent && now >= ctx->next_csd_ms)
                {
                    uint32_t minutes = (uint32_t)((now - ctx->session_start_ms) / 60000u);
                    uint32_t energy  = (uint32_t)(ctx->charge_energy_x10 + 0.5);

                    ctx->csd_sent    = 1;
                GB_SCHED_NEXT(ctx->next_csd_ms, now, T_CSD_PERIOD_MS);

                    (void)send_csd(cl, minutes, energy, ctx->charger_id);
                    ui_log("GB", "已发送 CSD 充电机统计: 累计 %u min / %.1f kWh / 编号 %u",
                           minutes, energy / 10.0, ctx->charger_id);
                }

                if (in_state_ms >= 3000u)
                {
                    ui_log("GB", "本次充电会话 #%u 结束，回到空闲态",
                           ctx->session_id);
                    gb27930_reset(ctx);
                }
            }
            break;

        /*--------------------------------------------------------------
         * 故障态：周期发送 CEM 错误报文，等待人工复位（或调用 gb27930_reset）
         *
         * 【为什么把 CEM 放在这里】标准表 29 的 CEM 是充电机在"检测到 BMS
         *   报文超时"时上报的错误报文，周期 250 ms。本端把"检测到错误"落在
         *   **已有的超时分支**上（它们通过 enter_fault 的 cem_mask 参数告诉
         *   本状态该报哪个 SPN），故障态里按 250 ms 连发，最多 T_CEM_MAX_TX 次。
         *   转出故障态（自动复位 / 用户重新开始）后自然停止，不新建并行机制。
         *------------------------------------------------------------*/
        case GB_ST_FAULT:
            if (ctx->cem_mask != 0u && ctx->cem_sent < T_CEM_MAX_TX &&
                now >= ctx->next_cem_ms)
            {
                GB_SCHED_NEXT(ctx->next_cem_ms, now, T_CEM_PERIOD_MS);
                ctx->cem_sent++;

                if (!dump)
                {
                    (void)send_cem(cl, ctx->cem_mask);
                }
                if (ctx->cem_sent == 1u)
                {
                    ui_log("GB", "进入故障态，开始发送 CEM 错误报文"
                                 "（ID 0x%08X，优先权 2，错误掩码 0x%02X）",
                           GB_ID_CEM, ctx->cem_mask);
                }
            }

            if (in_state_ms > 10000u)
            {
                ui_log("GB", "故障态持续 10 秒，自动复位重试");
                gb27930_reset(ctx);
            }
            break;

        default:
            gb27930_reset(ctx);
            break;
    }
}

void gb27930_request_start(gb_context_t *ctx)
{
    if (ctx == NULL) { return; }

    /* 已经在充电流程里：忽略重复请求，不要打断正在进行的会话 */
    switch (ctx->state)
    {
        case GB_ST_HANDSHAKE:
        case GB_ST_IDENTIFY:
        case GB_ST_PARAM_CONFIG:
        case GB_ST_CHARGING_READY:
        case GB_ST_CHARGING:
            ui_log("GB", "已在充电流程中（%s），忽略「充电」请求",
                   gb27930_state_str(ctx->state));
            return;
        default:
            break;
    }

    /* 【重要】待机以外的状态（尤其是 FAULT 和 STOPPING）也要能重新开始。
     *
     * 原来这里写的是 "if (ctx->state != GB_ST_IDLE) return;" ——
     * 于是上一次充电超时进了 FAULT 之后，再按「充电」就被**静默忽略**，
     * 界面上表现为"按了没反应、一直显示异常"，只能等 FAULT 自己超时退出
     * （现场描述成"多试几次就又能成功充电"就是这个原因）。
     *
     * 现在改成：先把状态机复位回 IDLE，再置请求，按下就一定生效。 */
    /* 【互锁】刚停止的 5 秒内不接受"开始"请求。
     *
     * 现场现象：按「停止」后 STM32 确实停了一下，紧接着又自己充上了。
     * 唯一能造成这个的路径就是"停止之后又收到一次开始请求"
     * （触摸事件重复投递、或者状态刚好在 IDLE 被判成"没在充电"）。
     * 这里再兜一道 —— 即使上层漏了一次，协议层也不会立刻重启。 */
    if (ctx->stop_ms != 0 && (mono_ms() - ctx->stop_ms) < 5000u)
    {
        ui_log("GB", "刚停止 %.1f 秒，忽略这次「开始」请求（防连击）",
               (double)(mono_ms() - ctx->stop_ms) / 1000.0);
        return;
    }

    if (ctx->state != GB_ST_IDLE)
    {
        ui_log("GB", "从 %s 状态重新开始充电", gb27930_state_str(ctx->state));
        gb27930_reset(ctx);
    }

    ctx->start_req = 1;     /* 注意顺序：复位会清标志，所以放在后面 */
    ui_log("GB", "用户按下「充电」按钮，准备开始充电");
}

int gb27930_is_active(const gb_context_t *ctx)
{
    if (ctx == NULL) { return 0; }

    switch (ctx->state)
    {
        case GB_ST_HANDSHAKE:
        case GB_ST_IDENTIFY:
        case GB_ST_PARAM_CONFIG:
        case GB_ST_CHARGING_READY:
        case GB_ST_CHARGING:
            return 1;

        /* 【注意】STOPPING **不算** active。
         *
         * 曾经把它算进来是为了防连击，但副作用是：按下「停止」之后，
         * 界面要等 STOPPING 整整走完 3 秒，按钮才从「停止」变回「充电」——
         * 手感上就是"点了暂停要等好几秒才响应"。
         * 防连击现在由两道更合适的机制负责：
         *   · 界面侧 charge_lock_ms（800 ms，只挡同一拍重复投递的触摸事件）
         *   · 协议侧 stop_ms（5 秒内拒绝新的开始请求）
         * 所以这里就让它老老实实表示"正在充电"。 */
        default:
            return 0;
    }
}

void gb27930_request_stop(gb_context_t *ctx)
{
    if (ctx == NULL) { return; }

    /* 只有在充电流程里才谈得上"停止" */
    switch (ctx->state)
    {
        case GB_ST_HANDSHAKE:
        case GB_ST_IDENTIFY:
        case GB_ST_PARAM_CONFIG:
        case GB_ST_CHARGING_READY:
        case GB_ST_CHARGING:
            break;
        default:
            ui_log("GB", "当前没有在充电（%s），忽略「停止」请求",
                   gb27930_state_str(ctx->state));
            return;
    }

    ui_log("GB", "用户按下「停止」按钮（相当于拔出充电枪），中止充电");
    ctx->stop_req = 1;
    ctx->stop_ms  = mono_ms();
}

void gb27930_tick(gb_context_t *ctx, can_layer_t *cl, uint64_t now_us)
{
    /* ---- 用户按了「停止」（拔枪）：立刻中止本次充电 ----
     * 放在最前面处理，保证"按下就停"，不用等下一个周期。 */
    if (ctx->stop_req)
    {
        ctx->stop_req = 0;

        switch (ctx->state)
        {
            case GB_ST_HANDSHAKE:
            case GB_ST_IDENTIFY:
            case GB_ST_PARAM_CONFIG:
            case GB_ST_CHARGING_READY:
            case GB_ST_CHARGING:
                ui_log("GB", "充电被用户中止（模拟拔出充电枪）");
                set_state(ctx, GB_ST_STOPPING);
                break;
            default:
                break;
        }
    }

    if (ctx == NULL)
    {
        return;
    }

    ctx->now_ms = (now_us != 0) ? (now_us / 1000ULL) : mono_ms();

    /* 1. 多帧组包超时检查：BRM 半包若超过 1 s 未收齐则丢弃，
     *    避免残缺数据污染下一次会话（isotp 模块内部按 t_last 判定） */
    if (ctx->brm_rx.active &&
        isotp_rx_tick(&ctx->brm_rx, NULL) == ISOTP_ERR_TIMEOUT)
    {
        raise_error(ctx, GB_ERR_ISOTP, "BRM 多帧组包超时，半包已丢弃");
    }

    /* 2. 状态机主逻辑 */
    state_machine(ctx, cl);
}

/*==============================================================================
 * §10  报文处理总入口
 *============================================================================*/

int gb27930_process_frame(gb_context_t *ctx, can_layer_t *cl, const can_item_t *item)
{
    isotp_status_t ist;

    (void)cl;

    if (ctx == NULL || item == NULL)
    {
        return ISOTP_ERR_PARAM;
    }

    ctx->now_ms = mono_ms();

    /*---------------------------------------------------------------
     * 1. BRM(41B) 与 BCP(13B) 都通过 J1939 传输协议传输，
     *    且 TP.CM/TP.DT 的 CAN ID 完全相同（GB_ID_BRM_TPCM / GB_ID_BRM_TPDT）。
     *    因此必须先解析 TP.CM 中的**目标 PGN**才能区分两者：
     *      TP.CM RTS: [0]=0x10 [1..2]=总长(小端) [3]=包数 [4]=0xFF [5..7]=PGN(小端24位)
     *-------------------------------------------------------------*/
    if (item->can_id == GB_ID_BRM_TPCM && item->dlc >= 8)
    {
        uint8_t ctrl = item->data[0];

        if (ctrl == 0x10u || ctrl == 0x14u)   /* RTS 或 BAM */
        {
            uint16_t total = (uint16_t)(item->data[1] | ((uint16_t)item->data[2] << 8));
            uint8_t  pkts  = item->data[3];

            ctx->tp_target_pgn = (uint32_t)item->data[5]
                               | ((uint32_t)item->data[6] << 8)
                               | ((uint32_t)item->data[7] << 16);

            ui_log("TP", "收到 RTS: 目标 PGN=0x%04X 总长 %u 字节 / %u 包",
                   ctx->tp_target_pgn, total, pkts);

            /* BAM（0x14）是广播式传输，接收方不需要回 CTS；
             * RTS（0x10）则必须回 CTS，否则发送方会等到 1 s 超时。 */
            if (ctrl == 0x10u && cl != NULL)
            {
                (void)send_tp_cts(cl, 0xFF, 0x01, 0);
            }
        }
        else if (ctrl == 0xFFu)
        {
            /* 对端中止 */
            ui_log("TP", "对端发送 Abort，原因码 0x%02X", item->data[1]);
        }
    }

    ist = isotp_rx_feed(&ctx->brm_rx, item);

    if (ist == ISOTP_COMPLETE)
    {
        int rc;

        if (ctx->tp_target_pgn == GB_PGN_BCP)
        {
            rc = parse_bcp(ctx, ctx->brm_rx.data, ctx->brm_rx.total_len);
        }
        else
        {
            rc = parse_brm(ctx, ctx->brm_rx.data, ctx->brm_rx.total_len);
        }

        if (rc != 0)
        {
            ctx->parse_err_count++;
        }
        else if (cl != NULL)
        {
            /* 组包成功后回 EndOfMsgACK，让 BMS 侧的 TP 状态机干净收尾 */
            (void)send_tp_ack(cl,
                              ctx->brm_rx.total_len,
                              (uint8_t)((ctx->brm_rx.total_len + 6u) / 7u),
                              ctx->tp_target_pgn);
        }

        ctx->rx_msg_count++;
        touch_rx(ctx, item);
        return 1;
    }
    if (ist == ISOTP_INCOMPLETE)
    {
        return 0;   /* 多帧会话进行中，本帧已被消费 */
    }
    if (ist < 0 && ist != ISOTP_OK)
    {
        ctx->parse_err_count++;
        raise_error(ctx, GB_ERR_ISOTP, isotp_status_str(ist));
        if (cl != NULL && ctx->brm_rx.active)
        {
            /* 组包失败时按 J1939-21 规定回 Abort，让对端立即停止发送 */
            (void)send_tp_abort(cl, 0x01, ctx->tp_target_pgn);
            isotp_rx_reset(&ctx->brm_rx);
        }
        return (int)ist;
    }

    /*---------------------------------------------------------------
     * 2. 单帧报文分发
     *    （CHM/BHM/CRM/CTS/CML/BRO/CRO/BCL/BCS/BSM/BST/CST/BSD/CSD）
     *
     *    【逐报文超时基准】BCL 与 BCS 要各自记一份时刻，因为标准给它们的
     *    超时不同（BCL 1 s、BCS 5 s），共用一个 last_rx_ms 就分不出来了。
     *-------------------------------------------------------------*/
    switch (item->can_id)
    {
        case GB_ID_BHM:
            (void)parse_bhm(ctx, item);
            break;

        case GB_ID_BRO:
            (void)parse_bro(ctx, item);
            break;

        case GB_ID_BCL:
            if (parse_bcl(ctx, item) != 0)
            {
                ctx->parse_err_count++;
                return GB_ERR_DATA_INVALID;
            }
            ctx->last_bcl_ms = ctx->now_ms;
            break;

        case GB_ID_BCS:
            if (parse_bcs(ctx, item) != 0)
            {
                ctx->parse_err_count++;
                return GB_ERR_DATA_INVALID;
            }
            ctx->last_bcs_ms = ctx->now_ms;
            /* 输出能量积分（给 CSD 的 SPN3612 用）：
             *   P = U(V) * I(A)  →  W；按两次 BCS 的间隔积分成 Wh，再换成 0.1 kWh。
             *   只在充电阶段累加，跨状态跳变不计。 */
            if (ctx->state == GB_ST_CHARGING && ctx->bcs_energy_last_ms != 0u &&
                ctx->now_ms > ctx->bcs_energy_last_ms)
            {
                double dt_h = (double)(ctx->now_ms - ctx->bcs_energy_last_ms) / 3600000.0;
                double wh   = ctx->bcs.measure_voltage * ctx->bcs.measure_current * dt_h;

                if (wh > 0.0 && dt_h < 1.0)
                {
                    ctx->charge_energy_x10 += wh / 100.0;   /* Wh → 0.1 kWh */
                }
            }
            ctx->bcs_energy_last_ms = ctx->now_ms;
            break;

        case GB_ID_BSM:
            if (parse_bsm(ctx, item) != 0)
            {
                ctx->parse_err_count++;
                return GB_ERR_DATA_INVALID;
            }
            break;

        case GB_ID_BST:
            (void)parse_bst(ctx, item);
            break;

        case GB_ID_BSD:
            /* 中止充电统计报文（标准表 26：7 字节）。
             * 它不参与状态机流转，但字段必须按标准解析 —— 原来按"充电量/
             * 时长/总压"读，与标准完全不符。 */
            (void)parse_bsd(ctx, item);
            break;

        case GB_ID_BEM:
            /* BMS 错误报文（标准表 28：4 字节，优先权 2）。
             * 只解析并展示，**不改变状态机走向** —— BEM 是 BMS 告诉充电机
             * "我这边没收到什么"，正确反应是把对应报文补发/重新握手，
             * 而不是充电机自己也进故障态（那会变成两端互相甩锅）。 */
            if (parse_bem(ctx, item) != 0)
            {
                ctx->parse_err_count++;
                return GB_ERR_DATA_INVALID;
            }
            break;

        default:
            /* 与 GB/T 27930 无关的报文（其它设备的广播等），静默丢弃 */
            ctx->unknown_id_count++;
            return 0;
    }

    ctx->rx_msg_count++;
    touch_rx(ctx, item);

    if (ctx->cb.on_message != NULL)
    {
        ctx->cb.on_message(ctx->cb.arg, item->can_id, item->data, item->dlc);
    }

    return 1;
}

/*==============================================================================
 * §11  字符串与存储导出
 *============================================================================*/

const char *gb27930_state_str(gb_state_t st)
{
    switch (st)
    {
        case GB_ST_IDLE:            return "空闲(IDLE)";
        case GB_ST_HANDSHAKE:       return "握手(HANDSHAKE)";
        case GB_ST_IDENTIFY:        return "辨识(IDENTIFY)";
        case GB_ST_PARAM_CONFIG:    return "参数配置(PARAM_CONFIG)";
        case GB_ST_CHARGING_READY:  return "充电准备(READY)";
        case GB_ST_CHARGING:        return "充电中(CHARGING)";
        case GB_ST_STOPPING:        return "结束(STOPPING)";
        case GB_ST_FAULT:           return "故障(FAULT)";
        default:                    return "未知";
    }
}

const char *gb27930_error_str(gb_error_t err)
{
    switch (err)
    {
        case GB_ERR_NONE:               return "无";
        case GB_ERR_HANDSHAKE_TIMEOUT:  return "握手超时(BHM 未收到, 5 s)";
        case GB_ERR_IDENTIFY_TIMEOUT:   return "辨识超时(BRM 未收到, 5 s)";
        case GB_ERR_PARAM_TIMEOUT:      return "参数配置超时(BCP 未收到, 5 s)";
        case GB_ERR_READY_TIMEOUT:      return "准备就绪超时(BRO/CRO 60 s 未就绪)";
        case GB_ERR_CHARGE_TIMEOUT:     return "充电阶段报文超时(BCS 5 s)";
        case GB_ERR_BCL_TIMEOUT:        return "BCL 超时(标准 1 s, 应立即结束充电)";
        case GB_ERR_ISOTP:              return "多帧组包错误";
        case GB_ERR_DATA_INVALID:       return "数据域非法";
        case GB_ERR_BMS_ABORT:          return "BMS 主动中止";
        case GB_ERR_BUS_FAULT:          return "总线故障";
        default:                        return "未知异常";
    }
}

void gb27930_fill_storage_record(const gb_context_t *ctx, storage_charge_t *rec)
{
    struct timespec ts;

    if (ctx == NULL || rec == NULL)
    {
        return;
    }

    memset(rec, 0, sizeof(*rec));
    clock_gettime(CLOCK_REALTIME, &ts);

    rec->ts_us = (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;

    /* --- BCP --- */
    if (ctx->has_bcp)
    {
        rec->bcp_max_single_voltage = ctx->bcp.max_single_voltage;
        rec->bcp_max_current        = ctx->bcp.max_current;
        rec->bcp_nominal_energy     = ctx->bcp.nominal_energy;
        rec->bcp_max_total_voltage  = ctx->bcp.max_total_voltage;
        rec->bcp_max_temperature    = ctx->bcp.max_temperature;
        rec->bcp_soc                = ctx->bcp.soc;
        rec->bcp_current_voltage    = ctx->bcp.current_voltage;
    }

    /* --- BCL --- */
    if (ctx->has_bcl)
    {
        rec->bcl_voltage_demand = ctx->bcl.voltage_demand;
        rec->bcl_current_demand = ctx->bcl.current_demand;
        rec->bcl_charge_mode    = ctx->bcl.charge_mode;
        /* 【标准依据】标准表 17 的 BCL 只有 5 字节 3 个字段，**没有**
         * 「允许充电电压 / 允许充电电流」—— 这两个量属于 BCP（SPN2819 /
         * SPN2817）。数据库的两列保留（改动 schema 需要迁移，不值得），
         * 这里填入等价的 BCP 值，语义上就是"本次会话被允许的充电上限"。
         * 没有收到 BCP 时填 0，不猜。 */
        rec->bcl_allow_voltage  = ctx->has_bcp ? ctx->bcp.max_total_voltage : 0.0f;
        rec->bcl_allow_current  = ctx->has_bcp ? ctx->bcp.max_current        : 0.0f;
    }

    /* --- BCS --- */
    if (ctx->has_bcs)
    {
        rec->bcs_measure_voltage    = ctx->bcs.measure_voltage;
        rec->bcs_measure_current    = ctx->bcs.measure_current;
        rec->bcs_max_single_voltage = ctx->bcs.max_single_voltage;
        /* 数据库列名是 bcs_max_single_no（"最高单体编号"）。标准里 BCS 的
         * B5-B6 打包的是**所在组号**（13-16 位），没有单体编号 ——
         * 这里存入组号，列名保持不变以免动 schema。 */
        rec->bcs_max_single_no      = ctx->bcs.max_single_group;
        rec->bcs_current_soc        = ctx->bcs.current_soc;
    }

    /* --- BSM --- */
    if (ctx->has_bsm)
    {
        rec->bsm_max_temp    = ctx->bsm.max_temp;
        rec->bsm_max_temp_no = ctx->bsm.max_temp_no;
        rec->bsm_min_temp    = ctx->bsm.min_temp;
        rec->bsm_min_temp_no = ctx->bsm.min_temp_no;
        /* 数据库列 bsm_fault 原语义是"故障标志位"，但标准表 20 的 BSM
         * **没有**故障标志位这个字段，只有 6 个 2 位状态字段。
         * 这里存入 B6（SPN3090~3093 四个状态打包后的原始字节），
         * 列名保持不变；B7 的状态可从 ctx->bsm.status_b7 取。 */
        rec->bsm_fault       = ctx->bsm.status_b6;
    }

    /* --- 状态机上下文 --- */
    rec->state      = (uint8_t)ctx->state;
    rec->error_code = (uint8_t)ctx->error;
    rec->session_id = ctx->session_id;
}

/******************* (C) COPYRIGHT 2025 CAN Monitor *****END OF FILE****/
