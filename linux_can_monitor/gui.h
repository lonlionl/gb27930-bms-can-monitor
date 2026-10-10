/**
 * @file    gui.h
 * @brief   CAN 监控终端图形界面（framebuffer 版，480x272 / 800x480 自适应）
 *
 * 界面结构
 * --------
 *   ┌ 主界面（实时监控）──────────────────────────────────┐
 *   │ 标题栏（含「历史曲线」触摸按钮）                     │
 *   │ 状态机当前状态 / 会话号 / 异常                       │
 *   │ 总电压 / 总电流 / SOC（带进度条，超阈值变色）        │
 *   │ 最高温度 / 最低温度 / 最高单体电压                   │
 *   │ CAN 总线统计（收发帧数 / 错误 / 波特率）             │
 *   │ 存储统计（原始帧 / 解析记录 / 库大小）               │
 *   └─────────────────────────────────────────────────────┘
 *                   │ 点「历史曲线」
 *                   ▼
 *   ┌ 历史界面（电量变化曲线）────────────────────────────┐
 *   │ 标题栏（含「返回」触摸按钮）                         │
 *   │ 曲线区：电压(蓝) / 电流(橙) / SOC(绿) 三轴归一化     │
 *   │ 网格 + Y 轴刻度 + X 轴时间范围                       │
 *   │ 底部统计：电量积分 kWh / 最大最小电压 / 温度范围     │
 *   └─────────────────────────────────────────────────────┘
 *
 * 设计要点
 * --------
 * 1. 界面层只吃一个 gui_data_t 快照 + 一个 gui_curve_t 曲线，
 *    不直接访问 gb27930 / SQLite，因此可以在 PC 上离线渲染自检。
 * 2. 触摸按钮用「矩形命中测试」，不依赖任何 widget 框架。
 * 3. 所有尺寸常量集中在 gui.h，改分辨率只需改 GUI_SCREEN_* 宏。
 */

#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include "fbdev.h"
#include "session.h"   /* 历史九宫格要用 session_info_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 历史曲线最多保存的采样点数 */
#define GUI_MAX_POINTS   512

/**
 * 设计分辨率
 * ----------
 *  界面按 480x272（野火 4.3 寸屏）设计，在更大的屏上会自动居中。
 *  这两个宏同时被两处使用，必须保持一致，所以放在头文件里而不是各自写死：
 *    · gui.c     —— 布局的基准尺寸（DW / DH）
 *    · gui_app.c —— 虚拟 framebuffer 的尺寸（--fb virtual）
 */
#define GUI_DESIGN_W     480
#define GUI_DESIGN_H     272

/** 界面页 */
typedef enum
{
    GUI_SCREEN_DASHBOARD  = 0,  /**< 主界面：实时监控 + 充电按钮 */
    GUI_SCREEN_DATACURVE  = 1,  /**< 数据曲线：本次充电的曲线 */
    GUI_SCREEN_HISTORY    = 2,  /**< 历史：3x3 缩略图 */
    GUI_SCREEN_HISTDETAIL = 3   /**< 历史详情：某一次的完整曲线 */
} gui_screen_t;

/** 充电阶段（界面显示用，来自 i.MX 侧协议层推断） */
typedef enum
{
    GUI_PHASE_STANDBY   = 0,    /**< 待机：还没开始充电 */
    GUI_PHASE_CHARGING  = 1,    /**< 充电中 */
    GUI_PHASE_FULL      = 2,    /**< 充满 */
    GUI_PHASE_DISCHARGE = 3     /**< 放电中：停止充电后按同速率掉电 */
} gui_phase_t;

/** 曲线指标：数据曲线页 / 历史详情页一次只画一条，用按钮切换 */
typedef enum
{
    GUI_METRIC_V = 0,           /**< 电压 V */
    GUI_METRIC_I,               /**< 电流 A */
    GUI_METRIC_T,               /**< 温度 ℃ */
    GUI_METRIC_SOC,             /**< 电量 % */
    GUI_METRIC_COUNT
} gui_metric_t;

/** 可点击的按钮 */
typedef enum
{
    GUI_BTN_NONE = 0,
    GUI_BTN_CHARGE,             /**< 主界面「充电」——相当于插入充电枪 */
    GUI_BTN_DATACURVE,          /**< 主界面「数据曲线」 */
    GUI_BTN_HISTLIST,           /**< 数据曲线页「历史」-> 九宫格 */
    GUI_BTN_BACK,               /**< 返回上一级 */
    GUI_BTN_CELL,               /**< 九宫格缩略图（索引用 gui_history_cell_at 取） */
    GUI_BTN_METRIC              /**< 曲线切换按钮（索引用 gui_metric_button_at 取） */
} gui_btn_t;

/**
 * @brief  点在哪个"曲线切换"按钮上
 * @return 0..3（依次是 电压/电流/温度/电量）；未命中返回 -1
 */
int gui_metric_button_at(int x, int y);

/** 界面渲染所需的数据快照 */
typedef struct
{
    /* --- 状态机 --- */
    gui_phase_t phase;           /**< 充电阶段：待机/充电中/充满/放电中 */
    int         charging_active; /**< 1 = 正在充电流程中（「充电」按钮此时是「停止」） */
    const char *state_name;      /**< 状态机状态文本（gb27930_state_str 的返回值） */
    const char *error_name;      /**< 异常文本 */
    uint32_t    session_id;      /**< 会话编号 */
    int         charging;        /**< 1 = 充电中（标题用绿色高亮） */

    /* --- 实时物理量（has_data = 0 时显示占位符） --- */
    int         has_data;
    double      voltage;         /**< 充电电压测量值 V */
    double      current;         /**< 充电电流测量值 A */
    double      soc;             /**< SOC % */
    double      cell_max_v;      /**< 最高单体电压 V */
    int         cell_max_no;     /**< 最高单体编号 */
    double      temp_max;        /**< 最高温度 ℃ */
    double      temp_min;        /**< 最低温度 ℃ */
    double      limit_v;         /**< 最高允许充电总电压 V（进度条上限） */
    double      limit_i;         /**< 最高允许充电电流 A（进度条上限） */
    double      energy_kwh;      /**< 本次会话累计电量 kWh */
    uint32_t    charge_sec;      /**< 本次会话累计充电秒数 */

    /* --- CAN 总线 --- */
    const char *ifname;
    const char *can_state;       /**< ERROR_ACTIVE / BUS_OFF ... */
    uint32_t    bitrate;
    uint64_t    rx_frames;
    uint64_t    tx_frames;
    uint64_t    err_frames;

    /* --- 存储 --- */
    uint64_t    db_raw;
    uint64_t    db_charge;
    uint64_t    db_size;
} gui_data_t;

/**
 * 曲线数据
 * ---------
 *  横轴是**充电时间**，所以每个点都要带自己的时间戳。
 *  左边界固定 = 本次充电的开始时间，右边界 = 最后一个采样点的时间：
 *    · 历史记录：右边界就是结束时间，固定不动
 *    · 正在充电：右边界随时间一直往右走（曲线一直向右生长）
 *  这样无论从 20% 还是 80% 开始充，曲线都从最左边开始画。
 */
typedef struct
{
    int      n;                              /**< 有效点数 */
    double   v[GUI_MAX_POINTS];              /**< 电压 V */
    double   i[GUI_MAX_POINTS];              /**< 电流 A */
    double   soc[GUI_MAX_POINTS];            /**< SOC % */
    double   temp[GUI_MAX_POINTS];           /**< 最高温度 ℃ */
    int64_t  ts[GUI_MAX_POINTS];             /**< 每个采样点的时间戳（微秒） */
    double   soc_max;                        /**< 本次会话见到的最高电量 %（判断是否真充满） */
    int64_t  ts_first;                       /**< 首点时间戳（微秒） */
    int64_t  ts_last;                        /**< 末点时间戳（微秒） */
    double   energy_kwh;                     /**< 积分电量 kWh */
} gui_curve_t;

/*==============================================================================
 *                              接口
 *============================================================================*/

/** 初始化界面层（记录屏幕尺寸，预计算布局） */
void gui_init(const fbdev_t *fb);

/** 绘制主界面（实时监控） */
void gui_draw_dashboard(fbdev_t *fb, const gui_data_t *d);

/** 绘制数据曲线页（本次充电；has_c = 0 时显示"尚未开始充电"） */
void gui_draw_datacurve(fbdev_t *fb, const gui_data_t *d,
                        const gui_curve_t *c, int has_c, gui_metric_t m);

/**
 * @brief  绘制历史九宫格
 * @param  list 会话列表，**新的在前**（list[0] = 左上角 = 最近一次）
 * @param  n    有效条数（其余格子画成空框）
 */
void gui_draw_histgrid(fbdev_t *fb, const session_info_t *list, int n);

/** 绘制历史详情页（某一次的完整曲线 + 参数） */
void gui_draw_histdetail(fbdev_t *fb, const session_info_t *info,
                         const gui_curve_t *c, int has_c, gui_metric_t m);

/**
 * @brief  九宫格里第 idx 个缩略图是否被点到
 * @return 0..8（0 = 左上角 = 最近一次）；未命中返回 -1
 */
int gui_history_cell_at(int x, int y);

/**
 * @brief  触摸命中测试
 * @param  screen 当前界面
 * @param  x, y   触摸坐标
 * @return 命中的按钮；未命中返回 GUI_BTN_NONE
 * @note   只判定「画出来的那个按钮矩形」，范围与视觉完全一致。
 *         曾经放宽到标题栏右侧一大块，实测手感反而变差（点在按钮旁边
 *         也会切页），量程校准好之后已收紧回精确矩形。
 */
gui_btn_t gui_hit_test(gui_screen_t screen, int x, int y);

/**
 * @brief  取标题栏某个按钮的绘制矩形（画面与命中判定共用同一份几何）
 * @param  screen 当前界面
 * @param  btn    哪个按钮
 * @param  x,y,w,h 输出参数，允许传 NULL
 */
void gui_button_rect(gui_screen_t screen, gui_btn_t btn,
                     int *x, int *y, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* GUI_H */
