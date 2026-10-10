/**
 * @file    uitouch.h
 * @brief   触摸屏输入（Linux evdev / input 子系统）
 *
 * 说明
 * ----
 *   野火 4.3 寸屏用的是 GT1151 电容触摸，内核已经把它注册成标准
 *   evdev 设备（/dev/input/eventX），所以用户态不需要写任何 I2C 驱动，
 *   直接读 event 即可。本模块负责：
 *     1. 自动在 /dev/input/event0..N 里找出「触摸屏」那个设备
 *        （按 EVIOCGBIT 能力位 + EVIOCGNAME 名字双重判断）；
 *     2. 兼容两种触摸上报协议：
 *          · Type A / 单点：BTN_TOUCH + ABS_X/ABS_Y
 *          · Type B / 多点：ABS_MT_TRACKING_ID + ABS_MT_POSITION_X/Y
 *        GT1151 上两种都可能出现，这里统一归一化成
 *        「当前坐标 + 按下/抬起边沿事件」；
 *     3. 把原始 ABS 坐标按屏幕分辨率做线性映射，解决触摸区与显示区
 *        不完全重合（常见 ±2%）的问题。
 */

#ifndef UITOUCH_H
#define UITOUCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int  fd;                /**< evdev 文件描述符；demo 模式为 -1 */
    char name[64];          /**< 设备名（EVIOCGNAME） */
    char path[64];          /**< 设备节点路径 */

    int  x, y;              /**< 映射到屏幕坐标后的当前触点 */
    int  raw_x, raw_y;      /**< 设备原始坐标 */
    int  down;              /**< 1 = 手指仍按着 */

    int  pressed;           /**< 本次 poll 期间发生了「按下」（边沿，读后自动清） */
    int  released;          /**< 本次 poll 期间发生了「抬起」（边沿，读后自动清） */

    int  abs_min_x, abs_max_x;   /**< ABS_X / ABS_MT_POSITION_X 的原始量程 */
    int  abs_min_y, abs_max_y;
    int  screen_w, screen_h;     /**< 映射目标分辨率 */

    int  has_mt;            /**< 1 = 支持 ABS_MT_* 多点协议 */
    uint32_t ev_count;      /**< 累计有效事件数（诊断用） */

    int  demo;              /**< 1 = 演示模式（无触摸硬件时按脚本产生触摸事件） */
    int  demo_step;         /**< 演示脚本已执行到第几步 */
    int  demo_next_ms;      /**< 下一步的触发时刻（毫秒，取自 CLOCK_MONOTONIC） */
    int  grabbed;           /**< 1 = 已用 EVIOCGRAB 独占抓取 */

    /* --- 坐标修正开关（触摸方向/镜像不对时用，见 uitouch_set_transform） --- */
    int  swap_xy;           /**< 1 = 交换 X/Y */
    int  mirror_x;          /**< 1 = X 轴镜像 */
    int  mirror_y;          /**< 1 = Y 轴镜像 */

    /* --- 量程来源（诊断用：内核给的两套量程可能不一致） --- */
    int  ax_min_x, ax_max_x;    /**< ABS_X / ABS_Y 上报的量程 */
    int  ax_min_y, ax_max_y;
    int  mt_min_x, mt_max_x;    /**< ABS_MT_POSITION_X/Y 上报的量程 */
    int  mt_min_y, mt_max_y;
    int  range_overridden;      /**< 1 = 量程由 --touch-calib 指定 */
    int  range_hint_done;       /**< 「量程可能不对」的提示只打一次 */

    /* --- 调试 --- */
    int  dbg;               /**< 1 = 打印每个事件与映射结果 */
    int  dbg_events;        /**< 已经打过多少个原始事件（只打前若干个，避免刷屏） */
    int  press_count;       /**< 累计按下次数（判断"量程可能不对"用） */
    int  obs_max_x;         /**< 实测见过的最大原始 X */
    int  obs_max_y;         /**< 实测见过的最大原始 Y */
    uint64_t last_activity_ms;  /**< 最近一次收到坐标的时刻（用于自动抬手兜底） */
} uitouch_t;

/**
 * @brief  打开触摸设备
 * @param  t        上下文
 * @param  dev      设备路径；传 NULL 表示自动查找；
 *                  传字符串 "demo" 进入**演示模式**
 * @param  screen_w 屏幕宽（用于坐标映射）
 * @param  screen_h 屏幕高
 * @return 0 成功；-1 没找到设备；负值为 errno
 *
 * 演示模式（dev = "demo"）
 * ----------------------
 *   没有触摸硬件时（PC 上跑、或者板子还没接屏），按固定脚本产生触摸事件；
 *   命令行等价写法是 --touch demo。本模式不打开任何设备节点，
 *   因此不需要 root，也不依赖 /dev/uinput：
 *     第 2 秒  点「数据曲线」按钮（主界面 -> 数据曲线，按钮 x 314..401）
 *     第 5 秒  点「历史」按钮（数据曲线 -> 历史九宫格，按钮 x 332..401）
 *     第 8 秒  点 (200,270)：九宫格（到 y 265）与标题栏（止于 y 23）之外的
 *              空白带，不应有任何切页
 *   按钮都在标题栏里（y 4..23，中心 y = 14），矩形由 gui.c 的
 *   gui_button_rect() 算出；命中判定与真实触摸共用 gui.c 的 gui_hit_test()。
 *   用来把「触摸 -> 命中测试 -> 切页 -> 从 SQLite 取曲线 -> 重绘」整条链路
 *   在无硬件条件下跑通，配合 --gui-rec 导出每一帧做目视检查。
 *   正常使用不要传这个值。
 */
int  uitouch_open(uitouch_t *t, const char *dev, int screen_w, int screen_h);

/** 关闭设备 */
void uitouch_close(uitouch_t *t);

/**
 * @brief  独占抓取触摸设备（EVIOCGRAB）
 *
 * 什么情况下需要它
 * --------------
 *   野火 Debian 出厂是带着桌面环境（X11 / LXDE）启动的。只要 X 在跑，
 *   它就会打开 /dev/input/eventX 并消费触摸事件，我们的程序**一个触摸
 *   事件都收不到** —— 表现就是「界面显示正常，但怎么点都没反应」。
 *   同时它还在往 /dev/fb0 上画，屏幕会时不时闪回桌面。
 *
 *   EVIOCGRAB 是内核提供的「独占锁」：抓取成功后，同一个事件设备上的
 *   事件只发给本进程，X 那边收不到，触摸立刻可用。
 *
 * 副作用与对策
 * ------------
 *   抓取期间桌面收不到触摸（鼠标键盘不受影响，那是别的 event 设备）。
 *   所以退出时一定要解除抓取 —— uitouch_close() 会自动调用，
 *   否则要重启 X 才能恢复桌面触摸。
 *
 * @return 0 成功；负值为 errno
 */
int  uitouch_grab(uitouch_t *t);

/** 解除独占抓取（可重复调用） */
void uitouch_ungrab(uitouch_t *t);

/**
 * @brief  读取并处理待处理事件（非阻塞）
 * @param  t          上下文
 * @param  timeout_ms 等待超时；0 = 立即返回
 * @return 1 = 坐标或按键状态有更新；0 = 无变化；负值为错误
 * @note   t->pressed / t->released 是一次性边沿标志，读完即清。
 */
int  uitouch_poll(uitouch_t *t, int timeout_ms);

/** 清掉一次性的按下/抬起边沿标志 */
void uitouch_clear_edges(uitouch_t *t);

/**
 * @brief  校准触摸原始量程（有些设备不报 ABS 最大值）
 * @note   若内核没上报 ABS_X 的 min/max，就用屏幕分辨率当量程兜底。
 */
void uitouch_set_calib(uitouch_t *t, int min_x, int max_x, int min_y, int max_y);

/** 打印设备信息（启动日志用） */
void uitouch_dump_info(const uitouch_t *t);

/**
 * @brief  设置坐标修正（触摸方向对不上时用）
 *
 * 什么时候需要它
 * --------------
 *   触摸芯片上报的是「面板坐标系」，而 LCD 可能被旋转过。常见的三种偏差：
 *     · 横向拖动、光标竖着走   → swap = 1
 *     · 左右反了               → mirror_x = 1
 *     · 上下反了               → mirror_y = 1
 *   先用 --touch-debug 看实际映射到哪儿，再决定开哪个开关，
 *   不用重新编译。
 */
void uitouch_set_transform(uitouch_t *t, int swap_xy, int mirror_x, int mirror_y);

/**
 * @brief  手动指定原始量程（内核上报的量程不准时用）
 *
 * 什么时候需要它
 * --------------
 *   坐标映射靠的是「原始值 ÷ 内核上报的最大值」。可有些板子的设备树把
 *   量程配错了：野火 i.MX6ULL 那块 Goodix 屏，内核说 X 最大 34799，
 *   但它实际按**屏幕像素**上报（右下角只给到 469×271）。
 *   结果所有坐标都被除以 72，全挤在屏幕左上角一小块 —— 现象就是
 *   「怎么点都没反应」（见手册 7.5 节）。
 *
 *   打开 --touch-debug 后，程序在发现这种不一致时会直接打印出该用的参数；
 *   也可以自己点一下屏幕右下角，看日志里打印的原始值来填。
 *
 * @param  xmin,xmax,ymin,ymax 真实量程，例如屏幕像素就是 0,480,0,272
 *   野火 i.MX6ULL + GT1151 实测可用值（X 用 474 而不是 480，面板右边
 *   最后几列压不到）：
 *       ./can_monitor -i can0 --gui --touch-calib 0,474,0,272
 */
void uitouch_set_range(uitouch_t *t, int xmin, int xmax, int ymin, int ymax);

/**
 * @brief  打开/关闭触摸调试输出
 * @note   打开后只做两件事：① 打印前若干个原始事件（看清楚设备到底发了什么）；
 *         ② 每次坐标更新都打印「原始值 → 映射值」。
 *         屏幕上一律不画调试图形，正式界面保持干净。
 */
void uitouch_set_debug(uitouch_t *t, int on);

/** 最近一次映射后的坐标 */
void uitouch_get_mapped(const uitouch_t *t, int *x, int *y);

/**
 * @brief  读当前是否有手指按着
 * @return 1 = 按着
 */
int  uitouch_is_down(const uitouch_t *t);

#ifdef __cplusplus
}
#endif

#endif /* UITOUCH_H */
