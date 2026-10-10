/**
 * @file    gui_app.h
 * @brief   GUI 应用层 —— 把协议/存储数据喂给界面，并处理触摸交互
 *
 * 线程模型
 * --------
 *   main 线程            : 命令行、终端仪表盘、信号处理
 *   CAN 接收线程          : 收帧 -> 环形缓冲
 *   协议解析线程          : 环形缓冲 -> gb27930 状态机 -> 存储队列
 *   存储线程              : 存储队列 -> SQLite
 *   **GUI 线程（本模块）** : 每 200 ms 组装一次数据快照并重绘，
 *                            每 30 ms 轮询一次触摸事件
 *
 *   数据一致性说明：GUI 线程只「读」协议上下文与 CAN 统计，
 *   不做任何写操作。读取时可能有微小的竞争（例如刚好读到半更新的
 *   电压值），但对显示而言最多造成一帧的数值跳变，不影响正确性；
 *   为此给显示加锁反而会让实时链路承担锁开销，得不偿失。
 */

#ifndef GUI_APP_H
#define GUI_APP_H

#include <stdint.h>

#include "can_layer.h"
#include "gb27930.h"

#ifdef __cplusplus
extern "C" {
#endif

/** GUI 运行配置 */
typedef struct
{
    can_layer_t  *can;        /**< CAN 层（读统计） */
    gb_context_t *ctx;        /**< 协议上下文（读状态机与物理量） */
    const char   *ifname;     /**< 接口名，显示用 */
    uint32_t      bitrate;    /**< 波特率，显示用 */
    const char   *fb_dev;     /**< framebuffer 设备，NULL = /dev/fb0 */
    const char   *touch_dev;  /**< 触摸设备，NULL = 自动查找 */
    const char   *rec_dir;    /**< 渲染留证目录，NULL = 不导出帧 */
    const char   *db_path;    /**< 主数据库路径（充电历史会话和它同一个文件） */
    int           rec_limit;  /**< 最多导出多少帧（0 = 默认 24） */
    int           touch_grab; /**< 1 = 强制 EVIOCGRAB 独占触摸 */
    int           no_touch_grab; /**< 1 = 即使检测到桌面也不自动抓取 */
    int           touch_debug;   /**< 1 = 打印触摸事件（只走日志，屏幕不画调试图形） */
    int           touch_swap;    /**< 1 = 交换触摸 X/Y */
    int           touch_mirror_x;/**< 1 = 触摸 X 轴镜像 */
    int           touch_mirror_y;/**< 1 = 触摸 Y 轴镜像 */
    int           cal_xmin, cal_xmax, cal_ymin, cal_ymax; /**< 手动原始量程 */
    int           cal_valid;     /**< 1 = 上面的量程有效（内核上报不准时用） */
} gui_app_cfg_t;

/**
 * @brief  启动 GUI 线程
 * @return 0 成功；-1 framebuffer 打开失败；-2 触摸打开失败；-3 线程创建失败
 * @note   framebuffer 或触摸打不开时会打印明确原因并返回失败，
 *         调用者可以继续以纯终端模式运行，不影响 CAN 核心功能。
 */
int  gui_app_start(const gui_app_cfg_t *cfg);

/** 请求 GUI 线程退出并等待其回收 */
void gui_app_stop(void);

/** GUI 是否处于活动状态 */
int  gui_app_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* GUI_APP_H */
