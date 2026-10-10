/**
 * @file    isotp.h
 * @brief   用户态轻量多帧传输层 —— ISO 15765-2 与 SAE J1939-21 (TP) 双格式支持
 *
 * ============================ 为什么要两种格式？ ============================
 * 需求文档要求「基于 ISO 15765-2 实现多帧重组」，而 GB/T 27930-2015 中
 * BRM（BMS 辨识报文，41 字节）等长报文**实际使用的是 J1939 传输协议**
 * （TP.CM 的 RTS/CTS 握手 + TP.DT 的 1~15 序号分包），二者帧格式并不相同：
 *
 *   ISO 15765-2 (PCI 首字节高 4 位)      J1939-21 / GB/T 27930 (控制字节)
 *   --------------------------------     ----------------------------------
 *   0x0N  单帧 SF                        0x10  RTS  请求发送 (TP.CM)
 *   0x1N  首帧 FF                        0x11  CTS  允许发送 (TP.CM)
 *   0x2N  连续帧 CF                      0x13  EndOfMsgACK  传输完成应答
 *   0x3N  流控帧 FC                      0x14  BAM  广播通告
 *                                        0xFF  Abort  中止
 *
 * 本模块把两者统一到同一套 API 之下，由 isotp_format_t 选择，
 * 默认使用 J1939TP（与国标一致），也可切换为 ISO15765 用于自研设备扩展。
 *
 * ============================ 设计要点 ====================================
 * 1. 纯用户态实现，不修改内核，不依赖 can-isotp 内核模块，兼容所有
 *    带 SocketCAN 的 ARM Linux 内核（I.MX6ULL 的 4.19 内核开箱即用）。
 * 2. 接收侧采用「超时丢弃」策略：一个会话超过 timeout_ms 未收到后续帧
 *    即整包丢弃并计数，避免半包污染后续会话。
 * 3. 发送侧实现完整的流控状态机：FF/RTS -> 等待 FC/CTS -> 按 BS 分块
 *    发送 CF/DT -> 直到发完 -> 处理 EndOfMsgACK。
 * 4. 所有统计量（会话数、错序丢包、超时丢包）对上层开放，用于终端诊断面板。
 */

#ifndef ISOTP_H
#define ISOTP_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "ring_buffer.h"   /* can_item_t */

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 *                              常量与枚举
 *============================================================================*/

/** 单条多帧报文的最大长度（GB/T 27930 中最长的 BRM 为 41 字节，留足余量） */
#define ISOTP_MAX_PAYLOAD      256u

/** 组包 / 流控默认超时（GB/T 27930-2015 规定 TP 相关超时为 1000 ms） */
#define ISOTP_DEFAULT_TIMEOUT  1000u

/** 发送最大重试次数 */
#define ISOTP_MAX_RETRY        3

/** 传输格式 */
typedef enum
{
    ISOTP_FMT_J1939TP = 0,   /**< SAE J1939-21 传输协议（GB/T 27930-2015 实际使用） */
    ISOTP_FMT_ISO15765 = 1   /**< ISO 15765-2 网络层（PCI 编码） */
} isotp_format_t;

/** 统一返回码 */
typedef enum
{
    ISOTP_OK          =  0,   /**< 操作成功（发送侧：一帧已发出） */
    ISOTP_INCOMPLETE  =  1,   /**< 已处理，仍需更多帧 */
    ISOTP_COMPLETE    =  2,   /**< 整包组包完成 */
    ISOTP_ERR_PARAM   = -1,   /**< 参数非法 */
    ISOTP_ERR_SEQ     = -2,   /**< 连续帧序号错乱 */
    ISOTP_ERR_OVERFLOW= -3,   /**< 长度超出缓冲区 */
    ISOTP_ERR_TIMEOUT = -4,   /**< 组包 / 流控超时 */
    ISOTP_ERR_ABORT   = -5,   /**< 对端发送 Abort */
    ISOTP_ERR_NOBUF   = -6,   /**< 无空闲会话 */
    ISOTP_ERR_STATE   = -7    /**< 状态机不允许该操作 */
} isotp_status_t;

/** 发送侧状态机 */
typedef enum
{
    ISOTP_TX_IDLE = 0,        /**< 空闲 */
    ISOTP_TX_WAIT_FC,         /**< 已发 FF/RTS，等待 FC/CTS */
    ISOTP_TX_SENDING,         /**< 正在发送 CF/DT */
    ISOTP_TX_DONE,            /**< 发送完成 */
    ISOTP_TX_ABORTED          /**< 已中止 */
} isotp_tx_state_t;

/** 发送回调：由调用者注入实际的 CAN 发送动作，实现与 HAL 层解耦 */
typedef int (*isotp_send_fn)(void *arg, uint32_t can_id, const uint8_t *data, uint8_t len);

/*==============================================================================
 *                              接收上下文
 *============================================================================*/

typedef struct
{
    isotp_format_t  fmt;                 /**< 传输格式 */
    uint32_t        cm_id;               /**< 首帧 / TP.CM 所在 CAN ID */
    uint32_t        dt_id;               /**< 连续帧 / TP.DT 所在 CAN ID */
    uint32_t        timeout_ms;          /**< 组包超时（毫秒） */

    int             active;              /**< 是否存在进行中的会话 */
    uint16_t        total_len;           /**< 报文总长度（FF/RTS 中携带） */
    uint16_t        recv_len;            /**< 已接收长度 */
    uint8_t         next_seq;            /**< 期望的下一个连续帧序号 */
    uint8_t         bs;                  /**< J1939 包大小 */
    uint8_t         data[ISOTP_MAX_PAYLOAD]; /**< 组包缓冲区 */
    uint32_t        src_addr;            /**< 源地址（用于应答 CTS/ACK） */

    struct timespec t_start;             /**< 会话开始时间 */
    struct timespec t_last;              /**< 最近一帧时间 */

    /* 统计 */
    uint32_t        stat_sessions;       /**< 累计完成会话数 */
    uint32_t        stat_frames;         /**< 累计处理的帧数 */
    uint32_t        stat_timeouts;       /**< 超时丢弃次数 */
    uint32_t        stat_seq_err;        /**< 序号错误次数 */
    uint32_t        stat_overflow;       /**< 溢出丢弃次数 */
} isotp_rx_t;

/*==============================================================================
 *                              发送上下文
 *============================================================================*/

typedef struct
{
    isotp_format_t   fmt;
    uint32_t         cm_id;              /**< FF / RTS 使用的 CAN ID */
    uint32_t         dt_id;              /**< CF / DT  使用的 CAN ID */

    isotp_tx_state_t state;
    const uint8_t   *payload;            /**< 待发送数据（调用者保证生命周期） */
    uint16_t         total_len;
    uint16_t         sent_len;
    uint8_t          next_seq;           /**< 下一个连续帧序号（1 基） */
    uint8_t          bs;                 /**< 对端允许的块大小，0xFF 表示不限 */
    uint8_t          stmin_ms;           /**< 最小帧间隔（毫秒部分） */
    uint16_t         stmin_us;           /**< 最小帧间隔（微秒部分，0xF1~0xF9 编码为 100~900us） */
    uint8_t          block_sent;         /**< 当前块内已发连续帧数 */
    uint8_t          ff_sent;            /**< 1 = 首帧/RTS 已发出，正在等待 FC/CTS */
    uint8_t          retry;              /**< 已重试次数 */
    uint32_t         timeout_ms;
    struct timespec  t_last;             /**< 最近一次发送时间 */

    uint32_t         stat_frames;        /**< 累计发出的帧数 */
    uint32_t         stat_retry;         /**< 累计重试次数 */
} isotp_tx_t;

/*==============================================================================
 *                              接收侧 API
 *============================================================================*/

/**
 * @brief  初始化接收上下文
 * @param  rx        上下文
 * @param  fmt       传输格式（J1939TP / ISO15765）
 * @param  cm_id     首帧 / TP.CM 的 CAN ID（BRM 场景为 0x1CEC56F4）
 * @param  dt_id     连续帧 / TP.DT 的 CAN ID（BRM 场景为 0x1CEB56F4）
 * @param  timeout_ms 组包超时，传 0 使用 ISOTP_DEFAULT_TIMEOUT
 */
void isotp_rx_init(isotp_rx_t *rx, isotp_format_t fmt,
                   uint32_t cm_id, uint32_t dt_id, uint32_t timeout_ms);

/** 复位当前会话（保留统计量） */
void isotp_rx_reset(isotp_rx_t *rx);

/**
 * @brief  向重组器投喂一帧 CAN 报文
 * @param  rx   上下文
 * @param  item CAN 帧（带内和时间戳）
 * @return ISOTP_COMPLETE  整包已组好，可从 rx->data / rx->total_len 读取；
 *         ISOTP_INCOMPLETE 会话进行中；
 *         ISOTP_OK         该帧与本会话无关（ID 不匹配）；
 *         其余为负值错误码，会话已被复位。
 * @note   只有 can_id 等于 cm_id 或 dt_id 的帧才会被处理，其它帧返回 ISOTP_OK。
 */
isotp_status_t isotp_rx_feed(isotp_rx_t *rx, const can_item_t *item);

/**
 * @brief  周期性超时检查（建议在解析线程空闲时每 50~100 ms 调用一次）
 * @param  now 当前时间；传 NULL 则内部调用 clock_gettime
 * @return ISOTP_ERR_TIMEOUT 刚刚因超时丢弃了一个半包；ISOTP_OK 无变化
 */
isotp_status_t isotp_rx_tick(isotp_rx_t *rx, const struct timespec *now);

/** 把错误码转成可读字符串 */
const char *isotp_status_str(isotp_status_t st);

/*==============================================================================
 *                              发送侧 API
 *============================================================================*/

/**
 * @brief  初始化发送上下文
 */
void isotp_tx_init(isotp_tx_t *tx, isotp_format_t fmt,
                   uint32_t cm_id, uint32_t dt_id, uint32_t timeout_ms);

/**
 * @brief  发起一次多帧发送
 * @param  payload 数据指针（调用期间必须保持有效）
 * @param  len     数据长度，<= 7 字节时可走单帧直发（由调用者自行决定）
 * @return ISOTP_OK 已排队；负值为错误码
 * @note   调用后必须循环调用 isotp_tx_poll() 推进状态机，直到返回
 *         ISOTP_COMPLETE / ISOTP_ERR_*。
 */
isotp_status_t isotp_tx_start(isotp_tx_t *tx, const uint8_t *payload, uint16_t len);

/**
 * @brief  推进发送状态机（发出 FF/RTS 或下一批 CF/DT）
 * @param  now 当前时间（NULL 表示内部取时间）
 * @return ISOTP_COMPLETE 全部发完；
 *         ISOTP_INCOMPLETE 仍在进行；
 *         ISOTP_ERR_TIMEOUT 等待流控帧超时且重试耗尽；
 *         其它负值为错误。
 */
isotp_status_t isotp_tx_poll(isotp_tx_t *tx, const struct timespec *now,
                             isotp_send_fn send, void *arg);

/**
 * @brief  处理对端回传的流控帧 / CTS / EndOfMsgACK
 * @return ISOTP_COMPLETE 传输确认完成；ISOTP_INCOMPLETE 可继续发送；
 *         ISOTP_ERR_ABORT 对端中止；ISOTP_OK 与本会话无关。
 */
isotp_status_t isotp_tx_feed(isotp_tx_t *tx, const can_item_t *item);

/** 主动中止并（可选）向对端发送 Abort */
void isotp_tx_abort(isotp_tx_t *tx, uint8_t reason, isotp_send_fn send, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* ISOTP_H */
