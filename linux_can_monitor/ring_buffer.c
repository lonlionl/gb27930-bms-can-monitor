/**
 * @file    ring_buffer.c
 * @brief   无锁环形缓冲区实现（SPMC / MPSC 安全的定长队列）
 *
 * 实现细节
 * --------
 * 槽位所有权转移模型：
 *   - head 指向「下一个可写槽位」，tail 指向「下一个可读槽位」；
 *   - 队列长度 = head - tail（无符号回绕减法天然正确）；
 *   - 生产者必须先 CAS 抢占一个 head 序号，再写该序号对应的槽位；
 *     由于 CAS 抢到了独占序号，多个生产者之间不会互相覆盖数据；
 *   - 消费者读到 head 后即可安全读取 [tail, head) 区间，
 *     因为生产者在发布 head 之前已完成数据写入（release 语义）。
 */

#include "ring_buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*==============================================================================
 *                              内部工具
 *============================================================================*/

/** 向上取整到 2 的幂 */
static size_t round_up_pow2(size_t v)
{
    size_t p = 1;
    if (v < 2)
    {
        return 2;
    }
    while (p < v)
    {
        p <<= 1;
    }
    return p;
}

/*==============================================================================
 *                              生命周期
 *============================================================================*/

int rb_init(ring_buffer_t *rb, size_t capacity)
{
    if (rb == NULL)
    {
        return -1;
    }

    memset(rb, 0, sizeof(*rb));

    if (capacity == 0)
    {
        capacity = RB_DEFAULT_CAPACITY;
    }

    rb->capacity = round_up_pow2(capacity);
    rb->mask     = rb->capacity - 1u;

    /* 使用 posix_memalign 保证槽位数组按 cache line 对齐（64 字节）。
     * 不用 C11 的 aligned_alloc，是为了兼容 I.MX6ULL 上较老的 glibc。 */
    {
        void *mem = NULL;
        if (posix_memalign(&mem, 64, rb->capacity * sizeof(can_item_t)) != 0)
        {
            return -2;
        }
        rb->slots = (can_item_t *)mem;
    }
    memset(rb->slots, 0, rb->capacity * sizeof(can_item_t));

    atomic_store_explicit(&rb->head, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->push_ok, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->push_drop, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->pop_ok, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->high_watermark, 0, memory_order_relaxed);

    return 0;
}

void rb_deinit(ring_buffer_t *rb)
{
    if (rb == NULL)
    {
        return;
    }
    free(rb->slots);
    rb->slots = NULL;
    rb->capacity = 0;
    rb->mask = 0;
}

void rb_reset(ring_buffer_t *rb)
{
    if (rb == NULL)
    {
        return;
    }
    atomic_store_explicit(&rb->head, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->push_ok, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->push_drop, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->pop_ok, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->high_watermark, 0, memory_order_relaxed);
}

/*==============================================================================
 *                              生产者
 *============================================================================*/

int rb_push(ring_buffer_t *rb, const can_item_t *item)
{
    size_t head, tail, next;

    if (rb == NULL || item == NULL || rb->slots == NULL)
    {
        return -2;
    }

    for (;;)
    {
        head = atomic_load_explicit(&rb->head, memory_order_relaxed);
        tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

        if ((head - tail) >= rb->capacity)
        {
            /* 队列满：丢弃新帧（保旧帧，保证已入库报文的时序连续性） */
            atomic_fetch_add_explicit(&rb->push_drop, 1, memory_order_relaxed);
            return -1;
        }

        next = head + 1;

        /* 抢占 head 序号：多生产者场景下保证每个生产者拿到不同槽位 */
        if (atomic_compare_exchange_weak_explicit(&rb->head, &head, next,
                                                  memory_order_release,
                                                  memory_order_relaxed))
        {
            break;
        }
        /* CAS 失败说明有其他生产者抢先，head 已被刷新，重试即可 */
    }

    /* 序号已独占，写入槽位数据（release 语义已在上面发布，此处为普通写） */
    rb->slots[head & rb->mask] = *item;

    atomic_fetch_add_explicit(&rb->push_ok, 1, memory_order_relaxed);

    /* 更新峰值占用（弱一致性比较，仅作容量评估用，允许少量误差） */
    {
        size_t used = head + 1 - tail;
        uint64_t hw = atomic_load_explicit(&rb->high_watermark, memory_order_relaxed);
        if ((uint64_t)used > hw)
        {
            atomic_store_explicit(&rb->high_watermark, (uint64_t)used, memory_order_relaxed);
        }
    }

    return 0;
}

/*==============================================================================
 *                              消费者
 *============================================================================*/

int rb_pop(ring_buffer_t *rb, can_item_t *item)
{
    size_t tail, head;

    if (rb == NULL || item == NULL || rb->slots == NULL)
    {
        return -2;
    }

    tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    head = atomic_load_explicit(&rb->head, memory_order_acquire);

    if (tail == head)
    {
        return -1;   /* 空队列 */
    }

    *item = rb->slots[tail & rb->mask];

    /* 单消费者，直接推进 tail 即可；release 保证槽位释放对生产者可见 */
    atomic_store_explicit(&rb->tail, tail + 1, memory_order_release);
    atomic_fetch_add_explicit(&rb->pop_ok, 1, memory_order_relaxed);

    return 0;
}

size_t rb_pop_batch(ring_buffer_t *rb, can_item_t *items, size_t max)
{
    size_t count = 0;
    size_t tail, head, avail, i;

    if (rb == NULL || items == NULL || max == 0 || rb->slots == NULL)
    {
        return 0;
    }

    tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    head = atomic_load_explicit(&rb->head, memory_order_acquire);

    avail = head - tail;
    if (avail == 0)
    {
        return 0;
    }
    if (avail > max)
    {
        avail = max;
    }

    for (i = 0; i < avail; i++)
    {
        items[count++] = rb->slots[(tail + i) & rb->mask];
    }

    atomic_store_explicit(&rb->tail, tail + avail, memory_order_release);
    atomic_fetch_add_explicit(&rb->pop_ok, avail, memory_order_relaxed);

    return count;
}

/*==============================================================================
 *                              查询
 *============================================================================*/

size_t rb_size(const ring_buffer_t *rb)
{
    size_t head, tail;
    if (rb == NULL)
    {
        return 0;
    }
    head = atomic_load_explicit(&rb->head, memory_order_acquire);
    tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    return head - tail;
}

size_t rb_capacity(const ring_buffer_t *rb)
{
    return (rb != NULL) ? rb->capacity : 0;
}

int rb_is_empty(const ring_buffer_t *rb)
{
    return (rb_size(rb) == 0) ? 1 : 0;
}

int rb_is_full(const ring_buffer_t *rb)
{
    if (rb == NULL)
    {
        return 0;
    }
    return (rb_size(rb) >= rb->capacity) ? 1 : 0;
}

uint64_t rb_push_count(const ring_buffer_t *rb)
{
    return (rb != NULL) ? atomic_load_explicit(&rb->push_ok, memory_order_relaxed) : 0;
}

uint64_t rb_drop_count(const ring_buffer_t *rb)
{
    return (rb != NULL) ? atomic_load_explicit(&rb->push_drop, memory_order_relaxed) : 0;
}

uint64_t rb_pop_count(const ring_buffer_t *rb)
{
    return (rb != NULL) ? atomic_load_explicit(&rb->pop_ok, memory_order_relaxed) : 0;
}

uint64_t rb_high_watermark(const ring_buffer_t *rb)
{
    return (rb != NULL) ? atomic_load_explicit(&rb->high_watermark, memory_order_relaxed) : 0;
}

/******************* (C) COPYRIGHT 2025 CAN Monitor *****END OF FILE****/
