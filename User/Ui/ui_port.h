/**
  ******************************************************************************
  * @file    ui_port.h
  * @brief   STM32 本地界面 —— LCD / 触摸 适配层（唯一需要按驱动版本调整的文件）
  *
  * 为什么要有这一层
  * ----------------
  *   正点原子的 LCD 驱动版本很多（lcd.c 有多套 API，老版本用 POINT_COLOR
  *   全局变量、新版本带 lcddev 结构体；触摸也有 tp_dev.scan() 与
  *   TP_Scan() 两种写法）。如果界面代码直接调用驱动函数，换个驱动版本就要
  *   通篇改一遍。这里把「驱动相关」的部分全部收进本文件（.c 里只有 100
  *   多行），ui_app.c 只使用下面这组 uip_* 接口 —— 驱动换了只改一个文件。
  *
  * 颜色约定
  * --------
  *   全部使用 RGB565（16 位），与 ILI9488 在 16 位色模式下的显存格式一致。
  *   这里不直接引用 lcd.h 里的 BLACK/RED/... 宏，避免与工程里其它模块
  *   重名（正点原子的宏名非常通用，极易冲突）。
  ******************************************************************************
  */

#ifndef __UI_PORT_H
#define __UI_PORT_H

#include "stm32f10x.h"
#include <stdint.h>

/*==============================================================================
 *                          总开关：要不要本地界面
 *
 *   UI_ENABLE = 1  —— 启用 3.5 寸屏界面，工程需要 LCD / 触摸驱动
 *                     （User/Lcd/lcd.c、ili93xx.c、touch.c、ctiic.c）
 *   UI_ENABLE = 0  —— 不要界面。此时 ui_port.c / ui_app.c 会退化成空实现，
 *                     整个工程**不依赖任何 LCD / 触摸驱动**，
 *                     CAN 与 BMS 报文模拟器功能完全不受影响。
 *
 *   为什么要留这个开关：
 *     界面依赖 TFTLCD 硬件与驱动，而 CAN 通信不依赖。如果 LCD 驱动还没弄好
 *     就先被编译错误卡住，连 CAN 都调不了 —— 那是最不划算的。
 *     所以这里做成一个宏：屏幕没调通时先置 0，把 CAN 跑通，
 *     等驱动就绪再置 1 打开界面。main.c / stm32f10x_it.c 一行都不用改。
 *
 *   两种改法都支持：
 *     · 直接把下面这行的 1 改成 0；
 *     · 或者在 Keil 的 C/C++ → Define 里加 `UI_ENABLE=0`
 *       （用 #ifndef 包着就是为了让命令行定义能覆盖默认值）。
 *============================================================================*/
#ifndef UI_ENABLE
#define UI_ENABLE       1
#endif

/*==============================================================================
 *                              屏幕分辨率上限
 *   ILI9488 在正点原子精英板上是 480x320（横屏）。
 *   这里给出编译期上限，界面布局用它分配静态数组；实际运行时以
 *   uip_width()/uip_height() 返回的实时值为准（支持旋转）。
 *============================================================================*/
#define UIP_MAX_W       480
#define UIP_MAX_H       320

/*==============================================================================
 *                              工业风配色（RGB565）
 *   与 Linux 端 framebuffer 界面保持同一套配色，两端看起来是一套产品。
 *============================================================================*/
#define UIP_BG          0x18C3      /* 背景     深灰蓝 #141820 的 565 近似 */
#define UIP_PANEL       0x2104      /* 面板底色                            */
#define UIP_BORDER      0x3A49      /* 边框                                */
#define UIP_TEXT        0xEF5D      /* 主文字（近白）                      */
#define UIP_DIM         0x8C71      /* 次要文字（灰）                      */
#define UIP_GREEN       0x3EAC      /* 正常   绿                           */
#define UIP_YELLOW      0xF644      /* 警告   黄                           */
#define UIP_RED         0xF2CB      /* 故障   红                           */
#define UIP_BLUE        0x4CF5      /* 强调   蓝                           */
#define UIP_CYAN        0x467C      /* 标题   青                           */
#define UIP_ORANGE      0xFD07      /* 曲线   橙                           */
#define UIP_TRACK       0x2945      /* 进度条底槽                          */

/*==============================================================================
 *                              生命周期
 *============================================================================*/

/**
  * @brief  初始化 LCD 与触摸（内部会调用正点原子的 LCD_Init / TP_Init）
  * @note   LCD 初始化较慢（约 100~300 ms），只在开机调用一次
  */
void uip_init(void);

/** 屏幕宽 / 高（像素，横屏方向） */
uint16_t uip_width(void);
uint16_t uip_height(void);

/*==============================================================================
 *                              绘图原语
 *   坐标一律为「闭区间」：(x0,y0) 与 (x1,y1) 两个像素都包含在内。
 *============================================================================*/

void uip_clear(uint16_t color);
void uip_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
void uip_pixel(uint16_t x, uint16_t y, uint16_t color);
void uip_hline(uint16_t x0, uint16_t x1, uint16_t y, uint16_t color);
void uip_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);

/** 任意两点间连线（Bresenham，自绘，不依赖驱动的画线函数） */
void uip_line(int x0, int y0, int x1, int y1, uint16_t color);

/** 水平进度条：在 [x0,x1]x[y0,y1] 内按 ratio(0.0~1.0) 填充 */
void uip_bar(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
             int32_t permille, uint16_t fill, uint16_t track);

/*==============================================================================
 *                              文字（GBK）
 *   字模来自 tools/gen_font.py 生成的 font16.h：
 *     · ASCII 0x20~0x7E ：宽 8 像素，步进 8
 *     · 汉字（GBK 双字节）：宽 16 像素，步进 16
 *   STM32 工程是 GBK 编码，字符串字面量里就是 GBK 字节，因此这里直接按
 *   GBK 双字节查表，不需要任何 Unicode 转换表。
 *============================================================================*/

/** 一行 UTF-8/GBK 混合文本的像素宽度 */
uint16_t uip_text_w(const char *txt);

/**
  * @brief  在 (x,y) 画一行文本
  * @param  y 文本单元格（16 像素高）的左上角
  * @return 绘制结束后的 x 坐标
  */
uint16_t uip_text(uint16_t x, uint16_t y, const char *txt, uint16_t color);

/** 以 cx 为水平中心画一行文本 */
void uip_text_center(uint16_t cx, uint16_t y, const char *txt, uint16_t color);

/**
  * @brief  放大 n 倍画一行文本（n = 1 或 2）
  * @note   16 点阵字库直接显示三个主要读数偏小，放大 2 倍后是 32 像素高，
  *         远看也清楚，是工业屏上常见的做法。放大只是把每个亮点画成
  *         n x n 的方块，不做插值，边缘依旧锐利。
  * @return 绘制结束后的 x 坐标
  */
uint16_t uip_text_scale(uint16_t x, uint16_t y, const char *txt,
                        uint16_t color, uint8_t n);

/** 放大 n 倍时的像素宽度 */
uint16_t uip_text_scale_w(const char *txt, uint8_t n);

/** 以 cx 为水平中心、放大 n 倍画一行文本 */
void uip_text_center_scale(uint16_t cx, uint16_t y, const char *txt,
                           uint16_t color, uint8_t n);

/**
  * @brief  在限定宽度内画文本，超出部分截断（不画到框外）
  */
void uip_text_clip(uint16_t x, uint16_t y, uint16_t max_w,
                   const char *txt, uint16_t color);

/*==============================================================================
 *                              触摸
 *============================================================================*/

/**
  * @brief  读取一次触摸，返回「按下」边沿事件
  * @param  x, y 输出触摸坐标（屏幕坐标系）
  * @return 1 = 本次检测到新的按下；0 = 无按下
  * @note   只返回「按下」边沿，界面不必自己做去抖；
  *         一次点击只会返回一次 1，避免连点误触发。
  */
uint8_t uip_touch_read(uint16_t *x, uint16_t *y);

/** 触摸屏是否可用（初始化失败时为 0，界面只显示不响应触摸） */
uint8_t uip_touch_ok(void);

#endif /* __UI_PORT_H */
