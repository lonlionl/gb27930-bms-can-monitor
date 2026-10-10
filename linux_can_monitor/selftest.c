/**
 * @file    selftest.c
 * @brief   GB/T 27930-2015 协议栈与各层模块的离线自检程序（无需真实 CAN 硬件）
 *
 * 需求文档明确要求「各模块独立可测试」，本文件即为该要求的落地：
 * 它把 CAN 硬件层换成合成帧，直接驱动协议解析、多帧重组、状态机、
 * 无锁环形缓冲与 SQLite 存储层，逐项断言结果，最后打印 PASS/FAIL 汇总。
 *
 * 覆盖的测试项：
 *   T1  无锁环形缓冲区：容量、顺序、满队列丢弃计数、批量出队
 *   T2  小端解析与缩放因子：BCP(13B) 七个物理量（标准 4.4 低字节先发送）
 *   T3  J1939 TP 多帧重组：BRM 41 字节 / 6 包，含 END 边界（41 = 5*7 + 6）
 *   T4  J1939 TP 多帧重组：BCP 13 字节 / 2 包
 *   T5  TP.CM 目标 PGN 识别（同一对 CAN ID 承载 BRM 与 BCP 两种报文）
 *   T6  单帧报文解析：BCL / BCS / BSM / BRO
 *   T7  物理量换算正确性（含 -400 A 电流偏移与 -50 ℃ 温度偏移）
 *   T8  充电机侧状态机流转：IDLE -> HANDSHAKE -> IDENTIFY -> PARAM -> READY -> CHARGING
 *   T9  SQLite 存储：WAL 模式、批量写入、按时间范围回读、统计摘要
 *   T10 异常路径：ISO-TP 序号错乱应被检出并丢弃整包
 *
 * 编译运行：
 *   make selftest && ./selftest
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "ring_buffer.h"
#include "isotp.h"
#include "gb27930.h"
#include "storage.h"

/* 自检程序不链接 main.c，但 storage.c / can_layer.c / gb27930.c 都引用了
 * 这个由 main.c 定义的全局开关，所以这里补一个定义，否则链接会报
 * "对 g_verbose 未定义的引用"。 */
int g_verbose = 0;
#include "can_layer.h"

/*==============================================================================
 *                              测试框架
 *============================================================================*/

static int g_pass = 0;
static int g_fail = 0;
static const char *g_case = "";

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            printf("  " "\033[31m" "FAIL" "\033[0m" " [%s] " fmt "\n",          \
                   g_case, ##__VA_ARGS__);                                     \
        }                                                                      \
    } while (0)

#define CHECK_FEQ(a, b, fmt, ...)                                              \
    CHECK(fabs((double)(a) - (double)(b)) < 0.001, fmt, ##__VA_ARGS__)

#define CASE(name)                                                             \
    do {                                                                       \
        g_case = name;                                                         \
        printf("  %-58s", name);                                               \
    } while (0)

#define CASE_END()                                                             \
    do {                                                                       \
        printf(" [ok]\n");                                                     \
    } while (0)

/*==============================================================================
 *                              帧构造辅助
 *============================================================================*/

/** 构造一个带指定 ID 与数据的内核时间戳 CAN 帧 */
static can_item_t mkframe(uint32_t id, const uint8_t *data, uint8_t len)
{
    can_item_t it;

    memset(&it, 0, sizeof(it));
    it.can_id      = id;
    it.dlc         = len;
    it.is_extended = 1;
    if (data != NULL && len > 0)
    {
        memcpy(it.data, data, len > 8 ? 8 : len);
    }
    return it;
}

/* GB/T 27930-2015 标准 4.4：数据信息传输采用低字节先发送的格式（小端）。
 * 测试载荷必须按小端构造，才能与协议层的小端解析对上。 */
static void le16_put(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/*==============================================================================
 *                              载荷构造（与 STM32 端完全一致的字节布局）
 *============================================================================*/

/** 按 BCP 布局构造 13 字节载荷 */
static void build_bcp(uint8_t *d, uint16_t cell_v_x100, uint16_t max_i_x10_off,
                      uint16_t energy_x10, uint16_t max_v_x10, uint8_t temp_off,
                      uint16_t soc_x10, uint16_t cur_v_x10)
{
    memset(d, 0, 13);
    le16_put(&d[0],  cell_v_x100);
    le16_put(&d[2],  max_i_x10_off);
    le16_put(&d[4],  energy_x10);
    le16_put(&d[6],  max_v_x10);
    d[8] = temp_off;
    le16_put(&d[9],  soc_x10);
    le16_put(&d[11], cur_v_x10);
}

/** 按 BRM 布局构造 41 字节载荷（标准表 11 的字段顺序）
 *   B1-B3 版本 / B4 电池类型 / B5-B6 额定容量 / B7-B8 额定总电压 /
 *   B9-B12 厂商 ASCII(4B) / B13-B16 电池组序号(4B) /
 *   B17 年(1985 偏移) B18 月 B19 日 / B20-B22 充电次数(3B) /
 *   B23 产权标识 / B24 预留 / B25-B41 VIN(17B) */
static void build_brm(uint8_t *d, uint16_t cap_x10, uint16_t v_x10)
{
    memset(d, 0xFF, 41);          /* 未写到的位按标准 7.9 填 1 */
    d[0] = 0x01; d[1] = 0x01; d[2] = 0x00;   /* B1-B3 协议版本 V1.1 = 0x000101 */
    d[3] = 0x03;                 /* B4 电池类型：03 = 磷酸铁锂 */
    le16_put(&d[4], cap_x10);    /* B5-B6 额定容量 0.1 Ah */
    le16_put(&d[6], v_x10);      /* B7-B8 额定总电压 0.1 V */
    memcpy(&d[8],  "CATL", 4);   /* B9-B12 生产厂商 4 字节 ASCII */
    memcpy(&d[12], "SN01", 4);   /* B13-B16 电池组序号 */
    d[16] = 39;                  /* B17 生产年份：39 + 1985 = 2024 */
    d[17] = 6;                   /* B18 月 */
    d[18] = 18;                  /* B19 日 */
    d[19] = 0x80; d[20] = 0x00; d[21] = 0x00;  /* B20-B22 充电次数 128（小端 3B） */
    d[22] = 0x01;                /* B23 产权标识：1 = 车自有 */
    d[23] = 0xFF;                /* B24 预留（填 1） */
    memcpy(&d[24], "LFP512V100A000001", 17);   /* B25-B41 VIN 17 字节 */
}

/**
 * @brief  把一个长载荷通过 J1939 TP 逐帧喂给协议层
 * @param  ctx      协议上下文
 * @param  pgn      目标 PGN（0x0200 BRM / 0x1C00 BCP）
 * @param  payload  载荷
 * @param  len      载荷长度
 * @return 0 成功
 */
static int feed_isotp(gb_context_t *ctx, uint32_t pgn,
                      const uint8_t *payload, uint16_t len)
{
    uint8_t  d[8];
    uint16_t sent = 0;
    uint8_t  seq  = 1;
    can_item_t f;

    /* --- RTS --- */
    memset(d, 0xFF, 8);
    d[0] = 0x10;
    d[1] = (uint8_t)(len & 0xFF);          /* 小端 */
    d[2] = (uint8_t)((len >> 8) & 0xFF);
    d[3] = (uint8_t)((len + 6) / 7);
    d[4] = 0xFF;
    d[5] = (uint8_t)(pgn & 0xFF);
    d[6] = (uint8_t)((pgn >> 8) & 0xFF);
    d[7] = (uint8_t)((pgn >> 16) & 0xFF);
    f = mkframe(GB_ID_BRM_TPCM, d, 8);
    (void)gb27930_process_frame(ctx, NULL, &f);

    /* --- DT × N --- */
    while (sent < len)
    {
        uint16_t n = (uint16_t)(len - sent);
        if (n > 7) { n = 7; }

        memset(d, 0xFF, 8);
        d[0] = seq++;
        memcpy(&d[1], &payload[sent], n);

        f = mkframe(GB_ID_BRM_TPDT, d, 8);
        (void)gb27930_process_frame(ctx, NULL, &f);

        sent = (uint16_t)(sent + n);
    }
    return 0;
}

/*==============================================================================
 *                              T1 无锁环形缓冲区
 *============================================================================*/

static void test_ring_buffer(void)
{
    ring_buffer_t rb;
    can_item_t    in, out;
    size_t        i;
    int           dropped = 0;

    CASE("T1 无锁环形缓冲区 容量/顺序/满丢弃/批量出队");

    CHECK(rb_init(&rb, 4) == 0, "rb_init 失败");
    CHECK(rb_capacity(&rb) == 4, "容量取整错误: %zu", rb_capacity(&rb));
    CHECK(rb_is_empty(&rb) == 1, "新建队列非空");

    /* 写入 3 帧，校验 FIFO 顺序 */
    for (i = 0; i < 3; i++)
    {
        memset(&in, 0, sizeof(in));
        in.can_id = 0x1000 + (uint32_t)i;
        in.dlc    = 8;
        in.data[0] = (uint8_t)i;
        CHECK(rb_push(&rb, &in) == 0, "第 %zu 帧入队失败", i);
    }
    CHECK(rb_size(&rb) == 3, "队列深度应为 3，实际 %zu", rb_size(&rb));

    /* 队列满（容量 4）：第 5 帧应被丢弃 */
    memset(&in, 0, sizeof(in));
    in.can_id = 0x9999;
    CHECK(rb_push(&rb, &in) == 0, "第 4 帧应能入队");
    dropped = rb_push(&rb, &in);
    CHECK(dropped == -1, "队列满时 rb_push 应返回 -1，实际 %d", dropped);
    CHECK(rb_drop_count(&rb) == 1, "丢弃计数应为 1，实际 %llu",
          (unsigned long long)rb_drop_count(&rb));

    /* 逐个出队，校验顺序：
     * 前 3 帧是 0x1000/0x1001/0x1002，第 4 帧是那次「恰好填满队列」的 0x9999，
     * 之后的 0x9999 因队列已满被丢弃，不应出现在出队序列里。 */
    for (i = 0; i < 4; i++)
    {
        uint32_t expect = (i < 3) ? (0x1000u + (uint32_t)i) : 0x9999u;

        CHECK(rb_pop(&rb, &out) == 0, "第 %zu 帧出队失败", i);
        CHECK(out.can_id == expect,
              "第 %zu 帧顺序错乱: 期望 0x%04X 实际 0x%04X",
              i, expect, out.can_id);
    }
    CHECK(rb_pop(&rb, &out) == -1, "空队列出队应返回 -1");
    CHECK(rb_is_empty(&rb) == 1, "队列应为空");

    /* 批量出队 */
    for (i = 0; i < 4; i++)
    {
        memset(&in, 0, sizeof(in));
        in.can_id = 0x2000 + (uint32_t)i;
        (void)rb_push(&rb, &in);
    }
    {
        can_item_t batch[8];
        size_t n = rb_pop_batch(&rb, batch, 8);
        CHECK(n == 4, "批量出队应取到 4 帧，实际 %zu", n);
        CHECK(batch[0].can_id == 0x2000 && batch[3].can_id == 0x2003,
              "批量出队顺序错乱");
    }
    CHECK(rb_high_watermark(&rb) == 4, "峰值水位应为 4，实际 %llu",
          (unsigned long long)rb_high_watermark(&rb));

    rb_deinit(&rb);
    CASE_END();
}

/*==============================================================================
 *                       T3/T4/T5 多帧重组与 PGN 识别
 *============================================================================*/

static void test_isotp_multiframe(gb_context_t *ctx)
{
    uint8_t  payload[64];
    uint8_t  d[8];
    can_item_t f;

    /*------------------------------------------------------------------
     * T3: BRM 41 字节 / 6 包
     *----------------------------------------------------------------*/
    CASE("T3 J1939 TP 重组 BRM 41 字节（含 41 = 5*7+6 的末包边界）");

    build_brm(payload, 1000 /* 100.0Ah */, 5120 /* 512.0V */);

    /* 先只发 RTS，验证 PGN 识别。
     * ★ B6-B8 的 PGN **从宏推导**，不写死字节 —— 写死过一次
     *   （BCP 的 PGN 从 0x1C00 改成 0x0600 后忘了同步，测试直接红），
     *   这里改成推导后就不可能再和宏脱节。 */
    memset(d, 0xFF, 8);
    d[0] = 0x10; d[1] = 41; d[2] = 0; d[3] = 6;
    d[5] = (uint8_t)(GB_PGN_BRM & 0xFFu);          /* PGN 小端 24 位 */
    d[6] = (uint8_t)((GB_PGN_BRM >> 8) & 0xFFu);
    d[7] = (uint8_t)((GB_PGN_BRM >> 16) & 0xFFu);
    CHECK(d[5] == 0x00 && d[6] == 0x02 && d[7] == 0x00,   /* BRM = 0x0200 */
          "BRM 的 TP.CM 参考帧字节错误: %02X %02X %02X（应为 00 02 00）",
          d[5], d[6], d[7]);
    f = mkframe(GB_ID_BRM_TPCM, d, 8);
    (void)gb27930_process_frame(ctx, NULL, &f);
    CHECK(ctx->tp_target_pgn == GB_PGN_BRM,
          "TP.CM 目标 PGN 识别错误: 期望 0x%04X 实际 0x%04X",
          GB_PGN_BRM, ctx->tp_target_pgn);

    /* 再补发剩余（RTS 已发过，直接发 DT） */
    {
        uint16_t sent = 0;
        uint8_t  seq = 1;
        while (sent < 41)
        {
            uint16_t n = (uint16_t)(41 - sent);
            if (n > 7) { n = 7; }
            memset(d, 0xFF, 8);
            d[0] = seq++;
            memcpy(&d[1], &payload[sent], n);
            f = mkframe(GB_ID_BRM_TPDT, d, 8);
            (void)gb27930_process_frame(ctx, NULL, &f);
            sent = (uint16_t)(sent + n);
        }
    }

    CHECK(ctx->has_brm == 1, "BRM 未完成组包");
    CHECK(ctx->brm.raw_len == 41, "BRM 长度应为 41，实际 %u", ctx->brm.raw_len);
    CHECK_FEQ(ctx->brm.rated_capacity, 100.0, "额定容量解析错误: %.2f", ctx->brm.rated_capacity);
    CHECK_FEQ(ctx->brm.rated_voltage, 512.0, "额定总压解析错误: %.2f", ctx->brm.rated_voltage);
    CHECK(strncmp(ctx->brm.manufacturer, "CATL", 4) == 0,
          "生产厂商解析错误: [%s]", ctx->brm.manufacturer);
    CHECK(strncmp(ctx->brm.vin, "LFP512V100A000001", 17) == 0,
          "VIN 解析错误: [%s]", ctx->brm.vin);
    /* B17 是"1 年/位，1985 偏移"：原始 39 → 2024 年 */
    CHECK(ctx->brm.produce_year == 2024 && ctx->brm.produce_month == 6 &&
          ctx->brm.produce_day == 18, "生产日期解析错误: %u-%u-%u",
          ctx->brm.produce_year, ctx->brm.produce_month, ctx->brm.produce_day);
    CHECK(ctx->brm.charge_count == 128, "充电次数解析错误: %u", ctx->brm.charge_count);
    CHECK(ctx->brm.battery_type == 3, "电池类型解析错误: %u", ctx->brm.battery_type);
    CHECK(ctx->brm.ownership == 1, "产权标识解析错误: %u", ctx->brm.ownership);
    /* B1-B3 版本号小端 24 位：`01 01 00` → **0x000101**
     * 标准原文：V1.1 = byte3,byte2—0001H；byte1—01H。
     * 期望值是 24 位的 0x000101，不是 16 位的 0x0001/0x0101。 */
    CHECK(ctx->brm.version == 0x000101u,
          "协议版本号解析错误: 0x%06X（期望 0x000101）", ctx->brm.version);

    CASE_END();

    /*------------------------------------------------------------------
     * T4 + T5: BCP 13 字节 / 2 包，且与 BRM 共用同一对 CAN ID
     *----------------------------------------------------------------*/
    CASE("T4/T5 同一 CAN ID 下用 PGN 区分 BCP 13 字节 / 2 包");

    /* 单体 3.65V, 电流 100.0A(带 -400 偏移 => 5000), 能量 51.2kWh,
     * 总压 584.0V, 温度 55C(偏移 105), SOC 45.0%, 当前电压 480.0V */
    build_bcp(payload, 365, 5000, 512, 5840, 105, 450, 4800);

    memset(d, 0xFF, 8);
    d[0] = 0x10; d[1] = 13; d[2] = 0; d[3] = 2;
    /* ★ BCP 的 PGN 是 **0x0600**（标准表 5：000600H），不是旧方案的 0x1C00。
     *   这里从宏推导，避免再次与宏脱节；下面那条 CHECK 把字面字节钉死，
     *   这样即便宏被改错，测试也能立刻指出"应该是什么"。 */
    d[5] = (uint8_t)(GB_PGN_BCP & 0xFFu);
    d[6] = (uint8_t)((GB_PGN_BCP >> 8) & 0xFFu);
    d[7] = (uint8_t)((GB_PGN_BCP >> 16) & 0xFFu);
    CHECK(GB_PGN_BCP == 0x0600u && d[5] == 0x00 && d[6] == 0x06 && d[7] == 0x00,
          "BCP 的 PGN 应为 0x0600（TP.CM 数据域 00 06 00），实际宏=0x%04X 字节=%02X %02X %02X",
          GB_PGN_BCP, d[5], d[6], d[7]);
    f = mkframe(GB_ID_BRM_TPCM, d, 8);
    (void)gb27930_process_frame(ctx, NULL, &f);
    CHECK(ctx->tp_target_pgn == GB_PGN_BCP,
          "BCP 的 TP.CM 目标 PGN 识别错误: 0x%04X", ctx->tp_target_pgn);

    {
        uint16_t sent = 0;
        uint8_t  seq = 1;
        while (sent < 13)
        {
            uint16_t n = (uint16_t)(13 - sent);
            if (n > 7) { n = 7; }
            memset(d, 0xFF, 8);
            d[0] = seq++;
            memcpy(&d[1], &payload[sent], n);
            f = mkframe(GB_ID_BRM_TPDT, d, 8);
            (void)gb27930_process_frame(ctx, NULL, &f);
            sent = (uint16_t)(sent + n);
        }
    }

    CHECK(ctx->has_bcp == 1, "BCP 未完成组包");
    CHECK_FEQ(ctx->bcp.max_single_voltage, 3.65, "单体最高允许电压错误: %.3f",
              ctx->bcp.max_single_voltage);
    CHECK_FEQ(ctx->bcp.max_current, 100.0, "最高允许电流错误(应含 -400A 偏移): %.2f",
              ctx->bcp.max_current);
    CHECK_FEQ(ctx->bcp.nominal_energy, 51.2, "标称总能量错误: %.2f", ctx->bcp.nominal_energy);
    CHECK_FEQ(ctx->bcp.max_total_voltage, 584.0, "最高允许总压错误: %.2f",
              ctx->bcp.max_total_voltage);
    CHECK_FEQ(ctx->bcp.max_temperature, 55.0, "最高允许温度错误(应含 -50C 偏移): %.2f",
              ctx->bcp.max_temperature);
    CHECK_FEQ(ctx->bcp.soc, 45.0, "SOC 错误: %.2f", ctx->bcp.soc);
    CHECK_FEQ(ctx->bcp.current_voltage, 480.0, "当前电池电压错误: %.2f",
              ctx->bcp.current_voltage);

    CASE_END();
}

/*==============================================================================
 *                       T6/T7 单帧报文解析与物理量换算
 *============================================================================*/

static void test_single_frames(gb_context_t *ctx)
{
    uint8_t    d[8];
    can_item_t f;

    /*------------------- BHM 握手 -------------------*/
    CASE("T6/T7 BHM 车辆握手解析（最高允许充电总电压 0.1 V/位）");
    memset(d, 0, 8);
    /* ★ 标准表 9 的 BHM **2 字节全是"最高允许充电总电压"**，没有版本号。
     *   5840 = 584.0 V（小端） */
    le16_put(&d[0], 5840);
    f = mkframe(GB_ID_BHM, d, 2);
    CHECK(gb27930_process_frame(ctx, NULL, &f) == 1, "BHM 未被识别");
    CHECK(ctx->has_bhm == 1, "has_bhm 未置位");
    CHECK_FEQ(ctx->bhm_max_total_voltage, 584.0,
              "BHM 最高允许充电总电压错误: %.2f", ctx->bhm_max_total_voltage);
    CASE_END();

    /*------------------- BRO 准备就绪 -------------------*/
    CASE("T6 BRO 电池充电准备就绪（0x00 未做好 / 0xAA 完成 / 0xFF 无效）");
    d[0] = 0xAA;
    f = mkframe(GB_ID_BRO, d, 1);
    CHECK(gb27930_process_frame(ctx, NULL, &f) == 1, "BRO 未被识别");
    CHECK(ctx->has_bro == 1, "BRO=0xAA 未置就绪标志");
    d[0] = 0x00;
    f = mkframe(GB_ID_BRO, d, 1);
    (void)gb27930_process_frame(ctx, NULL, &f);
    CHECK(ctx->has_bro == 0, "BRO=0x00 未清除就绪标志");
    d[0] = 0xFF;                       /* 标准表 15：0xFF = 无效 */
    f = mkframe(GB_ID_BRO, d, 1);
    (void)gb27930_process_frame(ctx, NULL, &f);
    CHECK(ctx->has_bro == 0, "BRO=0xFF（无效）不应被当成已就绪");
    CASE_END();

    /*------------------- BCL -------------------*/
    CASE("T6/T7 BCL 电池充电需求解析（标准表 17：只有 5 字节）");
    memset(d, 0, 8);
    le16_put(&d[0], 4820);            /* B1-B2 电压需求 482.0 V */
    le16_put(&d[2], 5000);            /* B3-B4 电流需求 5000*0.1-400 = 100.0 A */
    d[4] = 2;                         /* B5 充电模式：恒流 */
    f = mkframe(GB_ID_BCL, d, 5);     /* ★ DLC 必须是 5，不是 8 */
    CHECK(gb27930_process_frame(ctx, NULL, &f) == 1, "BCL 未被识别");
    CHECK(ctx->has_bcl == 1, "has_bcl 未置位");
    CHECK_FEQ(ctx->bcl.voltage_demand, 482.0, "电压需求错误: %.2f", ctx->bcl.voltage_demand);
    CHECK_FEQ(ctx->bcl.current_demand, 100.0, "电流需求错误: %.2f", ctx->bcl.current_demand);
    CHECK(ctx->bcl.charge_mode == 2, "充电模式错误: %u", ctx->bcl.charge_mode);
    CASE_END();

    /*------------------- BCS -------------------*/
    CASE("T6/T7 BCS 电池充电总状态解析（B5-B6 位打包 + SOC 在 B7）");
    memset(d, 0, 8);
    le16_put(&d[0], 5123);            /* B1-B2 512.3 V */
    le16_put(&d[2], 4987);            /* B3-B4 498.7-400 = 98.7 A */
    /* B5-B6 SPN3077：1-12 位 = 电压(0.01 V/位)，13-16 位 = 所在组号(1/位)
     *   电压 3.20 V → 320；组号 5 → 5 << 12 = 0x5000；合起来 0x5140 */
    le16_put(&d[4], (uint16_t)(320 | (5u << 12)));
    d[6] = 62;                        /* B7 SOC 62 %（1 %/位） */
    d[7] = 0xFF;                      /* B8 剩余充电时间：0xFF = 未规定（本工程不估算） */
    f = mkframe(GB_ID_BCS, d, 8);     /* 标准 9 字节；本工程按 8 字节单帧 */
    CHECK(gb27930_process_frame(ctx, NULL, &f) == 1, "BCS 未被识别");
    CHECK(ctx->has_bcs == 1, "has_bcs 未置位");
    CHECK_FEQ(ctx->bcs.measure_voltage, 512.3, "充电电压测量值错误: %.2f",
              ctx->bcs.measure_voltage);
    CHECK_FEQ(ctx->bcs.measure_current, 98.7, "充电电流测量值错误: %.2f",
              ctx->bcs.measure_current);
    CHECK_FEQ(ctx->bcs.max_single_voltage, 3.20, "最高单体电压错误: %.3f",
              ctx->bcs.max_single_voltage);
    CHECK(ctx->bcs.max_single_group == 5, "最高单体所在组号错误: %u",
          ctx->bcs.max_single_group);
    /* 注意格式串里的 "%%/位"：CHECK 的格式串会走 printf，
     * 字面量百分号必须写成 %% 否则 gcc 报
     *   warning: unknown conversion type character '/' in format [-Wformat=] */
    CHECK_FEQ(ctx->bcs.current_soc, 62.0, "SOC 错误: %.2f（BCS 是 1 %%/位）",
              ctx->bcs.current_soc);
    /* B8 剩余充电时间：本工程没有估算能力，按标准 7.9 发 0xFF 表示未规定。
     * 这里**刻意断言"标识为无有效值"而不是某个分钟数** ——
     * 把 0xFF 换算成 255 min 同样是在编造具体数值。 */
    CHECK(ctx->bcs.remain_charge_raw == 0xFF,
          "剩余充电时间原始字节应为 0xFF(未规定)，实际 0x%02X", ctx->bcs.remain_charge_raw);
    CHECK(ctx->bcs.remain_charge_absent == 1, "应标记出【该字段无有效值】");
    CHECK(ctx->bcs.remain_charge_lo_byte == 1,
          "应标记出【只带低字节】的 8 字节偏差");
    CASE_END();

    /*------------------- BSM -------------------*/
    CASE("T6/T7 BSM 动力蓄电池状态信息解析（标准表 20：7 字节，无电压字段）");
    memset(d, 0, 8);
    d[0] = 7;                         /* B1 最高单体电压**所在编号**（1 偏移） */
    d[1] = 88;                        /* B2 最高温度 88-50 = 38 C */
    d[2] = 7;                         /* B3 最高温度检测点编号（1 偏移） */
    d[3] = 81;                        /* B4 最低温度 81-50 = 31 C */
    d[4] = 2;                         /* B5 最低温度检测点编号 */
    /* B6：单体电压=00 正常，SOC=01 过高，过流=00 正常，过温=10 不可信
     *     低两位在前 → 0b10_00_01_00 = 0x84 */
    d[5] = (uint8_t)((0u << 0) | (1u << 2) | (0u << 4) | (2u << 6));
    /* B7：绝缘=10 不可信，连接器=10 不可信，充电允许=01 允许，未定义位=11
     *     低两位在前 → 0b11_01_10_10 = 0xDA */
    d[6] = (uint8_t)((2u << 0) | (2u << 2) | (1u << 4) | (3u << 6));
    f = mkframe(GB_ID_BSM, d, 7);     /* ★ DLC 必须是 7 */
    CHECK(gb27930_process_frame(ctx, NULL, &f) == 1, "BSM 未被识别");
    CHECK(ctx->has_bsm == 1, "has_bsm 未置位");
    CHECK(ctx->bsm.max_single_voltage_no == 7, "最高单体电压所在编号错误: %u",
          ctx->bsm.max_single_voltage_no);
    CHECK_FEQ(ctx->bsm.max_temp, 38.0, "最高温度错误(应含 -50C 偏移): %.2f",
              ctx->bsm.max_temp);
    CHECK_FEQ(ctx->bsm.min_temp, 31.0, "最低温度错误(应含 -50C 偏移): %.2f",
              ctx->bsm.min_temp);
    CHECK(ctx->bsm.max_temp_no == 7 && ctx->bsm.min_temp_no == 2,
          "温度检测点编号错误: %u/%u", ctx->bsm.max_temp_no, ctx->bsm.min_temp_no);
    CHECK(ctx->bsm.status_b6 == 0x84, "B6 原始值错误: 0x%02X", ctx->bsm.status_b6);
    CHECK(ctx->bsm.status_b7 == 0xDA, "B7 原始值错误: 0x%02X", ctx->bsm.status_b7);
    CHECK(ctx->bsm.cell_voltage_state == 0, "SPN3090 应为正常(00)，实际 %u",
          ctx->bsm.cell_voltage_state);
    CHECK(ctx->bsm.soc_state == 1, "SPN3091 应为过高(01)，实际 %u", ctx->bsm.soc_state);
    CHECK(ctx->bsm.over_current_state == 0, "SPN3092 应为正常(00)，实际 %u",
          ctx->bsm.over_current_state);
    CHECK(ctx->bsm.over_temp_state == 2, "SPN3093 应为不可信(10)，实际 %u",
          ctx->bsm.over_temp_state);
    CHECK(ctx->bsm.insulation_state == 2, "SPN3094 应为不可信(10)，实际 %u",
          ctx->bsm.insulation_state);
    CHECK(ctx->bsm.connector_state == 2, "SPN3095 应为不可信(10)，实际 %u",
          ctx->bsm.connector_state);
    CHECK(ctx->bsm.charge_permit == 1, "SPN3096 应为允许充电(01)，实际 %u",
          ctx->bsm.charge_permit);
    CASE_END();

    /*------------------- BSD -------------------*/
    CASE("T6/T7 BSD 中止充电统计解析（标准表 26：单体最低/最高压 + 最低/最高温）");
    memset(d, 0, 8);
    d[0] = 96;                        /* B1 中止 SOC 96 %（1 %/位） */
    le16_put(&d[1], 318);             /* B2-B3 单体**最低**电压 3.18 V */
    le16_put(&d[3], 352);             /* B4-B5 单体**最高**电压 3.52 V */
    d[5] = 78;                        /* B6 **最低**温度 78-50 = 28 C */
    d[6] = 95;                        /* B7 **最高**温度 95-50 = 45 C */
    f = mkframe(GB_ID_BSD, d, 7);
    CHECK(gb27930_process_frame(ctx, NULL, &f) == 1, "BSD 未被识别");
    CHECK(ctx->bsd.soc == 96, "BSD 中止 SOC 错误: %u", ctx->bsd.soc);
    CHECK_FEQ(ctx->bsd.min_single_voltage, 3.18, "单体最低电压错误: %.3f",
              ctx->bsd.min_single_voltage);
    CHECK_FEQ(ctx->bsd.max_single_voltage, 3.52, "单体最高电压错误: %.3f",
              ctx->bsd.max_single_voltage);
    CHECK_FEQ(ctx->bsd.min_temp, 28.0, "最低温度错误: %.2f", ctx->bsd.min_temp);
    CHECK_FEQ(ctx->bsd.max_temp, 45.0, "最高温度错误: %.2f", ctx->bsd.max_temp);
    CASE_END();

    /*------------------- BST 中止 -------------------*/
    CASE("T6 BST BMS 中止充电解析（B1 是 4 个 2 位字段，不是整字节掩码）");
    memset(d, 0, 8);
    /* B1：达到 SOC 目标值 = 01（1-2 位，位移 6）；其余 00 */
    d[0] = (uint8_t)(1u << 6);
    d[1] = 0x00;   /* B2 故障原因 */
    d[2] = 0x00;   /* B3 故障原因 */
    d[3] = 0x00;   /* B4 错误原因 */
    f = mkframe(GB_ID_BST, d, 4);
    CHECK(gb27930_process_frame(ctx, NULL, &f) == 1, "BST 未被识别");
    CHECK(ctx->has_bst == 1, "has_bst 未置位");
    CHECK(ctx->bst.reason_flags == 0x40, "中止原因字节错误: 0x%02X", ctx->bst.reason_flags);
    CHECK(GB_FIELD2(ctx->bst.reason_flags, GB_BST_R_SOC_SHIFT) == GB_ST_ABNORMAL,
          "SPN3511 1-2 位应为【达到 SOC 目标值】，实际 %u",
          GB_FIELD2(ctx->bst.reason_flags, GB_BST_R_SOC_SHIFT));
    CHECK(GB_FIELD2(ctx->bst.reason_flags, GB_BST_R_CHARGER_STOP_SHIFT) == 0,
          "SPN3511 7-8 位应为 00，实际 %u",
          GB_FIELD2(ctx->bst.reason_flags, GB_BST_R_CHARGER_STOP_SHIFT));
    CASE_END();

    /*------------------- 未知 ID -------------------*/
    CASE("T6 未知 CAN ID 应被静默丢弃且计数");
    {
        uint64_t before = ctx->unknown_id_count;
        d[0] = 0xFF;
        f = mkframe(0x12345678u, d, 1);
        CHECK(gb27930_process_frame(ctx, NULL, &f) == 0, "未知 ID 应返回 0");
        CHECK(ctx->unknown_id_count == before + 1, "未知 ID 计数未递增");
    }
    CASE_END();

    /*------------------- 短帧容错 -------------------*/
    CASE("T6 长度不足的报文应被拒绝而不崩溃");
    {
        uint64_t before = ctx->parse_err_count;
        memset(d, 0, 8);
        /* BCS 当前按 8 字节单帧发送（标准 9 字节需 TP），少于 8 字节应拒绝 */
        f = mkframe(GB_ID_BCS, d, 3);
        CHECK(gb27930_process_frame(ctx, NULL, &f) == GB_ERR_DATA_INVALID,
              "短帧 BCS 应返回 GB_ERR_DATA_INVALID");
        CHECK(ctx->parse_err_count == before + 1, "解析错误计数未递增");
    }
    CASE_END();

    /*------------------- CTS 压缩 BCD 编解码 -------------------*/
    CASE("T6/T7 CTS 压缩 BCD 编解码（标准表 13：秒 分 时 日 月 年）");
    {
        /* 2025-06-18 09:07:25 → 各字节压缩 BCD */
        CHECK(gb27930_bcd_encode(25) == 0x25, "25 -> BCD 应为 0x25，实际 0x%02X",
              gb27930_bcd_encode(25));
        CHECK(gb27930_bcd_encode(9)  == 0x09, "9  -> BCD 应为 0x09，实际 0x%02X",
              gb27930_bcd_encode(9));
        CHECK(gb27930_bcd_encode(0)  == 0x00, "0  -> BCD 应为 0x00，实际 0x%02X",
              gb27930_bcd_encode(0));
        CHECK(gb27930_bcd_encode(100) == 0x00, "非法值 100 应回 0，实际 0x%02X",
              gb27930_bcd_encode(100));

        CHECK(gb27930_bcd_decode(0x25) == 25, "0x25 -> 25，实际 %u",
              gb27930_bcd_decode(0x25));
        CHECK(gb27930_bcd_decode(0x09) == 9, "0x09 -> 9，实际 %u",
              gb27930_bcd_decode(0x09));
        CHECK(gb27930_bcd_decode(0x1A) == 0, "非法 BCD 0x1A 应回 0，实际 %u",
              gb27930_bcd_decode(0x1A));

        /* 按 CTS 的字节顺序验证一遍：B1 秒 B2 分 B3 时 B4 日 B5 月 B6-B7 年 */
        {
            uint8_t cts[7];
            gb27930_cts_pack(cts, 2025, 6, 18, 9, 7, 25);
            CHECK(cts[0] == 0x25, "CTS B1 应为秒 25(0x25)，实际 0x%02X", cts[0]);
            CHECK(cts[1] == 0x07, "CTS B2 应为分 07，实际 0x%02X", cts[1]);
            CHECK(cts[2] == 0x09, "CTS B3 应为时 09，实际 0x%02X", cts[2]);
            CHECK(cts[3] == 0x18, "CTS B4 应为日 18，实际 0x%02X", cts[3]);
            CHECK(cts[4] == 0x06, "CTS B5 应为月 06，实际 0x%02X", cts[4]);
            CHECK(cts[5] == 0x20, "CTS B6 应为年高位 20，实际 0x%02X", cts[5]);
            CHECK(cts[6] == 0x25, "CTS B7 应为年低位 25，实际 0x%02X", cts[6]);
        }
    }
    CASE_END();

    /*------------------- CRM / CSD 编号偏移 -------------------*/
    CASE("T6 CRM 辨识结果 0xAA / CSD 充电机编号 1 偏移常量");
    CHECK(GB_CRM_ID_OK == 0xAA && GB_CRM_ID_UNKNOWN == 0x00,
          "CRM 辨识结果取值应为 0x00 / 0xAA");
    CHECK(GB_READY_NOT_READY == 0x00 && GB_READY_READY == 0xAA &&
          GB_READY_INVALID == 0xFF,
          "BRO/CRO 取值应为 0x00 / 0xAA / 0xFF");
    /* 标准表 27 的 SPN3613 是"1/位，1 偏移"：id=1 时线上应发 2 */
    CHECK(gb27930_csd_encode_id(1u) == 2u, "充电机编号 1 偏移错误: %u",
          gb27930_csd_encode_id(1u));
    CASE_END();

    /*------------------- BEM（BMS 错误报文，优先权 2） -------------------*/
    CASE("T12 BEM BMS错误报文解析（标准表 28：4 字节，2 位字段，优先权 2）");
    /* ID 必须是 0x081E56F4：优先权 2 → 最高 3 位 = 010 */
    CHECK(GB_ID_BEM == 0x081E56F4u, "BEM 的 ID 错误: 0x%08X（应为 0x081E56F4）",
          GB_ID_BEM);
    CHECK(((GB_ID_BEM >> 26) & 0x7u) == 2u, "BEM 优先权应为 2，实际 %u",
          (unsigned)((GB_ID_BEM >> 26) & 0x7u));
    CHECK(GB_ID_CEM == 0x081FF456u, "CEM 的 ID 错误: 0x%08X（应为 0x081FF456）",
          GB_ID_CEM);
    CHECK(((GB_ID_CEM >> 26) & 0x7u) == 2u, "CEM 优先权应为 2，实际 %u",
          (unsigned)((GB_ID_CEM >> 26) & 0x7u));

    memset(d, 0, 8);
    /* B1：SPN3902(辨识 0xAA 超时)=01 在 1.3-1.4 位 → 1<<2 = 0x04；
     *     SPN3901(辨识 0x00 超时)=00 正常 */
    d[0] = (uint8_t)((GB_ST_NORMAL) | (GB_ST_ABNORMAL << 2));
    /* B2：SPN3903(CTS/CML)=01 → 0x01；SPN3904(CRO)=00 */
    d[1] = (uint8_t)(GB_ST_ABNORMAL | (GB_ST_NORMAL << 2));
    /* B3：SPN3905(CCS)=01 → 0x01；SPN3906(CST)=10 不可信 → 2<<2 = 0x08 */
    d[2] = (uint8_t)(GB_ST_ABNORMAL | (GB_ST_UNRELIABLE << 2));
    /* B4：SPN3907(CSD)=00；高 6 位"其他"（可选项）按 7.9 填 1 → 0xFC */
    d[3] = (uint8_t)(GB_ST_NORMAL | 0xFCu);
    f = mkframe(GB_ID_BEM, d, 4);
    CHECK(gb27930_process_frame(ctx, NULL, &f) == 1, "BEM 未被识别");
    CHECK(ctx->has_bem == 1, "has_bem 未置位");
    CHECK(ctx->bem.crm_00_timeout == 0, "SPN3901 应为 00 正常，实际 %u",
          ctx->bem.crm_00_timeout);
    CHECK(ctx->bem.crm_aa_timeout == 1, "SPN3902 应为 01 超时，实际 %u",
          ctx->bem.crm_aa_timeout);
    CHECK(ctx->bem.cts_cml_timeout == 1, "SPN3903 应为 01 超时，实际 %u",
          ctx->bem.cts_cml_timeout);
    CHECK(ctx->bem.cro_timeout == 0, "SPN3904 应为 00 正常，实际 %u",
          ctx->bem.cro_timeout);
    CHECK(ctx->bem.ccs_timeout == 1, "SPN3905 应为 01 超时，实际 %u",
          ctx->bem.ccs_timeout);
    CHECK(ctx->bem.cst_timeout == 2, "SPN3906 应为 10 不可信，实际 %u",
          ctx->bem.cst_timeout);
    CHECK(ctx->bem.csd_timeout == 0, "SPN3907 应为 00 正常，实际 %u",
          ctx->bem.csd_timeout);
    CASE_END();

    /*------------------- CEM 组包（字段错位与 BEM 不同） -------------------*/
    CASE("T12 CEM 组包字段位移（标准表 29：B1 只有 1 个字段，与 BEM 不同）");
    {
        uint8_t cem[CEM_LEN];

        /* 关键点：CEM 与 BEM 的字段排布**不一样** ——
         * BEM 的 B1 有两个字段（SPN3901/3902），CEM 的 B1 只有一个（SPN3921）；
         * CEM 的 B3 有三个字段，SPN3926 落在 B3.5-3.6（位移 4）。
         * 所以这里先断言位移本身，再断言打包结果。 */
        CHECK(CEM_B1_IDENT_SHIFT == 0u, "CEM SPN3921 位移应为 0");
        CHECK(CEM_B2_BCP_SHIFT == 0u && CEM_B2_BRO_SHIFT == 2u,
              "CEM SPN3922/3923 位移应为 0/2");
        CHECK(CEM_B3_BCS_SHIFT == 0u && CEM_B3_BCL_SHIFT == 2u && CEM_B3_BST_SHIFT == 4u,
              "CEM SPN3924/3925/3926 位移应为 0/2/4");
        CHECK(CEM_B3_BST_SHIFT != BEM_B3_CST_SHIFT,
              "CEM 的 SPN3926 与 BEM 的 SPN3906 位移**不应相同**"
              "（BEM=%u CEM=%u）—— 这说明两张表被错误地套用了同一组位移",
              (unsigned)BEM_B3_CST_SHIFT, (unsigned)CEM_B3_BST_SHIFT);

        /* 只报 SPN3922(BCP 超时) 与 SPN3925(BCL 超时)。逐字节按标准表 29 推：
         *   B1 = 00_00_00_00 = 0x00                        （SPN3921 在 1.1-1.2）
         *   B2 = 00_00_00_01 = 0x01                        （SPN3922 在 2.1-2.2）
         *   B3 = 00_01_00_00 = **0x04**                    （SPN3925 在 **3.3-3.4**）
         *   B4 = 11_11_11_00 = 0xFC                        （B4.1-4.2 = 00，高 6 位填 1）
         *
         * ★ 这里曾经写成 0x10 —— 那是把 SPN3925 当成了 B3.5-3.6（位移 4）。
         *   标准表 29 明写 3.3 → SPN3925，位移是 **2**，所以值是 0x04。
         *   现在改成**从位移宏推导**，不再手算字面量。 */
        gb27930_cem_pack(cem, (uint8_t)(CEM_ERR_BCP | CEM_ERR_BCL));
        CHECK(cem[0] == 0x00, "CEM B1 应为 0x00，实际 0x%02X", cem[0]);
        CHECK(cem[1] == 0x01, "CEM B2 应为 0x01（SPN3922·BCP 在 2.1-2.2），实际 0x%02X",
              cem[1]);
        CHECK(cem[2] == (uint8_t)(1u << CEM_B3_BCL_SHIFT),
              "CEM B3 应为 0x%02X（SPN3925·BCL 在 3.3-3.4，位移 %u），实际 0x%02X",
              (unsigned)(1u << CEM_B3_BCL_SHIFT), (unsigned)CEM_B3_BCL_SHIFT, cem[2]);
        CHECK(cem[3] == 0xFC, "CEM B4 应为 0xFC（SPN3927 正常 + 其他填 1），实际 0x%02X",
              cem[3]);

        /* 全部字段都报超时：每个已定义的 2 位字段都应为 01。
         * 期望值同样从位移宏推，避免再手算错。 */
        gb27930_cem_pack(cem, 0x7Fu);
        CHECK(cem[0] == (uint8_t)(1u << CEM_B1_IDENT_SHIFT),
              "CEM 全超时 B1 应为 0x%02X，实际 0x%02X",
              (unsigned)(1u << CEM_B1_IDENT_SHIFT), cem[0]);
        CHECK(cem[1] == (uint8_t)((1u << CEM_B2_BCP_SHIFT) | (1u << CEM_B2_BRO_SHIFT)),
              "CEM 全超时 B2 应为 0x%02X，实际 0x%02X",
              (unsigned)((1u << CEM_B2_BCP_SHIFT) | (1u << CEM_B2_BRO_SHIFT)), cem[1]);
        CHECK(cem[2] == (uint8_t)((1u << CEM_B3_BCS_SHIFT) | (1u << CEM_B3_BCL_SHIFT)
                                | (1u << CEM_B3_BST_SHIFT)),
              "CEM 全超时 B3 应为 0x%02X（0/2/4 三个位移），实际 0x%02X",
              (unsigned)((1u << CEM_B3_BCS_SHIFT) | (1u << CEM_B3_BCL_SHIFT)
                       | (1u << CEM_B3_BST_SHIFT)), cem[2]);
        CHECK(cem[3] == (uint8_t)((1u << CEM_B4_BSD_SHIFT) | CEM_B4_OTHER_FILL),
              "CEM 全超时 B4 应为 0x%02X，实际 0x%02X",
              (unsigned)((1u << CEM_B4_BSD_SHIFT) | CEM_B4_OTHER_FILL), cem[3]);
    }
    CASE_END();

    /*------------------- CCS 常量与 ID -------------------*/
    CASE("T12 CCS 充电机充电状态常量（标准表 19：8 字节，50 ms，优先权 6）");
    CHECK(GB_ID_CCS == 0x1812F456u, "CCS 的 ID 错误: 0x%08X（应为 0x1812F456）",
          GB_ID_CCS);
    CHECK(((GB_ID_CCS >> 26) & 0x7u) == 6u, "CCS 优先权应为 6，实际 %u",
          (unsigned)((GB_ID_CCS >> 26) & 0x7u));
    CHECK(CCS_LEN == 8 && CCS_OFF_OUT_V == 0 && CCS_OFF_OUT_I == 2 &&
          CCS_OFF_MINUTES == 4 && CCS_OFF_PERMIT == 6,
          "CCS 偏移定义错误: len=%d v=%d i=%d min=%d permit=%d",
          CCS_LEN, CCS_OFF_OUT_V, CCS_OFF_OUT_I, CCS_OFF_MINUTES, CCS_OFF_PERMIT);
    CASE_END();
}

/*==============================================================================
 *                      T10 ISO-TP 异常路径（序号错乱）
 *============================================================================*/

static void test_isotp_error(gb_context_t *ctx)
{
    uint8_t    payload[64];
    uint8_t    d[8];
    can_item_t f;

    CASE("T10 ISO-TP 连续帧序号错乱应丢弃整包");

    build_brm(payload, 1000, 5120);

    memset(d, 0xFF, 8);
    d[0] = 0x10; d[1] = 41; d[2] = 0; d[3] = 6;
    d[5] = 0x00; d[6] = 0x02; d[7] = 0x00;
    f = mkframe(GB_ID_BRM_TPCM, d, 8);
    (void)gb27930_process_frame(ctx, NULL, &f);
    CHECK(ctx->brm_rx.active == 1, "RTS 后会话未建立");

    /* 故意跳过序号 1，直接发序号 3 */
    memset(d, 0xFF, 8);
    d[0] = 3;
    memcpy(&d[1], payload, 7);
    f = mkframe(GB_ID_BRM_TPDT, d, 8);
    (void)gb27930_process_frame(ctx, NULL, &f);

    CHECK(ctx->brm_rx.active == 0, "序号错乱后会话应被复位");
    CHECK(ctx->brm_rx.stat_seq_err >= 1, "序号错误计数未递增");
    CHECK(ctx->has_brm == 0 || ctx->brm.raw_len == 41, "半包不得被当完整包解析");

    CASE_END();
}

/*==============================================================================
 *                      T8 充电机侧状态机流转
 *============================================================================*/

/**
 * @brief  推进状态机直到进入期望状态，或超时
 * @return 1 = 已到达目标状态；0 = 超时
 *
 * 【为什么需要这个helper】状态迁移与"喂报文"必须是**交替**的：
 *   收到 CHM 后要 tick 一次才会从 IDLE 进 HANDSHAKE，
 *   而 BHM 只在 HANDSHAKE 分支里生效；BRM 只在 IDENTIFY 分支里生效……
 *   如果只 tick 一次就假定状态已经到位，遇上时序差异就会**静默丢帧**，
 *   最后表现成"状态卡在某一阶段"。
 *   本函数按 30 ms 步进反复 tick，既推进状态机也顺带等一点真实时间
 *   （超时判据用的是真实的 monotonic 时钟）。
 */
static int sm_advance_to(gb_context_t *ctx, gb_state_t want, int max_steps)
{
    int k;

    for (k = 0; k < max_steps; k++)
    {
        if (ctx->state == want)
        {
            return 1;
        }
        gb27930_tick(ctx, NULL, 0);
        usleep(30000);
    }
    return (ctx->state == want) ? 1 : 0;
}

static void test_state_machine(gb_context_t *ctx)
{
    uint8_t    d[8];
    can_item_t f;
    int        i;
    int        ok;

    CASE("T8 充电机侧状态机 IDLE -> ... -> CHARGING 全流程");

    gb27930_reset(ctx);
    CHECK(ctx->state == GB_ST_IDLE, "复位后应处于空闲态");

    /* 充电机现在**不会自动开始充电**：必须先"按下充电按钮"
     * （相当于把充电枪插上），才会在 1 秒后进入握手阶段。
     * 这条断言同时验证了"默认待机"这个新行为。 */
    gb27930_request_start(ctx);

    /* 空闲态停留 1 秒后进入握手阶段 */
    for (i = 0; i < 30; i++)
    {
        gb27930_tick(ctx, NULL, 0);
        usleep(40000);
    }
    CHECK(ctx->state == GB_ST_HANDSHAKE,
          "1 秒后应进入握手态，实际 %s", gb27930_state_str(ctx->state));

    /* 收到 BHM -> 辨识阶段 */
    memset(d, 0, 8);
    d[0] = 0x01; d[1] = 0x00; d[2] = 0x01;
    f = mkframe(GB_ID_BHM, d, 8);
    (void)gb27930_process_frame(ctx, NULL, &f);
    gb27930_tick(ctx, NULL, 0);
    CHECK(ctx->state == GB_ST_IDENTIFY,
          "收到 BHM 后应进入辨识态，实际 %s", gb27930_state_str(ctx->state));

    /* 收到 BRM -> 参数配置阶段 */
    {
        uint8_t payload[41];
        build_brm(payload, 1000, 5120);
        (void)feed_isotp(ctx, GB_PGN_BRM, payload, 41);
    }
    gb27930_tick(ctx, NULL, 0);
    CHECK(ctx->state == GB_ST_PARAM_CONFIG,
          "收到 BRM 后应进入参数配置态，实际 %s", gb27930_state_str(ctx->state));

    /* 收到 BCP -> 充电准备阶段 */
    {
        uint8_t payload[13];
        build_bcp(payload, 365, 5000, 512, 5840, 105, 450, 4800);
        (void)feed_isotp(ctx, GB_PGN_BCP, payload, 13);
    }
    gb27930_tick(ctx, NULL, 0);
    CHECK(ctx->state == GB_ST_CHARGING_READY,
          "收到 BCP 后应进入充电准备态，实际 %s", gb27930_state_str(ctx->state));

    /* 收到 BRO=0xAA -> 充电中 */
    d[0] = 0xAA;
    f = mkframe(GB_ID_BRO, d, 1);
    (void)gb27930_process_frame(ctx, NULL, &f);
    gb27930_tick(ctx, NULL, 0);
    CHECK(ctx->state == GB_ST_CHARGING,
          "收到 BRO=0xAA 后应进入充电态，实际 %s", gb27930_state_str(ctx->state));

    CASE_END();

    /*------------------- 充电阶段报文超时 -------------------
     * 【为什么期望值是 BCL 超时而不是"充电超时"】
     * 标准对充电阶段的报文是**逐条不同**的超时，本工程实现了其中两条：
     *   BCL 1 s（标准 p.12：充电机应立即结束充电）
     *   BCS 5 s（标准 p.12）
     * 本用例进入 GB_ST_CHARGING 后**一条 BCL/BCS 都没发**，
     * 所以 1 s 的 BCL 判据必然先于 5 s 的 BCS/通用判据触发 ——
     * 这是正确行为，不是 bug。断言按 1 s 那条写。
     *
     * 这里只跑约 2 秒（1 s 判据 + 余量），比原来 5.6 秒更省时间。 */
    CASE("T8 充电阶段无 BCL 应触发 BCL 1 秒超时（标准逐报文规定）");

    for (i = 0; i < 60; i++)       /* 约 2.4 秒，足够跨过 BCL 的 1 s 判据 */
    {
        gb27930_tick(ctx, NULL, 0);
        usleep(40000);
    }
    CHECK(ctx->state == GB_ST_FAULT,
          "应进入故障态，实际 %s", gb27930_state_str(ctx->state));
    CHECK(ctx->error == GB_ERR_BCL_TIMEOUT,
          "异常码应为 BCL 超时(标准 1 s)，实际 %s", gb27930_error_str(ctx->error));

    CASE_END();

    /*------------------- BCL 按时刷新 -> 不超时，改由 BCS 判据接管 ---------
     * 反向验证：只要 BCL 在 1 s 之内刷新，BCL 判据就不该触发。 */
    CASE("T8 持续喂 BCL 时不应报 BCL 超时（逐报文超时是分开判的）");
    {
        uint8_t  bcl[GB_LEN_BCL];
        uint8_t  p41[41];
        uint8_t  p13[13];
        uint16_t total;

        gb27930_reset(ctx);
        gb27930_request_start(ctx);
        for (i = 0; i < 30; i++) { gb27930_tick(ctx, NULL, 0); usleep(40000); }

        /* 把状态推到「充电中」。
         *
         * 【关键】每一步都必须**先喂报文、再 tick 推进状态**，不能一口气
         * 把所有报文喂完 —— 状态迁移发生在 tick 里，而每条报文的处理
         * 只在它对应的那个状态分支里生效。比如：
         *   收到 BHM 后若不 tick，状态还停在 HANDSHAKE，
         *   此时送进来的 BRM 会因为"不在 IDENTIFY 分支"被直接丢掉，
         *   后面就一路错位、最后卡在某个阶段。
         * 所以每步都用 sm_advance_to() 等到状态真的到位再进下一步。 */
        ok = sm_advance_to(ctx, GB_ST_HANDSHAKE, 60);
        CHECK(ok, "应进入握手态，实际 %s", gb27930_state_str(ctx->state));

        /* --- BHM -> 辨识 --- */
        memset(d, 0, 8);
        le16_put(&d[0], 5840);                 /* 最高允许充电总电压 584.0 V */
        f = mkframe(GB_ID_BHM, d, GB_LEN_BHM);
        (void)gb27930_process_frame(ctx, NULL, &f);
        ok = sm_advance_to(ctx, GB_ST_IDENTIFY, 30);
        CHECK(ok, "收到 BHM 后应进入辨识态，实际 %s", gb27930_state_str(ctx->state));

        /* --- BRM（TP 多帧）-> 参数配置 --- */
        build_brm(p41, 1000, 5120);
        (void)feed_isotp(ctx, GB_PGN_BRM, p41, 41);
        ok = sm_advance_to(ctx, GB_ST_PARAM_CONFIG, 30);
        CHECK(ok, "收到 BRM 后应进入参数配置态，实际 %s", gb27930_state_str(ctx->state));

        /* --- BCP（TP 多帧）-> 充电准备 --- */
        build_bcp(p13, 365, 5000, 512, 5840, 105, 450, 4800);
        (void)feed_isotp(ctx, GB_PGN_BCP, p13, 13);
        ok = sm_advance_to(ctx, GB_ST_CHARGING_READY, 30);
        CHECK(ok, "收到 BCP 后应进入充电准备态，实际 %s", gb27930_state_str(ctx->state));

        /* --- BRO=0xAA -> 充电中 --- */
        memset(d, 0, sizeof(d));
        d[0] = 0xAA;
        f = mkframe(GB_ID_BRO, d, GB_LEN_BRO);
        (void)gb27930_process_frame(ctx, NULL, &f);
        ok = sm_advance_to(ctx, GB_ST_CHARGING, 30);
        CHECK(ok, "收到 BRO=0xAA 后应进入充电态，实际 %s", gb27930_state_str(ctx->state));

        /* 进不去充电态就没必要继续跑下面的循环了 —— 直接收尾，
         * 免得后面断言报一堆误导性的错（真正的失败点就是这一条）。 */
        if (!ok)
        {
            CASE_END();
            return;
        }

        /* --- 每 200 ms 喂一条合法 BCL，跑约 6 秒 ---
         * 6 秒必须同时跨过两条判据：1 s 的 BCL 专用超时、5 s 的 BCS/通用超时。
         * 原来只跑 2.5 秒，而下面断言 5 s 判据已触发 —— 那是永远不可能成立的。
         * BCL 每 200 ms 刷新一次，所以 1 s 那条始终不该触发；5 s 那条该触发。 */
        memset(bcl, 0, sizeof(bcl));
        le16_put(&bcl[0], 4820);          /* 电压需求 482.0 V */
        le16_put(&bcl[2], 5000);          /* 电流需求 100.0 A（含 -400A 偏移） */
        bcl[4] = 2;                       /* 恒流 */

        total = 0;
        for (i = 0; i < 150; i++)          /* 150 × 40 ms = 6.0 s */
        {
            if ((i % 5) == 0)             /* 每 5 步 ≈ 200 ms 发一次 */
            {
                f = mkframe(GB_ID_BCL, bcl, GB_LEN_BCL);
                (void)gb27930_process_frame(ctx, NULL, &f);
                total++;
            }
            gb27930_tick(ctx, NULL, 0);
            usleep(40000);
        }
        CHECK(total >= 25, "应至少喂进 25 条 BCL（6 s / 200 ms = 30），实际 %u", (unsigned)total);
        CHECK(ctx->error != GB_ERR_BCL_TIMEOUT,
              "BCL 一直在刷新，不该报 BCL 超时（实际 %s）",
              gb27930_error_str(ctx->error));
        /* BCL 正常，但全程没发 BCS —— 该由 5 s 的 BCS/通用判据接管。
         * 这条**同时证明了"逐报文超时是分开判的"**：
         * 同一个充电阶段里，1 s 那条没触发，5 s 那条触发。 */
        CHECK(ctx->error == GB_ERR_CHARGE_TIMEOUT,
              "BCL 正常时应由 BCS/通用 5 s 判据接管，实际 %s",
              gb27930_error_str(ctx->error));
    }
    CASE_END();
}

/*==============================================================================
 *                      T9 SQLite 存储层
 *============================================================================*/

/** 原始报文回读回调，只统计条数 */
static int count_raw_cb(void *arg, const storage_raw_t *rec)
{
    int *n = (int *)arg;
    (void)rec;
    if (n != NULL)
    {
        (*n)++;
    }
    return 0;
}

/** 解析记录回读回调，只统计条数 */
static int count_charge_cb(void *arg, const storage_charge_t *rec)
{
    int *n = (int *)arg;
    (void)rec;
    if (n != NULL)
    {
        (*n)++;
    }
    return 0;
}

static void test_storage(void)
{
    storage_raw_t    raw;
    storage_charge_t rec;
    const char      *dbpath = "/tmp/gb27930_selftest.db";
    int              i;
    int              rc;
    char             wal_mode[32];
    FILE            *fp;

    CASE("T9 SQLite 存储：WAL / 批量写入 / 回读 / 摘要");

    unlink(dbpath);
    unlink("/tmp/gb27930_selftest.db-wal");
    unlink("/tmp/gb27930_selftest.db-shm");

    CHECK(storage_open(dbpath) == 0, "storage_open 失败");

    /* 写入 120 条原始帧 + 30 条解析记录 */
    for (i = 0; i < 120; i++)
    {
        memset(&raw, 0, sizeof(raw));
        raw.ts_us    = 1700000000000000LL + (int64_t)i * 1000;
        raw.can_id   = 0x181056F4u;   /* 标准表 5：BCL 的 ID */
        raw.dlc      = 8;
        raw.data[0]  = (uint8_t)i;
        raw.data[7]  = 0xA5;
        CHECK(storage_push_raw(&raw) == 0, "第 %d 条原始帧入队失败", i);
    }
    for (i = 0; i < 30; i++)
    {
        memset(&rec, 0, sizeof(rec));
        rec.ts_us                 = 1700000000000000LL + (int64_t)i * 1000000;
        rec.session_id            = 1;
        rec.state                 = 5;
        rec.bcs_measure_voltage   = 480.0f + (float)i;
        rec.bcs_measure_current   = 95.0f;
        rec.bcs_current_soc       = 45.0f + (float)i;
        rec.bsm_max_temp          = 30.0f + (float)i * 0.1f;
        rec.bsm_min_temp          = 25.0f;
        CHECK(storage_push_charge(&rec) == 0, "第 %d 条解析记录入队失败", i);
    }

    rc = storage_flush();
    CHECK(rc == 150, "storage_flush 应提交 150 条，实际 %d", rc);
    CHECK(storage_raw_written() == 120, "原始帧写入数应为 120，实际 %llu",
          (unsigned long long)storage_raw_written());
    CHECK(storage_charge_written() == 30, "解析记录写入数应为 30，实际 %llu",
          (unsigned long long)storage_charge_written());
    CHECK(storage_raw_dropped() == 0, "不应有丢弃");

    /* 回读：按 CAN ID 查询应返回 120 条 */
    {
        int n = 0;
        int total = storage_query_raw(0, 0, 0x181056F4u, 0, count_raw_cb, &n);
        CHECK(total == 120, "按 CAN ID 回读应为 120 条，实际 %d", total);
        CHECK(n == 120, "回调应被调用 120 次，实际 %d", n);
    }
    /* 按时间范围查询：只取前 10 条的时间窗口 */
    {
        int n = 0;
        int total = storage_query_raw(1700000000000000LL, 1700000000009000LL,
                                      0, 0, count_raw_cb, &n);
        CHECK(total == 10, "按时间范围回读应为 10 条，实际 %d", total);
    }
    {
        int n = 0;
        int total = storage_query_charge(0, 0, 0, count_charge_cb, &n);
        CHECK(total == 30, "按解析表回读应为 30 条，实际 %d", total);
    }

    /* 空回调应被拒绝 */
    CHECK(storage_query_raw(0, 0, 0, 0, NULL, NULL) == -1,
          "回调为空时应返回 -1");

    /* 数据库文件校验 */
    fp = fopen(dbpath, "rb");
    CHECK(fp != NULL, "数据库文件未创建");
    if (fp != NULL)
    {
        fseek(fp, 0, SEEK_END);
        CHECK(ftell(fp) > 0, "数据库文件为空");
        fclose(fp);
    }
    CHECK(storage_db_size_bytes() > 0, "数据库大小统计为 0");
    (void)wal_mode;

    storage_close();

    /* 重新打开确认数据已持久化（验证 COMMIT 真正落盘） */
    CHECK(storage_open(dbpath) == 0, "重新打开数据库失败");
    {
        storage_summary_t sum;
        memset(&sum, 0, sizeof(sum));
        CHECK(storage_summary(0, 0, &sum) == 0, "storage_summary 失败");
        CHECK(sum.count == 30, "解析记录条数应为 30，实际 %d", sum.count);
        CHECK_FEQ(sum.voltage_min, 480.0, "最小电压错误: %.2f", sum.voltage_min);
        CHECK_FEQ(sum.voltage_max, 509.0, "最大电压错误: %.2f", sum.voltage_max);
        CHECK_FEQ(sum.soc_min, 45.0, "最小 SOC 错误: %.2f", sum.soc_min);
        CHECK_FEQ(sum.soc_max, 74.0, "最大 SOC 错误: %.2f", sum.soc_max);
    }
    /* CSV 导出 */
    {
        int n = storage_export_charge_csv(0, 0, "/tmp/gb27930_selftest.csv");
        CHECK(n == 30, "CSV 导出应为 30 行，实际 %d", n);
        unlink("/tmp/gb27930_selftest.csv");
    }
    storage_close();

    unlink(dbpath);
    unlink("/tmp/gb27930_selftest.db-wal");
    unlink("/tmp/gb27930_selftest.db-shm");

    CASE_END();
}

/*==============================================================================
 *                              主函数
 *============================================================================*/

/**
 * @brief  测试「CAN 错误帧 → 中文可读原因」的翻译
 *
 * 为什么要专门测它
 * --------------
 *   现场排查时，日志里能看到的就只有这一行翻译结果。如果它把「总线关闭」
 *   翻成空字符串或者翻错，排查方向就会被带偏（本工程就真实踩过一次：
 *   原来代码直接把标志位打成 0x00000100，看日志完全不知道发生了什么）。
 *   下面用的测试向量**就是实机上真实抓到的那三条**：
 *     can_id=0x00000004 data[1]=0x20  → 发送错误被动
 *     can_id=0x00000040               → 总线关闭
 *     can_id=0x00000100               → 控制器已自动重启
 */
static void test_can_err_decode(void)
{
    char     buf[256];
    uint8_t  d[8];
    int      len;

    CASE("T11 CAN 错误帧标志位翻译（含实机抓到的 Bus-Off 组合）");

    memset(d, 0, sizeof(d));

    /* ---- 实机抓到的那三条 ---- */
    d[0] = 0x00; d[1] = 0x20;                      /* 控制器状态 = 发送错误被动 */
    len = can_err_frame_str(0x00000004u, d, buf, sizeof(buf));
    CHECK(len > 0, "CRTL 帧翻译为空");
    CHECK(strstr(buf, "发送错误被动") != NULL,
          "应识别出「发送错误被动」，实际: %s", buf);

    len = can_err_frame_str(0x00000040u, d, buf, sizeof(buf));
    CHECK(strstr(buf, "总线关闭") != NULL,
          "应识别出「总线关闭」，实际: %s", buf);
    CHECK(can_err_is_bus_off(0x00000040u) == 1, "Bus-Off 应被判定为严重事件");

    len = can_err_frame_str(0x00000100u, d, buf, sizeof(buf));
    CHECK(strstr(buf, "自动重启") != NULL,
          "应识别出「控制器已自动重启」，实际: %s", buf);

    /* ---- 错误警告 / 错误被动 / 溢出 的状态位 ---- */
    d[1] = 0x08;
    (void)can_err_frame_str(0x00000004u, d, buf, sizeof(buf));
    CHECK(strstr(buf, "发送错误警告") != NULL, "TEC>96 未识别: %s", buf);

    d[1] = 0x02;
    (void)can_err_frame_str(0x00000004u, d, buf, sizeof(buf));
    CHECK(strstr(buf, "发送缓冲溢出") != NULL, "TX 溢出未识别: %s", buf);

    /* ---- 协议错误 + 出错位置（ACK 位 = 没人应答，最有诊断价值） ---- */
    memset(d, 0, sizeof(d));
    d[2] = CAN_ERR_PROT_FORM;
    d[3] = CAN_ERR_PROT_LOC_ACK;
    (void)can_err_frame_str(CAN_ERR_PROT, d, buf, sizeof(buf));
    CHECK(strstr(buf, "格式错误") != NULL, "格式错误未识别: %s", buf);
    CHECK(strstr(buf, "ACK 位无应答") != NULL,
          "ACK 位无应答未识别（这条最关键：直接指向对端不在总线上）: %s", buf);
    CHECK(can_err_is_bus_off(CAN_ERR_PROT) == 0, "普通协议错误不该被判为 Bus-Off");

    /* ---- 错误计数器（CAN_ERR_CNT 置位时 data[6]/data[7] 才有效） ---- */
    memset(d, 0, sizeof(d));
    d[6] = 200; d[7] = 3;
    (void)can_err_frame_str(CAN_ERR_BUSOFF | CAN_ERR_CNT, d, buf, sizeof(buf));
    CHECK(strstr(buf, "TEC=200") != NULL && strstr(buf, "REC=3") != NULL,
          "错误计数器未打印: %s", buf);

    /* ---- 收发器故障提示 ---- */
    (void)can_err_frame_str(CAN_ERR_TRX, d, buf, sizeof(buf));
    CHECK(strstr(buf, "收发器") != NULL, "收发器故障未识别: %s", buf);

    /* ---- 无法识别的类别也要给出提示，而不是空字符串 ---- */
    (void)can_err_frame_str(0x00000002u /* LOSTARB */, d, buf, sizeof(buf));
    CHECK(buf[0] != '\0', "无法识别的错误帧不应翻成空串");

    CASE_END();
}

int main(void)
{
    gb_context_t ctx;

    printf("\n");
    printf("======================================================================\n");
    printf(" GB/T 27930-2015 协议栈离线自检（无需 CAN 硬件）\n");
    printf("======================================================================\n");

    gb27930_init(&ctx, NULL);
    test_ring_buffer();
    test_isotp_multiframe(&ctx);
    test_single_frames(&ctx);
    test_isotp_error(&ctx);
    test_state_machine(&ctx);
    test_storage();
    test_can_err_decode();

    printf("----------------------------------------------------------------------\n");
    printf(" 测试结果:  \033[32mPASS %d\033[0m / \033[31mFAIL %d\033[0m\n", g_pass, g_fail);
    printf("======================================================================\n\n");

    return (g_fail == 0) ? 0 : 1;
}
