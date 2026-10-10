/**
 * @file    ring_buffer.h
 * @brief   无锁环形缓冲区 —— CAN 接收线程与协议解析线程之间的数据解耦通道
 *
 * 设计要点
 * --------
 * 1. 采用「每槽一个 struct can_item_t」的定长环形队列，不使用任何互斥锁；
 * 2. 写指针（head）用 C11 原子变量的 CAS 推进，因此天然支持
 *    **多生产者**（本工程为 1 个 CAN 接收线程，可扩展到多路 CAN）；
 * 3. 读指针（tail）只由 **单消费者**（协议解析线程）推进，无需原子 CAS，
 *    只需 release 语义保证数据可见性；
 * 4. 容量固定为 2 的幂，用位与代替取模，消除除法开销；
 * 5. 队列满时采用「丢弃新帧 + 计数」策略，保证已入队的报文时序不乱。
 *
 * 内存序说明
 * ----------
 *   生产者：先写 slot 数据，再用 memory_order_release 发布 head
 *   消费者：先用 memory_order_acquire 读取 head，再读 slot 数据
 *   这套 acquire/release 配对即可保证 slot 数据读写的可见性与顺序性，
 *   不需要代价高昂的 seq_cst 全栅栏。
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 环形缓冲区默认容量（CAN 帧数），必须是 2 的幂 */
#define RB_DEFAULT_CAPACITY   1024u

/**
 * @brief 环形缓冲区中的元素：一帧带时间戳的 CAN 报文
 *
 * 时间戳取自内核 SO_TIMESTAMPNS，精度为纳秒，比用户态 clock_gettime
 * 更接近报文真实到达时刻（不受调度延迟影响）。
 */
typedef struct
{
    uint32_t        can_id;      /**< 29 位扩展帧 ID（已剥离 EFF/RTR/ERR 标志位） */
    uint8_t         data[8];     /**< 数据域（CAN FD 不涉及，固定 8 字节） */
    uint8_t         dlc;         /**< 数据长度 0~8 */
    uint8_t         is_error;    /**< 1 = 该帧为 CAN 错误帧（CAN_ERR_FLAG） */
    uint8_t         is_extended; /**< 1 = 扩展帧，0 = 标准帧 */
    uint8_t         reserved;    /**< 对齐填充 */
    struct timespec ts;          /**< 内核接收时间戳（CLOCK_REALTIME） */
} can_item_t;

/**
 * @brief 无锁环形缓冲区控制块
 *
 * 注意：控制块本身必须常驻内存（本工程使用静态/堆分配，不随栈释放），
 *       且各字段之间以 cache line 对齐可进一步降低伪共享，
 *       Cortex-A7 单核场景下收益有限，此处仅做 64 字节对齐。
 */
typedef struct
{
    can_item_t      *slots;                  /**< 槽位数组 */
    size_t           capacity;               /**< 容量（2 的幂） */
    size_t           mask;                   /**< capacity - 1，用于位与取模 */
    _Atomic size_t   head;                   /**< 写指针，多生产者 CAS 推进 */
    _Atomic size_t   tail;                   /**< 读指针，单消费者推进 */
    _Atomic uint64_t push_ok;                /**< 累计成功入队帧数 */
    _Atomic uint64_t push_drop;              /**< 累计因队列满而丢弃帧数 */
    _Atomic uint64_t pop_ok;                 /**< 累计成功出队帧数 */
    _Atomic uint64_t high_watermark;         /**< 历史最大占用深度（用于容量评估） */
} ring_buffer_t;

/*---------------------------------------------------------------------------
 * 生命周期
 *-------------------------------------------------------------------------*/

/**
 * @brief  初始化环形缓冲区
 * @param  rb       控制块指针
 * @param  capacity 期望容量，内部会向上取整到 2 的幂；传 0 使用默认 1024
 * @return 0 成功；-1 参数非法；-2 内存分配失败
 */
int  rb_init(ring_buffer_t *rb, size_t capacity);

/**
 * @brief  销毁环形缓冲区并释放内存
 */
void rb_deinit(ring_buffer_t *rb);

/*---------------------------------------------------------------------------
 * 生产者接口
 *-------------------------------------------------------------------------*/

/**
 * @brief  写入一帧（多生产者安全）
 * @param  rb   控制块
 * @param  item 待写入的数据帧
 * @return 0 成功；-1 队列已满（帧被丢弃，drop 计数 +1）；-2 参数非法
 * @note   可从任意线程调用，甚至可在信号处理函数中调用（无锁、无系统调用）
 */
int  rb_push(ring_buffer_t *rb, const can_item_t *item);

/*---------------------------------------------------------------------------
 * 消费者接口
 *-------------------------------------------------------------------------*/

/**
 * @brief  取出一帧（仅允许单消费者线程调用）
 * @param  rb   控制块
 * @param  item 输出参数
 * @return 0 成功；-1 队列为空；-2 参数非法
 */
int  rb_pop(ring_buffer_t *rb, can_item_t *item);

/**
 * @brief  批量取出多帧，减少循环开销
 * @param  rb    控制块
 * @param  items 输出数组
 * @param  max   数组最大长度
 * @return 实际取出的帧数（0 表示队列为空）
 */
size_t rb_pop_batch(ring_buffer_t *rb, can_item_t *items, size_t max);

/*---------------------------------------------------------------------------
 * 查询接口
 *-------------------------------------------------------------------------*/

size_t   rb_size(const ring_buffer_t *rb);      /**< 当前已用槽位数 */
size_t   rb_capacity(const ring_buffer_t *rb);  /**< 容量 */
int      rb_is_empty(const ring_buffer_t *rb);  /**< 1 = 空 */
int      rb_is_full(const ring_buffer_t *rb);   /**< 1 = 满 */

uint64_t rb_push_count(const ring_buffer_t *rb);   /**< 累计写入帧数 */
uint64_t rb_drop_count(const ring_buffer_t *rb);   /**< 累计丢弃帧数 */
uint64_t rb_pop_count(const ring_buffer_t *rb);    /**< 累计读取帧数 */
uint64_t rb_high_watermark(const ring_buffer_t *rb);/**< 峰值占用深度 */

/**
 * @brief  清空队列并复位统计量（用于重新开始一轮联调测试）
 */
void     rb_reset(ring_buffer_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* RING_BUFFER_H */
