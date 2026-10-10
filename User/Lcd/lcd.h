/**
  ******************************************************************************
  * @file    lcd.h
  * @brief   TFTLCD 驱动 —— ILI9488 / FSMC 接口 / 480x320 横屏
  *
  * 硬件连接（正点原子 STM32F103ZET6 精英板 TFTLCD 排针）
  * -----------------------------------------------------
  *   LCD_CS   <- FSMC_NE4   (PG12)
  *   LCD_RS   <- FSMC_A10   (PG0)     ← 74HC573 锁存地址线当命令/数据选择
  *   LCD_WR   <- FSMC_NWE   (PD5)
  *   LCD_RD   <- FSMC_NOE   (PD4)
  *   LCD_RST  <- PD3
  *   LCD_BL   <- PB0        （高电平点亮背光）
  *   数据总线  <- FSMC_D0~D15（PD0/PD1/PD8~PD11/PD14/PD15 + PE7~PE15）
  *
  *   如果换成别的主板，只需要改本文件里的 LCD_RS_BIT、LCD_ADDR_BASE、
  *   LCD_BL_* 与 lcd.c 里的 LCD_FSMC_BANK / 引脚表。
  *
  * 与正点原子官方 lcd.c 的关系
  * ---------------------------
  *   本文件是**功能等价但更精简**的实现，只提供界面真正需要的那几个接口
  *   （初始化、填充、画点、画线、设置窗口）。
  *   刻意不做的事情：
  *     · 不依赖 driver 自带的字库（font.h）—— 界面文字统一走 User/Ui/font16.h，
  *       这样 STM32 与 I.MX6ULL 两端字模完全一致，也不会多占一份 Flash；
  *     · 不依赖外部 timer 模块 —— 内部用 SysTick 计数延时，零依赖。
  ******************************************************************************
  */

#ifndef __LCD_H
#define __LCD_H

#include "stm32f10x.h"

/*==============================================================================
 *                              屏幕尺寸（横屏）
 *============================================================================*/
#define LCD_W           480u        /* 宽 480 像素（横屏） */
#define LCD_H           320u        /* 高 320 像素（横屏） */

/*==============================================================================
 *                              颜色（RGB565）
 *   保留正点原子的常用色名，方便沿用以前的代码。
 *   UI 自己的配色在 User/Ui/ui_port.h 里（UIP_* 前缀），两套互不冲突。
 *============================================================================*/
#define WHITE       0xFFFF
#define BLACK       0x0000
#define BLUE        0x001F
#define DARKBLUE    0x0010
#define RED         0xF800
#define GREEN       0x07E0
#define CYAN        0x07FF
#define YELLOW      0xFFE0
#define MAGENTA     0xF81F
#define ORANGE      0xFD20
#define GRAY        0x8410
#define LGRAY       0xC618
#define DGRAY       0x4208

/*==============================================================================
 *                          FSMC 地址映射
 *
 *   FSMC Bank1 NOR/SRAM4（NE4）基地址 0x6C000000。
 *   16 位数据宽度下，FSMC 的地址线 Ax 对应字节地址的 bit(x+1)，
 *   所以 A10 对应 1<<11 = 0x800：
 *       写 0x6C000000 + 0        → A10 = 0 → RS = 0 → 写命令
 *       写 0x6C000000 + 0x800    → A10 = 1 → RS = 1 → 写数据
 *
 *   【如果屏完全不亮、读 ID 也读不到】先怀疑这里：
 *   FSMC_A10 是不是真的接到了 LCD_RS。改 LCD_RS_BIT 试下列几个常见值：
 *       6  → 1<<7  = 0x080   （部分 F4 板）
 *      10  → 1<<11 = 0x800   （F103 精英板 / 战舰板，本工程的默认值）
 *      16  → 1<<17 = 0x20000
 * ===========================================================================*/
#define LCD_RS_BIT      10
#define LCD_ADDR_BASE   0x6C000000UL

#define LCD_REG_ADDR    ((volatile uint16_t *)(LCD_ADDR_BASE))
#define LCD_RAM_ADDR    ((volatile uint16_t *)(LCD_ADDR_BASE | (1UL << (LCD_RS_BIT + 1))))

#define LCD_WR_REG(reg)     (*LCD_REG_ADDR  = (uint16_t)(reg))
#define LCD_WR_DATA(dat)    (*LCD_RAM_ADDR  = (uint16_t)(dat))
#define LCD_RD_DATA()       (*LCD_RAM_ADDR)

/*==============================================================================
 *                              背光
 *============================================================================*/
#define LCD_BL_PORT     GPIOB
#define LCD_BL_PIN      GPIO_Pin_0
#define LCD_BL_CLK      RCC_APB2Periph_GPIOB

/*==============================================================================
 *                              API
 *============================================================================*/

/** 初始化 LCD：GPIO + FSMC + 面板初始化序列（同时打印 ID 到串口） */
void LCD_Init(void);

/** 读面板 ID（ILI9488 正常应返回 0x9488） */
uint16_t LCD_ReadID(void);

/** 全屏清成一种颜色 */
void LCD_Clear(uint16_t color);

/**
  * @brief  填充矩形（坐标闭区间，(x0,y0) 与 (x1,y1) 都包含）
  * @note   这是界面的主力函数：ui_port.c 的 uip_fill 直接映射到它。
  *         内部一次设好窗口后连续写数据，比逐点画快一个数量级。
  */
void LCD_FillRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);

/** 与正点原子官方驱动同名的别名，方便直接替换 */
void LCD_Fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color);

/** 画一个点 */
void LCD_SetPixel(uint16_t x, uint16_t y, uint16_t color);

/** 画一条直线（Bresenham，自绘，不依赖面板的图形加速） */
void LCD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);

/** 画矩形边框 */
void LCD_DrawRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);

/** 设置显存写入窗口，并把写指针指向 GRAM（0x2C） */
void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/** 开/关背光 */
void LCD_BackLight(uint8_t on);

/** 毫秒级延时（内部用 SysTick 计数，不依赖任何外部 timer 模块） */
void LCD_DelayMs(uint16_t ms);

/*------------------------------------------------------------------------------
 * 面板底层初始化（实现在 ili93xx.c）
 *   之所以单独一个文件，是为了和正点原子的文件划分保持一致，
 *   以后要支持别的面板（ILI9341 / NT35310…）时只改这一个文件。
 *----------------------------------------------------------------------------*/
void     ILI93xx_Init(void);
uint16_t ILI93xx_ReadID(void);

#endif /* __LCD_H */
