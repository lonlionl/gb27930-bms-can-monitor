/**
 * @file    can_layer.h
 * @brief   CAN 硬件抽象层（HAL）—— 基于 Linux SocketCAN 的薄封装
 *
 * 本模块是整棵软件树的底座，向上一层（协议解析层）屏蔽以下细节：
 *   1. 网卡查找、socket 创建、bind 到 canX 接口；
 *   2. 通过 rtnetlink 在线配置 CAN 控制器（bitrate / 采样点 / 启动关闭 / 回环），
 *      不依赖 `ip` 命令，不 fork 外部进程，适合嵌入式精简 rootfs；
 *   3. 通过 setsockopt(CAN_RAW_FILTER) 下发 **CAN 控制器硬件滤波表**，
 *      只放行 GB/T 27930-2015 中 BMS→充电机方向的报文 ID，
 *      无效帧在控制器/MUX 层即被丢弃，显著降低 CPU 占用；
 *   4. 通过 setsockopt(CAN_RAW_ERR_FILTER) 打开错误帧上报，
 *      把位错误 / 填充错误 / 格式错误 / 应答错误 / CRC 错误分门别类计数；
 *   5. 提供阻塞带超时的接收接口与发送接口，收发计数与总线状态统一统计。
 *
 * 说明：本文件不使用任何第三方库，仅依赖 Linux 内核自带的 SocketCAN 子系统。
 */

#ifndef CAN_LAYER_H
#define CAN_LAYER_H

#include <stdint.h>
#include <stddef.h>

/* can_err_frame_str() 的入参就是 SocketCAN 错误帧的类别标志位
 * （CAN_ERR_BUSOFF / CAN_ERR_PROT / CAN_ERR_CRTL …），
 * 所以把定义这些宏的 UAPI 头文件一并导出给调用方，
 * 免得每个用它的 .c 都要自己再包含一次。 */
#include <linux/can/error.h>

/*----------------------------------------------------------------------------
 * 兼容性补丁
 *   SocketCAN 的 UAPI 头文件在不同内核版本之间有差异：
 *     · CAN_ERR_CNT 直到较新内核才加入 uapi/linux/can/error.h，
 *       老内核（I.MX6ULL 常见的 4.15 / 4.19）没有；
 *     · 部分 CAN_ERR_PROT_LOC_* 定位码、CAN_ERR_CRTL_* 状态位同样缺失。
 *   本工程要能在 4.9 ~ 6.x 全系列内核上编译，所以按内核源码里的原始取值
 *   在这里补全。放在头文件里（而不是 .c 里）是因为这些宏属于
 *   can_err_frame_str() 这个公开接口的一部分，调用方也会用到。
 *--------------------------------------------------------------------------*/
#ifndef CAN_ERR_CNT
#define CAN_ERR_CNT             0x00000200U  /* TX/RX 错误计数器有效 */
#endif
#ifndef CAN_ERR_PROT_LOC_UNSPEC
#define CAN_ERR_PROT_LOC_UNSPEC 0x00
#endif
#ifndef CAN_ERR_PROT_LOC_CRC_SEQ
#define CAN_ERR_PROT_LOC_CRC_SEQ 0x04
#endif
#ifndef CAN_ERR_PROT_LOC_ACK
#define CAN_ERR_PROT_LOC_ACK    0x09
#endif
#ifndef CAN_ERR_PROT_LOC_ACK_DEL
#define CAN_ERR_PROT_LOC_ACK_DEL 0x0B
#endif
#ifndef CAN_ERR_PROT_LOC_CRC_DEL
#define CAN_ERR_PROT_LOC_CRC_DEL 0x0F
#endif
#ifndef CAN_ERR_CRTL_RX_WARNING
#define CAN_ERR_CRTL_RX_WARNING 0x04
#endif
#ifndef CAN_ERR_CRTL_TX_WARNING
#define CAN_ERR_CRTL_TX_WARNING 0x08
#endif
#ifndef CAN_ERR_CRTL_RX_PASSIVE
#define CAN_ERR_CRTL_RX_PASSIVE 0x10
#endif
#ifndef CAN_ERR_CRTL_TX_PASSIVE
#define CAN_ERR_CRTL_TX_PASSIVE 0x20
#endif
#ifndef CAN_ERR_PROT_BIT
#define CAN_ERR_PROT_BIT        0x01
#endif
#ifndef CAN_ERR_PROT_FORM
#define CAN_ERR_PROT_FORM       0x02
#endif
#ifndef CAN_ERR_PROT_STUFF
#define CAN_ERR_PROT_STUFF      0x04
#endif

#include "ring_buffer.h"   /* 复用 can_item_t 作为帧载体 */

#ifdef __cplusplus
extern "C" {
#endif

/** CAN 接口名（I.MX6ULL 默认 can0） */
#define CAN_DEFAULT_IFNAME   "can0"

/** GB/T 27930-2015 总线波特率 */
#define CAN_BITRATE_GB27930  250000u

/**
 * CAN 总线状态
 *
 * 注意：内核头文件 <linux/can/netlink.h> 中已经定义了同名的
 * CAN_STATE_ERROR_ACTIVE / CAN_STATE_BUS_OFF 等枚举常量（用于
 * IFLA_CAN_STATE 属性）。若这里使用相同名字会与之冲突，
 * 因此统一下发 CANBUS_STATE_* 前缀，由 can_state_str() 负责转字符串。
 */
typedef enum
{
    CANBUS_STATE_ERROR_ACTIVE = 0,   /**< 错误主动（正常） */
    CANBUS_STATE_ERROR_WARNING,      /**< 错误警告（TEC/REC > 96） */
    CANBUS_STATE_ERROR_PASSIVE,      /**< 错误被动（TEC/REC > 127） */
    CANBUS_STATE_BUS_OFF,            /**< 总线关闭 */
    CANBUS_STATE_STOPPED,            /**< 控制器停止 */
    CANBUS_STATE_SLEEPING,           /**< 睡眠 */
    CANBUS_STATE_UNKNOWN
} can_bus_state_t;

/**
 * @brief 总线统计信息（用于终端仪表盘与故障诊断）
 */
typedef struct
{
    uint64_t rx_frames;      /**< 收到的有效数据帧总数 */
    uint64_t tx_frames;      /**< 成功发送的数据帧总数 */
    uint64_t tx_errors;      /**< 发送失败次数（EAGAIN/ENOBUFS 等） */
    uint64_t err_frames;     /**< 收到的错误帧总数 */
    uint64_t err_bit;        /**< 位错误（Bit Error） */
    uint64_t err_stuff;      /**< 填充错误（Stuff Error） */
    uint64_t err_form;       /**< 格式错误（Form Error） */
    uint64_t err_ack;        /**< 应答错误（ACK Error） */
    uint64_t err_crc;        /**< CRC 错误 */
    uint64_t err_other;      /**< 其他错误 */
    uint64_t bus_off;        /**< 进入 Bus-Off 次数 */
    uint64_t err_warning;    /**< 进入错误警告次数 */
    uint64_t err_passive;    /**< 进入错误被动次数 */
    uint64_t ctrl_restarts;  /**< 控制器自动重启次数（来自 sysfs） */
    uint64_t bus_errors_sys; /**< sysfs bus_error 计数 */
    uint8_t  txerr;          /**< 发送错误计数器 TEC */
    uint8_t  rxerr;          /**< 接收错误计数器 REC */
    can_bus_state_t state;   /**< 当前总线状态 */
} can_stats_t;

/**
 * @brief CAN 抽象层实例
 */
typedef struct
{
    int      fd;             /**< SocketCAN 原始套接字 */
    int      ifindex;        /**< 网卡索引 */
    char     ifname[16];     /**< 接口名，如 "can0" */
    uint32_t bitrate;        /**< 当前配置的波特率 */
    can_stats_t stats;       /**< 统计信息 */
    int      opened;         /**< 是否已打开 */
} can_layer_t;

/*==============================================================================
 * 接口配置（rtnetlink，无需 ip 命令）
 *============================================================================*/

/**
 * @brief  通过 rtnetlink 设置 CAN 控制器波特率与采样点
 * @param  ifname  接口名（"can0"）
 * @param  bitrate 波特率，如 250000
 * @return 0 成功；负值为 errno
 * @note   若内核不支持在线配置（老内核 / 未加载 can_dev），返回 -EOPNOTSUPP，
 *         此时请改用 `ip link set can0 type can bitrate 250000` 命令。
 */
int can_set_bitrate(const char *ifname, uint32_t bitrate);

/**
 * @brief  通过 rtnetlink 启动 / 关闭 CAN 接口
 * @param  ifname 接口名
 * @param  up     1 = up，0 = down
 * @return 0 成功；负值为 errno
 */
int can_set_link_up(const char *ifname, int up);

/**
 * @brief  打开 / 关闭 CAN 控制器回环模式
 * @param  ifname 接口名
 * @param  on     1 = 打开回环（自发自收），0 = 关闭
 * @return 0 成功；负值为 errno
 * @note   单板无对端时可用回环模式 + `cangen` 验证软件链路；
 *         与 STM32 真实联调时应关闭回环。
 */
int can_set_loopback(const char *ifname, int on);

/**
 * @brief  查询接口是否存在及其当前状态（flags / mtu / bitrate）
 * @return 0 存在；负值为 errno
 */
int can_query_link(const char *ifname, char *info, size_t info_len);

/*==============================================================================
 * 生命周期
 *============================================================================*/

/**
 * @brief  初始化 CAN 抽象层
 * @param  cl     实例指针
 * @param  ifname 接口名，传 NULL 使用 CAN_DEFAULT_IFNAME
 * @return 0 成功；负值为 errno；-1001 表示接口不存在
 * @note   本函数内部完成：socket(AF_CAN, SOCK_RAW, CAN_RAW) -> ioctl(SIOCGIFINDEX)
 *         -> setsockopt(SO_TIMESTAMPNS) -> bind()。
 *         硬件滤波与错误帧订阅需另外调用下面两个函数。
 */
int  can_layer_init(can_layer_t *cl, const char *ifname);

/**
 * @brief  关闭 socket 并复位实例
 */
void can_layer_deinit(can_layer_t *cl);

/*==============================================================================
 * 滤波与错误帧
 *============================================================================*/

/**
 * @brief  下发自定义 29 位扩展 ID 精确匹配滤波表
 * @param  ids  扩展 ID 数组
 * @param  num  数量（SocketCAN 最多 512 条）
 * @return 0 成功；负值为 errno
 * @note   GB/T 27930 的 BMS 侧报文 ID 列表由协议层 gb27930.c 提供
 *         （gb27930_apply_rx_filter），保持 HAL 层与协议层解耦。
 */
int can_apply_filter_ids(can_layer_t *cl, const uint32_t *ids, size_t num);

/**
 * @brief  关闭滤波，接收全部报文（联调排查用）
 */
int can_apply_filter_all(can_layer_t *cl);

/**
 * @brief  订阅 CAN 错误帧（位错误 / 填充错误 / 格式错误 / 应答错误 / CRC 错误）
 * @return 0 成功；负值为 errno
 */
int can_enable_error_frames(can_layer_t *cl);

/*==============================================================================
 * 收发
 *============================================================================*/

/**
 * @brief  发送一帧扩展数据帧
 * @param  ext_id 29 位扩展 ID
 * @param  data   数据指针
 * @param  dlc    数据长度 0~8
 * @return 0 成功；-1 帧格式非法；-2 EAGAIN（发送队列满）；其他负值为 errno
 */
int can_layer_send(can_layer_t *cl, uint32_t ext_id, const uint8_t *data, uint8_t dlc);

/**
 * @brief  接收一帧（带超时）
 * @param  cl         实例
 * @param  item       输出帧（含内核时间戳）
 * @param  timeout_ms 超时毫秒数，0 表示立即返回（非阻塞），<0 表示永久阻塞
 * @return  1 = 收到数据帧；0 = 超时；-1 = 收到错误帧（已统计，item->is_error = 1）；
 *          其他负值为 errno
 * @note   is_error = 1 时 item->data[0..7] 为 SocketCAN 原始错误帧内容，
 *         便于上层记录总线故障现场。
 */
int can_layer_recv(can_layer_t *cl, can_item_t *item, int timeout_ms);

/**
 * @brief  读取一次内核 sysfs 总线统计并合并到 cl->stats
 */
void can_layer_refresh_sysfs_stats(can_layer_t *cl);

/**
 * 收发帧统计开关
 * -----------------
 * 待机时 BMS 每秒广播一次电池状态（周期遥测），这些帧不是充电协议交互，
 * 却会把界面上的"收/发帧数"顶到几千，让人看不出这一次充电到底跑了多少报文。
 * 协议线程会在会话结束后置 0、进入会话前置 1：
 *   · 0 = 只处理帧，不计入统计
 *   · 1 = 正常统计（默认）
 * 注意：帧本身照收照处理，只是数字不涨。
 */
extern volatile int g_can_stat_enable;

/** 把总线状态枚举转成可读字符串 */
const char *can_state_str(can_bus_state_t st);

/**
 * @brief  把 SocketCAN 错误帧的内容翻译成中文可读原因
 *
 * 为什么需要它
 * ----------
 *   内核送上来的是 can_id 里的一组标志位 + 8 字节错误数据，直接打印成
 *   「标志=0x00000100 数据=00 00 …」是没法看的 —— 现场排查时根本不知道
 *   是总线关闭、错误被动，还是 ACK 错误。
 *   本函数把它翻成一串中文，例如：
 *       "控制器已重启; 总线关闭; 发送错误被动(TEC>127)"
 *
 * @param  can_id 错误帧的 can_id（含 CAN_ERR_FLAG）
 * @param  data   错误帧的 8 字节数据
 * @param  out    输出缓冲
 * @param  cap    缓冲大小
 * @return 写入的字符数（不含结尾 0）
 */
int can_err_frame_str(uint32_t can_id, const uint8_t *data, char *out, size_t cap);

/**
 * @brief  判断一条错误帧是不是「总线关闭 / 控制器重启」这类严重事件
 * @return 1 = 是（用于触发额外告警）
 */
int can_err_is_bus_off(uint32_t can_id);

#ifdef __cplusplus
}
#endif

#endif /* CAN_LAYER_H */
