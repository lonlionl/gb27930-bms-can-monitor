/**
 * @file    ui.h
 * @brief   终端监控界面（ANSI 彩色仪表盘，无第三方 UI 库依赖）
 *
 * 设计说明
 * --------
 * 1. 纯 ANSI 转义序列实现，不依赖 ncurses，交叉编译零成本；
 *    在 I.MX6ULL 的串口终端 / SSH 终端上均可正常显示。
 * 2. 采用「光标归位 + 整屏重绘」的方式刷新（\033[H），避免滚屏闪烁；
 *    配合隐藏/显示光标，观感接近专业工控上位机。
 * 3. 颜色语义与工业告警色一致：
 *      绿色 = 正常，黄色 = 警告，红色 = 故障/越限，青色 = 标题，灰色 = 次要信息。
 * 4. 界面分区：
 *      ┌ 顶部标题栏：设备信息 / 运行时长 / 当前时间
 *      ├ 总线状态区：接口、波特率、Bus 状态、TEC/REC、错误分类计数
 *      ├ 状态机区  ：当前状态、已驻留时间、异常码、会话编号
 *      ├ 实时数据区：电压/电流/SOC/温度/单体电压（带进度条与阈值着色）
 *      ├ 报文统计区：各类型报文收发计数、周期抖动
 *      ├ 存储区    ：SQLite 写入量、队列水位、数据库大小
 *      └ 底部状态栏：快捷键提示 / 最近一条事件
 */

#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <stddef.h>

#include "can_layer.h"
#include "gb27930.h"
#include "ring_buffer.h"
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 界面运行期数据快照（由主线程每秒填充一次）
 */
typedef struct
{
    const char   *ifname;             /**< CAN 接口名 */
    uint32_t      bitrate;            /**< 波特率 */
    const char   *db_path;            /**< 数据库路径 */

    can_stats_t   can;                /**< CAN 层统计 */
    uint64_t      rx_rate;            /**< 最近 1 秒接收速率 (帧/s) */
    uint64_t      tx_rate;            /**< 最近 1 秒发送速率 (帧/s) */
    uint64_t      total_rx;           /**< 累计接收帧数 */
    uint64_t      total_tx;           /**< 累计发送帧数 */

    size_t        rb_size;            /**< 环形缓冲当前水位 */
    size_t        rb_capacity;        /**< 环形缓冲容量 */
    uint64_t      rb_pushed;          /**< 累计入队帧数 */
    uint64_t      rb_dropped;         /**< 累计丢弃帧数 */

    const gb_context_t *ctx;          /**< 协议上下文（含状态机与实时数据） */

    uint64_t      db_raw_written;     /**< 已写入原始帧数 */
    uint64_t      db_charge_written;  /**< 已写入解析记录数 */
    uint64_t      db_raw_dropped;     /**< 原始帧丢弃数 */
    uint64_t      db_commits;         /**< COMMIT 次数 */
    uint64_t      db_size;            /**< 数据库文件字节数 */

    double        uptime_sec;         /**< 程序已运行秒数 */
    const char   *last_event;         /**< 最近一条事件文本（可为 NULL） */
} ui_snapshot_t;

/** 仪表盘最少需要的终端行数；低于该值自动降级为滚动日志。
 *  紧凑布局实测约 24 行，留一点余量取 28。 */
#define UI_MIN_ROWS     28
/** 低于该行数则省略分隔线并压缩事件区，改用紧凑布局 */
#define UI_FULL_ROWS    36

/**
 * @brief  初始化界面
 * @return 1 = 已进入仪表盘模式；0 = 终端太矮，已自动降级为滚动日志模式
 * @note   降级时只打印一条提示，绝不输出任何光标控制序列，
 *         避免在串口 / Mobaxterm 上出现画面滚动错乱。
 */
int  ui_init(void);

/** 当前是否处于仪表盘模式（1 = 仪表盘，0 = 滚动日志） */
int  ui_is_dashboard(void);

/** 恢复终端状态：显示光标、复位颜色 */
void ui_shutdown(void);

/** 打印程序启动横幅（含接口、波特率、数据库路径等） */
void ui_banner(const char *version, const char *ifname, uint32_t bitrate,
               const char *db_path, int filter_on, int loopback);

/**
 * @brief  渲染整屏仪表盘
 */
void ui_render(const ui_snapshot_t *snap);

/**
 * @brief  在仪表盘下方打印一条事件日志（不破坏仪表盘布局）
 */
void ui_log(const char *level, const char *fmt, ...);

/** 打印帮助信息 */
void ui_usage(const char *prog);

/*---------------------------------------------------------------------------
 * 供协议层回调使用的轻量日志宏（统一前缀与配色）
 *-------------------------------------------------------------------------*/
#define UI_C_RESET   "\033[0m"
#define UI_C_RED     "\033[31m"
#define UI_C_GREEN   "\033[32m"
#define UI_C_YELLOW  "\033[33m"
#define UI_C_BLUE    "\033[34m"
#define UI_C_MAGENTA "\033[35m"
#define UI_C_CYAN    "\033[36m"
#define UI_C_GRAY    "\033[90m"
#define UI_C_BOLD    "\033[1m"

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
