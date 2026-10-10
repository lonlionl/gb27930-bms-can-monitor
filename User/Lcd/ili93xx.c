/**
  ******************************************************************************
  * @file    ili93xx.c
  * @brief   ILI9488 面板初始化序列与读 ID
  *
  * 为什么单独一个文件
  * ------------------
  *   和正点原子的文件划分保持一致：lcd.c 管「总线怎么访问」，
  *   ili93xx.c 管「面板寄存器怎么配」。以后支持别的面板
  *   （ILI9341 / NT35310 / ST7796 …）时只改这一个文件，
  *   lcd.c 与整个界面层一行都不用动。
  *
  * 关于分辨率与方向
  * ----------------
  *   ILI9488 的面板原生是 320x480 竖屏。本工程要的是 480x320 横屏，
  *   靠 MADCTL(0x36) 的 MV 位交换行列来实现。
  *
  *   MADCTL 逐位含义（高 4 位就是「方向」的全部）：
  *       bit7 MY = 1  上下翻转（行方向）
  *       bit6 MX = 1  左右翻转（列方向）★ 想左右镜像就翻这一位
  *       bit5 MV = 1  交换行列（竖屏 / 横屏切换）
  *       bit3 BGR= 1  RGB/BGR 顺序（颜色发蓝/发红就翻它）
  *
  *   本工程默认 **0x28 = MV + BGR**，即：
  *       480x320 横屏、左右不镜像、BGR 顺序
  *   ——按需求「屏幕需要左右镜像」的要求，这里已经关掉了 MX 位。
  *
  *   常用取值对照：
  *      0x28  480x320 横屏             ← 本工程默认（左右不镜像）
  *      0x68  480x320 横屏 + 左右镜像   ← 若反了就把 MX 打开
  *      0xA8  480x320 横屏 + 上下镜像
  *      0xE8  480x320 横屏 + 上下左右都镜像（180°）
  *      0x48  320x480 竖屏（原生方向）
  *
  *   【注意】MX 是「相对于面板原生扫描方向」的翻转。面板装成什么样、
  *   排线怎么走，都会影响最终观感，所以这里只能给出常见组合 ——
  *   上电看一眼，不对就按上面的表换一个值，改一处即可。
  ******************************************************************************
  */

#include "lcd.h"

/*==============================================================================
 *                              可调参数
 *============================================================================*/

/** MADCTL：方向 + 颜色顺序。左右镜像就翻转 0x40 这一位（见文件头说明） */
#define LCD_MADCTL      0x28

/** 0x3A 像素格式：0x55 = 16 位 RGB565（界面用这个） */
#define LCD_PIXFMT      0x55

/*==============================================================================
 *                              内部函数
 *============================================================================*/
static void ILI9488_InitSeq(void);

/*==============================================================================
 *                              读 ID
 *
 *   0xD3 = Read ID4，ILI9488 返回 4 个字节：00 00 94 88。
 *   前两个字节是 dummy，需要先读掉。
 *   注意：只有 FSMC 的读时序配对了才能读回来；如果读回 0x0000 或 0xFFFF，
 *   基本可以判定 RS 地址线（LCD_RS_BIT）或数据总线有问题。
 *============================================================================*/
uint16_t ILI93xx_ReadID(void)
{
    uint16_t id_hi;
    uint16_t id_lo;

    LCD_WR_REG(0xD3);
    (void)LCD_RD_DATA();            /* dummy 1 */
    (void)LCD_RD_DATA();            /* dummy 2 */
    id_hi = (uint16_t)(LCD_RD_DATA() & 0x00FFu);
    id_lo = (uint16_t)(LCD_RD_DATA() & 0x00FFu);

    return (uint16_t)((id_hi << 8) | id_lo);
}

/*==============================================================================
 *                              初始化
 *============================================================================*/

void ILI93xx_Init(void)
{
    ILI9488_InitSeq();
}

/**
  * @brief  ILI9488 初始化序列
  *
  *   这一段是 ILI9488 的标准上电流程，顺序不能乱：
  *     Adjust Control 3 → Power Control 1/2/3 → VCOM → Interface Mode
  *     → Frame Rate → Display Inversion → Display Function
  *     → 正/负 Gamma → MADCTL → 像素格式 → 反显 → 退出睡眠 → 开显示
  *
  *   【踩过的坑】ILI9488 与 ILI9341 的部分寄存器编号相同但取值完全不同
  *   （例如 0xC0 Power Control 1：ILI9341 是 0x19，ILI9488 是 0x13）。
  *   如果把 ILI9341 的初始化表套到 ILI9488 上，屏会亮但颜色发暗、发灰，
  *   甚至整屏偏色。所以这里用的是 ILI9488 自己的参数表。
  */
static void ILI9488_InitSeq(void)
{
    /* ---------- 软复位 ---------- */
    LCD_WR_REG(0x01);               /* Software Reset */
    LCD_DelayMs(120);

    /* ---------- Adjust Control 3（ILI9488 特有的上电调整） ---------- */
    LCD_WR_REG(0xF7);
    LCD_WR_DATA(0xA9); LCD_WR_DATA(0x51);
    LCD_WR_DATA(0x2C); LCD_WR_DATA(0x82);

    /* ---------- Power Control ---------- */
    LCD_WR_REG(0xC0);               /* Power Control 1: Vreg1out / Verg2out */
    LCD_WR_DATA(0x13); LCD_WR_DATA(0x13);

    LCD_WR_REG(0xC1);               /* Power Control 2: VGH / VGL */
    LCD_WR_DATA(0x41);

    LCD_WR_REG(0xC2);               /* Power Control 3: VCI 电压 */
    LCD_WR_DATA(0x44);

    LCD_WR_REG(0xC5);               /* VCOM Control */
    LCD_WR_DATA(0x00); LCD_WR_DATA(0x00);
    LCD_WR_DATA(0x00); LCD_WR_DATA(0x00);

    /* ---------- 接口与刷新率 ---------- */
    LCD_WR_REG(0xB0);               /* Interface Mode Control：16 位并口用默认 */
    LCD_WR_DATA(0x00);

    LCD_WR_REG(0xB1);               /* Frame Rate Control */
    LCD_WR_DATA(0xB0); LCD_WR_DATA(0x11);

    LCD_WR_REG(0xB4);               /* Display Inversion Control：列反显 */
    LCD_WR_DATA(0x02);

    LCD_WR_REG(0xB6);               /* Display Function Control */
    LCD_WR_DATA(0x02);              /* 扫描方向 + RGB 顺序 */
    LCD_WR_DATA(0x02);              /* 5.5 英寸屏的时序参数 */
    LCD_WR_DATA(0x3B);              /* 480 行（0x13B = 315+1 行区间参数） */

    /* ---------- Gamma 校正 ---------- */
    LCD_WR_REG(0xE0);               /* Positive Gamma */
    LCD_WR_DATA(0x00); LCD_WR_DATA(0x03); LCD_WR_DATA(0x09); LCD_WR_DATA(0x08);
    LCD_WR_DATA(0x16); LCD_WR_DATA(0x0A); LCD_WR_DATA(0x3F); LCD_WR_DATA(0x78);
    LCD_WR_DATA(0x4C); LCD_WR_DATA(0x09); LCD_WR_DATA(0x0A); LCD_WR_DATA(0x08);
    LCD_WR_DATA(0x16); LCD_WR_DATA(0x1A); LCD_WR_DATA(0x0F);

    LCD_WR_REG(0xE1);               /* Negative Gamma */
    LCD_WR_DATA(0x00); LCD_WR_DATA(0x16); LCD_WR_DATA(0x19); LCD_WR_DATA(0x03);
    LCD_WR_DATA(0x0F); LCD_WR_DATA(0x05); LCD_WR_DATA(0x32); LCD_WR_DATA(0x45);
    LCD_WR_DATA(0x46); LCD_WR_DATA(0x04); LCD_WR_DATA(0x0E); LCD_WR_DATA(0x0D);
    LCD_WR_DATA(0x35); LCD_WR_DATA(0x37); LCD_WR_DATA(0x0F);

    /* ---------- 方向与像素格式 ---------- */
    LCD_WR_REG(0x36);               /* MADCTL */
    LCD_WR_DATA(LCD_MADCTL);

    LCD_WR_REG(0x3A);               /* Pixel Format */
    LCD_WR_DATA(LCD_PIXFMT);

    /* ---------- 反显 ----------
     * 3.5 寸模块大多是常黑（Normally Black）面板，必须开反显，
     * 否则颜色会是负片效果（黑变白、白变黑）。 */
    LCD_WR_REG(0x21);               /* Display Inversion ON */

    /* ---------- 退出睡眠并开显示 ---------- */
    LCD_WR_REG(0x11);               /* Sleep Out */
    LCD_DelayMs(120);               /* 手册要求 ≥ 120 ms */

    LCD_WR_REG(0x29);               /* Display ON */
    LCD_DelayMs(50);
}

/******************* (C) COPYRIGHT 2026 GB27930 Project *****END OF FILE********/
