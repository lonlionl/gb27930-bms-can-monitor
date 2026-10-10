/**
 * @file    isotp.c
 * @brief   用户态轻量多帧传输层实现（ISO 15765-2 + SAE J1939-21 / GB/T 27930）
 *
 * 代码结构
 * --------
 *   [时间工具]      ts_diff_us / ts_add_ms
 *   [接收：J1939]   j1939_rx_cm() / j1939_rx_dt()
 *   [接收：ISO]     iso15765_rx()
 *   [发送状态机]    isotp_tx_poll() / isotp_tx_feed()
 *
 * GB/T 27930-2015 BRM 实例（41 字节，BMS 0xF4 -> 充电机 0x56）
 * ------------------------------------------------------------
 *   BMS  -> 充电机  TP.CM(0x1CEC56F4) : 10 29 00 06 FF FF FF FF
 *                                      ^^ ^^^^^ ^^
 *                                      |  |     └ 6 个数据包
 *                                      |  └ 41 字节总长（小端！J1939 规定低字节在前）
 *                                      └ RTS
 *   充电机 -> BMS   TP.CM(0x1CECF456) : 11 06 01 00 00 00 00 00
 *                                      ^^ ^^ ^^
 *                                      |  |  └ 从第 1 包开始
 *                                      |  └ 允许一次发 6 包
 *                                      └ CTS
 *   BMS  -> 充电机  TP.DT(0x1CEB56F4) : 01 <7 字节数据>
 *                    TP.DT             : 02 <7 字节数据>
 *                    ...
 *                    TP.DT             : 06 <6 字节数据>   (41 = 5*7 + 6)
 *   充电机 -> BMS   TP.CM             : 13 29 00 06 FF FF FF FF  (EndOfMsgACK)
 *
 * 注意：J1939 的报文长度 / PGN / 包间隔字段为 **小端**（低字节在前），
 *       这是 J1939-21 的协议规定。GB/T 27930-2015 标准 4.4 也规定
 *       「数据信息传输采用低字节先发送的格式」，两者**一致，并不相反** ——
 *       所以这一层的小端实现保持不变即可，也不存在"要区别对待"的问题。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "isotp.h"

#include <stdio.h>
#include <string.h>

/*==============================================================================
 *                              时间工具
 *============================================================================*/

/** 计算 a - b 的微秒差（有符号） */
static int64_t ts_diff_us(const struct timespec *a, const struct timespec *b)
{
    int64_t s  = (int64_t)a->tv_sec  - (int64_t)b->tv_sec;
    int64_t ns = (int64_t)a->tv_nsec - (int64_t)b->tv_nsec;
    return s * 1000000LL + ns / 1000LL;
}

/** a - b 的毫秒差 */
static int64_t ts_diff_ms(const struct timespec *a, const struct timespec *b)
{
    return ts_diff_us(a, b) / 1000LL;
}

/** 取单调时钟，避免系统时间被 NTP 调整时误判超时 */
static void ts_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

/** 解码 ISO 15765-2 的 STmin 字节，返回微秒数 */
static uint32_t isotp_decode_stmin(uint8_t stmin)
{
    if (stmin <= 0x7F)
    {
        return (uint32_t)stmin * 1000u;          /* 0x00~0x7F : 0~127 ms */
    }
    if (stmin >= 0xF1 && stmin <= 0xF9)
    {
        return (uint32_t)(stmin - 0xF0) * 100u;  /* 0xF1~0xF9 : 100~900 us */
    }
    return 127000u;                              /* 保留值按最大 127ms 处理 */
}

const char *isotp_status_str(isotp_status_t st)
{
    switch (st)
    {
        case ISOTP_OK:           return "OK";
        case ISOTP_INCOMPLETE:   return "INCOMPLETE";
        case ISOTP_COMPLETE:     return "COMPLETE";
        case ISOTP_ERR_PARAM:    return "ERR_PARAM";
        case ISOTP_ERR_SEQ:      return "ERR_SEQ(连续帧序号错乱)";
        case ISOTP_ERR_OVERFLOW: return "ERR_OVERFLOW(长度超限)";
        case ISOTP_ERR_TIMEOUT:  return "ERR_TIMEOUT(流控/组包超时)";
        case ISOTP_ERR_ABORT:    return "ERR_ABORT(对端中止)";
        case ISOTP_ERR_NOBUF:    return "ERR_NOBUF";
        case ISOTP_ERR_STATE:    return "ERR_STATE(状态机不允许)";
        default:                 return "UNKNOWN";
    }
}

/*==============================================================================
 *                              接收 — 初始化 / 复位
 *============================================================================*/

void isotp_rx_init(isotp_rx_t *rx, isotp_format_t fmt,
                   uint32_t cm_id, uint32_t dt_id, uint32_t timeout_ms)
{
    if (rx == NULL)
    {
        return;
    }
    memset(rx, 0, sizeof(*rx));
    rx->fmt        = fmt;
    rx->cm_id      = cm_id;
    rx->dt_id      = dt_id;
    rx->timeout_ms = (timeout_ms == 0) ? ISOTP_DEFAULT_TIMEOUT : timeout_ms;
}

void isotp_rx_reset(isotp_rx_t *rx)
{
    if (rx == NULL)
    {
        return;
    }
    rx->active    = 0;
    rx->total_len = 0;
    rx->recv_len  = 0;
    rx->next_seq  = 1;
    rx->bs        = 0;
    memset(rx->data, 0, sizeof(rx->data));
}

/*==============================================================================
 *                              接收 — J1939 TP
 *============================================================================*/

/**
 * @brief  处理 TP.CM 帧（连接管理：RTS / CTS / EndOfMsgACK / BAM / Abort）
 */
static isotp_status_t j1939_rx_cm(isotp_rx_t *rx, const can_item_t *item)
{
    uint8_t  ctrl;
    uint16_t total;

    rx->stat_frames++;
    ctrl = item->data[0];

    switch (ctrl)
    {
        case 0x10:   /* RTS —— 请求发送 */
        case 0x14:   /* BAM —— 广播通告（无需流控应答） */
        {
            /* J1939 报文长度：data[1] = 低字节，data[2] = 高字节（小端） */
            total = (uint16_t)(item->data[1] | ((uint16_t)item->data[2] << 8));

            if (total == 0 || total > ISOTP_MAX_PAYLOAD)
            {
                rx->stat_overflow++;
                isotp_rx_reset(rx);
                fprintf(stderr, "[ISOTP] RTS/BAM 长度 %u 非法，丢弃\n", total);
                return ISOTP_ERR_OVERFLOW;
            }

            rx->active    = 1;
            rx->total_len = total;
            rx->recv_len  = 0;
            rx->next_seq  = 1;
            rx->bs        = (ctrl == 0x14) ? 0xFF : item->data[3];  /* BAM 无需分块 */
            rx->src_addr  = item->can_id & 0xFF;
            rx->t_start   = item->ts;
            rx->t_last    = item->ts;

            /* 注意：真正的 CTS 应答由协议层在收到 RTS 后立即回发，
             * 因为 TP.CM 的 CAN ID 与 TP.DT 不同，本模块只负责组包。 */
            return ISOTP_INCOMPLETE;
        }

        case 0x11:   /* CTS —— 本模块作为接收方不应收到，忽略 */
        case 0x13:   /* EndOfMsgACK —— 本模块作为接收方不应收到，忽略 */
            return ISOTP_OK;

        case 0xFF:   /* Abort —— 对端中止 */
            if (rx->active)
            {
                fprintf(stderr, "[ISOTP] 对端发送 Abort，原因码 0x%02X\n", item->data[1]);
                isotp_rx_reset(rx);
                return ISOTP_ERR_ABORT;
            }
            return ISOTP_OK;

        default:
            return ISOTP_OK;
    }
}

/**
 * @brief  处理 TP.DT 帧（数据传输，data[0] 为 1 基序号，data[1..7] 为有效数据）
 */
static isotp_status_t j1939_rx_dt(isotp_rx_t *rx, const can_item_t *item)
{
    uint8_t  seq = item->data[0];
    uint16_t copy;

    rx->stat_frames++;

    if (!rx->active)
    {
        /* 孤立的 DT 帧（无对应 RTS），静默忽略，避免刷屏 */
        return ISOTP_OK;
    }

    if (seq != rx->next_seq)
    {
        rx->stat_seq_err++;
        fprintf(stderr, "[ISOTP] DT 序号错乱：期望 %u，实收 %u，丢弃整包\n",
                rx->next_seq, seq);
        isotp_rx_reset(rx);
        return ISOTP_ERR_SEQ;
    }

    copy = (uint16_t)(rx->total_len - rx->recv_len);
    if (copy > 7u)
    {
        copy = 7u;
    }
    if (item->dlc < 8u)
    {
        /* 数据不足 8 字节时按实际长度截断，做防御性处理 */
        uint16_t avail = (item->dlc > 1u) ? (uint16_t)(item->dlc - 1u) : 0u;
        if (copy > avail)
        {
            copy = avail;
        }
    }

    memcpy(&rx->data[rx->recv_len], &item->data[1], copy);
    rx->recv_len = (uint16_t)(rx->recv_len + copy);

    /* J1939 序号范围 1~255，255 之后回绕到 1 */
    rx->next_seq = (seq == 0xFFu) ? 1u : (uint8_t)(seq + 1u);
    rx->t_last   = item->ts;

    if (rx->recv_len >= rx->total_len)
    {
        rx->active = 0;
        rx->stat_sessions++;
        return ISOTP_COMPLETE;
    }

    return ISOTP_INCOMPLETE;
}

/*==============================================================================
 *                              接收 — ISO 15765-2
 *============================================================================*/

static isotp_status_t iso15765_rx(isotp_rx_t *rx, const can_item_t *item)
{
    uint8_t  pci  = item->data[0];
    uint8_t  type = (uint8_t)((pci >> 4) & 0x0F);
    uint16_t copy;

    rx->stat_frames++;

    switch (type)
    {
        case 0x0:   /* --- 单帧 SF --- */
        {
            uint16_t len = (uint16_t)(pci & 0x0F);
            if (len > 7u)
            {
                len = 7u;
            }
            memcpy(rx->data, &item->data[1], len);
            rx->total_len = len;
            rx->recv_len  = len;
            rx->active    = 0;
            rx->stat_sessions++;
            return ISOTP_COMPLETE;
        }

        case 0x1:   /* --- 首帧 FF --- */
        {
            uint16_t total = (uint16_t)(((uint16_t)(pci & 0x0F) << 8) | item->data[1]);

            if (total > ISOTP_MAX_PAYLOAD)
            {
                rx->stat_overflow++;
                isotp_rx_reset(rx);
                return ISOTP_ERR_OVERFLOW;
            }

            rx->active    = 1;
            rx->total_len = total;
            rx->recv_len  = 0;
            rx->next_seq  = 1;
            rx->bs        = 0;
            rx->t_start   = item->ts;
            rx->t_last    = item->ts;

            copy = (total < 6u) ? total : 6u;
            memcpy(rx->data, &item->data[2], copy);
            rx->recv_len = copy;

            if (rx->recv_len >= rx->total_len)
            {
                rx->active = 0;
                rx->stat_sessions++;
                return ISOTP_COMPLETE;
            }
            return ISOTP_INCOMPLETE;
        }

        case 0x2:   /* --- 连续帧 CF --- */
        {
            uint8_t seq = (uint8_t)(pci & 0x0F);

            if (!rx->active)
            {
                return ISOTP_OK;   /* 孤立 CF，忽略 */
            }
            if (seq != (rx->next_seq & 0x0F))
            {
                rx->stat_seq_err++;
                fprintf(stderr, "[ISOTP] CF 序号错乱：期望 %u，实收 %u\n",
                        rx->next_seq & 0x0F, seq);
                isotp_rx_reset(rx);
                return ISOTP_ERR_SEQ;
            }

            copy = (uint16_t)(rx->total_len - rx->recv_len);
            if (copy > 7u)
            {
                copy = 7u;
            }
            memcpy(&rx->data[rx->recv_len], &item->data[1], copy);
            rx->recv_len = (uint16_t)(rx->recv_len + copy);
            rx->next_seq = (uint8_t)((rx->next_seq + 1u) & 0x0F);
            rx->t_last   = item->ts;

            if (rx->recv_len >= rx->total_len)
            {
                rx->active = 0;
                rx->stat_sessions++;
                return ISOTP_COMPLETE;
            }
            return ISOTP_INCOMPLETE;
        }

        case 0x3:   /* --- 流控帧 FC：接收方向不应出现 --- */
        default:
            return ISOTP_OK;
    }
}

/*==============================================================================
 *                              接收 — 统一入口
 *============================================================================*/

isotp_status_t isotp_rx_feed(isotp_rx_t *rx, const can_item_t *item)
{
    if (rx == NULL || item == NULL)
    {
        return ISOTP_ERR_PARAM;
    }
    if (item->is_error || item->dlc == 0)
    {
        return ISOTP_OK;
    }

    if (rx->fmt == ISOTP_FMT_J1939TP)
    {
        if (item->can_id == rx->cm_id)
        {
            return j1939_rx_cm(rx, item);
        }
        if (item->can_id == rx->dt_id)
        {
            return j1939_rx_dt(rx, item);
        }
        return ISOTP_OK;
    }

    /* ISO 15765-2：FF 与 CF 使用同一 CAN ID（物理寻址），
     * 为兼容 J1939 风格的双 ID 设计，这里两个 ID 都接受。 */
    if (item->can_id == rx->cm_id || item->can_id == rx->dt_id)
    {
        return iso15765_rx(rx, item);
    }
    return ISOTP_OK;
}

isotp_status_t isotp_rx_tick(isotp_rx_t *rx, const struct timespec *now)
{
    struct timespec t;

    if (rx == NULL || !rx->active)
    {
        return ISOTP_OK;
    }

    if (now != NULL)
    {
        t = *now;
    }
    else
    {
        ts_now(&t);
    }

    if (ts_diff_ms(&t, &rx->t_last) > (int64_t)rx->timeout_ms)
    {
        rx->stat_timeouts++;
        fprintf(stderr, "[ISOTP] 组包超时（已收 %u/%u 字节），丢弃半包\n",
                rx->recv_len, rx->total_len);
        isotp_rx_reset(rx);
        return ISOTP_ERR_TIMEOUT;
    }

    return ISOTP_OK;
}

/*==============================================================================
 *                              发送侧
 *============================================================================*/

void isotp_tx_init(isotp_tx_t *tx, isotp_format_t fmt,
                   uint32_t cm_id, uint32_t dt_id, uint32_t timeout_ms)
{
    if (tx == NULL)
    {
        return;
    }
    memset(tx, 0, sizeof(*tx));
    tx->fmt        = fmt;
    tx->cm_id      = cm_id;
    tx->dt_id      = dt_id;
    tx->timeout_ms = (timeout_ms == 0) ? ISOTP_DEFAULT_TIMEOUT : timeout_ms;
    tx->state      = ISOTP_TX_IDLE;
}

isotp_status_t isotp_tx_start(isotp_tx_t *tx, const uint8_t *payload, uint16_t len)
{
    if (tx == NULL || payload == NULL || len == 0)
    {
        return ISOTP_ERR_PARAM;
    }
    if (len > ISOTP_MAX_PAYLOAD)
    {
        return ISOTP_ERR_OVERFLOW;
    }
    if (tx->state == ISOTP_TX_WAIT_FC || tx->state == ISOTP_TX_SENDING)
    {
        return ISOTP_ERR_STATE;   /* 上一次传输尚未结束 */
    }

    tx->payload    = payload;
    tx->total_len  = len;
    tx->sent_len   = 0;
    tx->next_seq   = 1;
    tx->bs         = 0;
    tx->stmin_ms   = 0;
    tx->stmin_us   = 0;
    tx->block_sent = 0;
    tx->ff_sent    = 0;
    tx->retry      = 0;
    tx->state      = ISOTP_TX_WAIT_FC;
    ts_now(&tx->t_last);

    return ISOTP_OK;
}

/** 构造并发送首帧（ISO FF）或请求发送帧（J1939 RTS） */
static int isotp_send_first_frame(isotp_tx_t *tx, isotp_send_fn send, void *arg)
{
    uint8_t d[8];
    uint16_t remain = (uint16_t)(tx->total_len - tx->sent_len);

    memset(d, 0xFF, sizeof(d));

    if (tx->fmt == ISOTP_FMT_J1939TP)
    {
        /* J1939-21 RTS：10 <总长低字节> <总长高字节> <包数> FF FF FF FF
         * 长度字段为小端；GB/T 27930-2015 标准 4.4 的数据域同样是
         * 低字节先发送，故两者一致，此处无需特殊换算。 */
        uint16_t pkts = (uint16_t)((tx->total_len + 6u) / 7u);
        d[0] = 0x10;
        d[1] = (uint8_t)(tx->total_len & 0xFF);
        d[2] = (uint8_t)((tx->total_len >> 8) & 0xFF);
        d[3] = (uint8_t)(pkts & 0xFF);
        d[4] = 0xFF;
        d[5] = 0xFF;
        d[6] = 0xFF;
        d[7] = 0xFF;
    }
    else
    {
        /* ISO 15765-2 FF：1 <总长高 4 位> <总长低 8 位> <数据 6 字节> */
        uint8_t n = (remain < 6u) ? (uint8_t)remain : 6u;
        d[0] = (uint8_t)(0x10 | ((tx->total_len >> 8) & 0x0F));
        d[1] = (uint8_t)(tx->total_len & 0xFF);
        memcpy(&d[2], &tx->payload[tx->sent_len], n);
        tx->sent_len = (uint16_t)(tx->sent_len + n);
    }

    if (send(arg, tx->cm_id, d, 8) != 0)
    {
        return -1;
    }
    tx->stat_frames++;
    return 0;
}

/** 发送一帧连续帧（ISO CF）或数据传送帧（J1939 DT） */
static int isotp_send_cf(isotp_tx_t *tx, isotp_send_fn send, void *arg)
{
    uint8_t  d[8];
    uint16_t remain = (uint16_t)(tx->total_len - tx->sent_len);
    uint8_t  n = (remain < 7u) ? (uint8_t)remain : 7u;

    memset(d, 0xFF, sizeof(d));

    if (tx->fmt == ISOTP_FMT_J1939TP)
    {
        d[0] = tx->next_seq;                       /* J1939 序号 1~255 */
        memcpy(&d[1], &tx->payload[tx->sent_len], n);
        /* J1939 未用字节填充 0xFF（协议规定） */
    }
    else
    {
        d[0] = (uint8_t)(0x20 | (tx->next_seq & 0x0F));   /* ISO 序号 0~15 */
        memcpy(&d[1], &tx->payload[tx->sent_len], n);
        if (n < 7u)
        {
            memset(&d[1 + n], 0x00, (size_t)(7u - n));    /* ISO 未用字节填充 0x00 */
        }
    }

    if (send(arg, tx->dt_id, d, 8) != 0)
    {
        return -1;
    }

    tx->sent_len = (uint16_t)(tx->sent_len + n);
    tx->next_seq = (tx->fmt == ISOTP_FMT_J1939TP)
                 ? ((tx->next_seq == 0xFFu) ? 1u : (uint8_t)(tx->next_seq + 1u))
                 : (uint8_t)((tx->next_seq + 1u) & 0x0F);
    tx->block_sent++;
    tx->stat_frames++;
    return 0;
}

isotp_status_t isotp_tx_poll(isotp_tx_t *tx, const struct timespec *now,
                             isotp_send_fn send, void *arg)
{
    struct timespec t;
    int64_t elapsed_us;
    uint32_t stmin_us;

    if (tx == NULL || send == NULL)
    {
        return ISOTP_ERR_PARAM;
    }

    if (tx->state == ISOTP_TX_DONE)
    {
        return ISOTP_COMPLETE;
    }
    if (tx->state == ISOTP_TX_ABORTED)
    {
        return ISOTP_ERR_ABORT;
    }
    if (tx->state == ISOTP_TX_IDLE)
    {
        return ISOTP_ERR_STATE;
    }

    if (now != NULL)
    {
        t = *now;
    }
    else
    {
        ts_now(&t);
    }

    /*---------------- 状态一：等待流控帧（FC / CTS） ----------------*/
    if (tx->state == ISOTP_TX_WAIT_FC)
    {
        if (!tx->ff_sent)
        {
            if (isotp_send_first_frame(tx, send, arg) != 0)
            {
                tx->state = ISOTP_TX_ABORTED;
                return ISOTP_ERR_TIMEOUT;
            }
            tx->ff_sent = 1;
            tx->t_last  = t;
            return ISOTP_INCOMPLETE;
        }

        /* 已在等待：检查是否超时，超时则重发首帧 */
        if (ts_diff_ms(&t, &tx->t_last) > (int64_t)tx->timeout_ms)
        {
            if (tx->retry < ISOTP_MAX_RETRY)
            {
                tx->retry++;
                tx->stat_retry++;
                tx->ff_sent = 0;   /* 触发重发 */
                tx->t_last  = t;
                fprintf(stderr, "[ISOTP] 等待流控帧超时，第 %u 次重试\n", tx->retry);
                return ISOTP_INCOMPLETE;
            }
            tx->state = ISOTP_TX_ABORTED;
            fprintf(stderr, "[ISOTP] 等待流控帧超时，重试耗尽，传输失败\n");
            return ISOTP_ERR_TIMEOUT;
        }
        return ISOTP_INCOMPLETE;
    }

    /*---------------- 状态二：正在发送连续帧 ----------------*/
    if (tx->state == ISOTP_TX_SENDING)
    {
        /* 遵守对端要求的 STmin 最小间隔 */
        stmin_us = (uint32_t)tx->stmin_ms * 1000u + tx->stmin_us;
        elapsed_us = ts_diff_us(&t, &tx->t_last);
        if (elapsed_us < (int64_t)stmin_us)
        {
            return ISOTP_INCOMPLETE;   /* 时间未到，下次再发 */
        }

        if (isotp_send_cf(tx, send, arg) != 0)
        {
            /* 发送邮箱满：不推进状态，下个 tick 重试（已发送计数未变） */
            tx->t_last = t;
            return ISOTP_INCOMPLETE;
        }
        tx->t_last = t;

        if (tx->sent_len >= tx->total_len)
        {
            /* 全部数据已发出。J1939 需等待 EndOfMsgACK，ISO 15765-2 直接完成 */
            if (tx->fmt == ISOTP_FMT_J1939TP)
            {
                tx->state = ISOTP_TX_DONE;
                return ISOTP_COMPLETE;
            }
            tx->state = ISOTP_TX_DONE;
            return ISOTP_COMPLETE;
        }

        /* 块发送完毕：若对端限定了块大小，需要重新等待 CTS/FC */
        if (tx->bs != 0u && tx->bs != 0xFFu && tx->block_sent >= tx->bs)
        {
            tx->block_sent = 0;
            tx->ff_sent    = 0;
            tx->retry      = 0;
            tx->state      = ISOTP_TX_WAIT_FC;
            tx->t_last     = t;
            return ISOTP_INCOMPLETE;
        }

        return ISOTP_INCOMPLETE;
    }

    return ISOTP_ERR_STATE;
}

isotp_status_t isotp_tx_feed(isotp_tx_t *tx, const can_item_t *item)
{
    if (tx == NULL || item == NULL)
    {
        return ISOTP_ERR_PARAM;
    }
    if (tx->state == ISOTP_TX_IDLE || tx->state == ISOTP_TX_DONE ||
        tx->state == ISOTP_TX_ABORTED)
    {
        return ISOTP_OK;
    }
    if (item->can_id != tx->cm_id)
    {
        return ISOTP_OK;
    }

    if (tx->fmt == ISOTP_FMT_J1939TP)
    {
        switch (item->data[0])
        {
            case 0x11:   /* CTS —— 允许发送 */
                tx->bs         = item->data[1];                 /* 允许的包数 */
                tx->stmin_ms   = item->data[3];                 /* 包间隔 ms */
                tx->stmin_us   = 0;
                tx->block_sent = 0;
                tx->retry      = 0;
                tx->ff_sent    = 1;
                tx->state      = ISOTP_TX_SENDING;
                return ISOTP_INCOMPLETE;

            case 0x13:   /* EndOfMsgACK —— 对端确认收妥 */
                tx->state = ISOTP_TX_DONE;
                return ISOTP_COMPLETE;

            case 0xFF:   /* Abort */
                tx->state = ISOTP_TX_ABORTED;
                fprintf(stderr, "[ISOTP] 对端中止本次传输，原因码 0x%02X\n", item->data[1]);
                return ISOTP_ERR_ABORT;

            default:
                return ISOTP_OK;
        }
    }

    /* ISO 15765-2 流控帧 */
    if (((item->data[0] >> 4) & 0x0F) == 0x03)
    {
        uint8_t fs = (uint8_t)(item->data[0] & 0x0F);
        uint32_t stmin;

        if (fs == 0x0)          /* CTS */
        {
            stmin = isotp_decode_stmin(item->data[2]);
            tx->bs         = item->data[1];
            tx->stmin_ms   = (uint8_t)(stmin / 1000u);
            tx->stmin_us   = (uint16_t)(stmin % 1000u);
            tx->block_sent = 0;
            tx->retry      = 0;
            tx->ff_sent    = 1;
            tx->state      = ISOTP_TX_SENDING;
            return ISOTP_INCOMPLETE;
        }
        if (fs == 0x1)          /* WAIT：继续等待，由超时机制兜底 */
        {
            tx->t_last = item->ts;
            return ISOTP_INCOMPLETE;
        }
        /* fs == 0x2 OVFLW：接收方缓冲不足 */
        tx->state = ISOTP_TX_ABORTED;
        fprintf(stderr, "[ISOTP] 对端流控溢出(OVFLW)，传输中止\n");
        return ISOTP_ERR_ABORT;
    }

    return ISOTP_OK;
}

void isotp_tx_abort(isotp_tx_t *tx, uint8_t reason, isotp_send_fn send, void *arg)
{
    uint8_t d[8];

    if (tx == NULL)
    {
        return;
    }
    if (tx->state != ISOTP_TX_WAIT_FC && tx->state != ISOTP_TX_SENDING)
    {
        return;
    }

    if (send != NULL)
    {
        memset(d, 0xFF, sizeof(d));
        d[0] = (tx->fmt == ISOTP_FMT_J1939TP) ? 0xFF : 0x32;   /* J1939 Abort / ISO FC.OVFLW */
        d[1] = reason;
        (void)send(arg, tx->cm_id, d, 8);
    }

    tx->state = ISOTP_TX_ABORTED;
}

/******************* (C) COPYRIGHT 2025 CAN Monitor *****END OF FILE****/
