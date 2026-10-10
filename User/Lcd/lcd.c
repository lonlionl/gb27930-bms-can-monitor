/**
  ******************************************************************************
  * @file    lcd.c
  * @brief   TFTLCD 驱动实现 —— GPIO + FSMC + 绘图原语
  *
  * 设计说明
  * --------
  *   1. 只做「界面需要的事」：初始化、设窗口、填充、画点、画线。
  *      文字由 User/Ui/font16.h + ui_port.c 负责，驱动不掺和，
  *      这样字模只有一份，两端（STM32 / I.MX6ULL）显示完全一致。
  *
  *   2. 性能的关键是「一次设窗口，连续灌数据」：
  *      面板的 GRAM 写入是自增地址的，设好一次窗口之后连续写 N 个像素
  *      只需要一次 FSMC 地址切换。逐点调用 LCD_SetPixel 会为每个像素
  *      重设一次窗口（每次 11 个寄存器写），慢 10 倍以上。
  *      界面里所有矩形、进度条、字模都走 LCD_FillRect，所以整屏刷新才够快。
  *
  *   3. 不依赖任何外部模块：
  *      · 延时用 SysTick 计数器（BMS_Protocol_Init 已经把 SysTick 配成 1 ms，
  *        这里只读它的当前值做短延时，不改配置）；
  *      · 字库不依赖 font.h；
  *      · 不与 timer.h / delay.h 之类的工程其它文件耦合。
  ******************************************************************************
  */

#include "lcd.h"
#include <stdio.h>

/*==============================================================================
 *                              内部常量
 *============================================================================*/

/* FSMC Bank1 NOR/SRAM4（NE4）—— 与 lcd.h 里的 LCD_ADDR_BASE 对应 */
#define LCD_FSMC_BANK       FSMC_Bank1_NORSRAM4

/*==============================================================================
 *                              内部函数声明
 *============================================================================*/
static void LCD_GPIO_Init(void);
static void LCD_FSMC_Init(void);

/*==============================================================================
 *                              SysTick 短延时
 *
 *   SysTick 由 BMS_Protocol_Init() 配成 1 ms 周期、并已使能，
 *   这里用它的当前计数值（LOAD/VAL）做精确的短延时，
 *   既不需要额外的定时器，也不会干扰协议层的毫秒时基。
 *
 *   注意：即使 SysTick 没使能，下面的循环也不会死等 ——
 *   加了最大自旋次数兜底，最坏情况退化成忙等。
 *============================================================================*/

/** 等待 SysTick 计数器走过 n 个 tick（1 tick = 1 ms） */
static void LCD_DelayTicks(uint32_t ticks)
{
    uint32_t last = SysTick->VAL;
    uint32_t load = SysTick->LOAD + 1u;      /* LOAD 是重装值，周期是 LOAD+1 */
    uint32_t elapsed = 0;
    uint32_t spin = 0;

    if (load == 0u) { load = 72000u; }       /* SysTick 没配好时的兜底 */

    while (elapsed < ticks)
    {
        uint32_t now = SysTick->VAL;

        /* VAL 是递减计数：now < last 说明没跨过 0，差值就是走过的 tick */
        if (now <= last)
        {
            elapsed += (last - now);
        }
        else
        {
            elapsed += (last + (load - now));
        }
        last = now;

        if (++spin > 2000000u) { break; }    /* 兜底：绝不在这里卡死 */
    }
}

void LCD_DelayMs(uint16_t ms)
{
    LCD_DelayTicks((uint32_t)ms);
}

/*==============================================================================
 *                              GPIO / FSMC 初始化
 *============================================================================*/

/**
  * @brief  LCD 相关 GPIO
  *
  *   精英板 V2 的 TFTLCD 接口用的是 FSMC Bank1 NE4 + A10，引脚如下：
  *     PD0/PD1        : FSMC_D2/D3
  *     PD4/PD5        : FSMC_NOE/NWE
  *     PD8~PD11       : FSMC_D13~D16
  *     PD14/PD15      : FSMC_D0/D1
  *     PE7~PE15       : FSMC_D4~D12
  *     PG0            : FSMC_A10  → LCD_RS
  *     PG12           : FSMC_NE4  → LCD_CS
  *     PD3            : LCD_RST   （普通推挽输出）
  *     PB0            : LCD_BL    （普通推挽输出，高电平点亮）
  */
static void LCD_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD | RCC_APB2Periph_GPIOE |
                           RCC_APB2Periph_GPIOG | LCD_BL_CLK, ENABLE);

    /* ---- 数据/控制总线：复用推挽 ---- */
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;

    /* PD0 PD1 PD4 PD5 PD8 PD9 PD10 PD11 PD14 PD15 */
    gpio.GPIO_Pin = GPIO_Pin_0  | GPIO_Pin_1  | GPIO_Pin_4  | GPIO_Pin_5  |
                    GPIO_Pin_8  | GPIO_Pin_9  | GPIO_Pin_10 | GPIO_Pin_11 |
                    GPIO_Pin_14 | GPIO_Pin_15;
    GPIO_Init(GPIOD, &gpio);

    /* PE7~PE15 */
    gpio.GPIO_Pin = GPIO_Pin_7  | GPIO_Pin_8  | GPIO_Pin_9  | GPIO_Pin_10 |
                    GPIO_Pin_11 | GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14 |
                    GPIO_Pin_15;
    GPIO_Init(GPIOE, &gpio);

    /* PG0  = FSMC_A10 → LCD_RS */
    gpio.GPIO_Pin = GPIO_Pin_0;
    GPIO_Init(GPIOG, &gpio);

    /* PG12 = FSMC_NE4 → LCD_CS */
    gpio.GPIO_Pin = GPIO_Pin_12;
    GPIO_Init(GPIOG, &gpio);

    /* ---- LCD_RST：普通推挽输出，初始拉高（不复位） ---- */
    gpio.GPIO_Pin   = GPIO_Pin_3;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOD, &gpio);
    GPIO_SetBits(GPIOD, GPIO_Pin_3);

    /* ---- 背光：普通推挽输出 ---- */
    gpio.GPIO_Pin   = LCD_BL_PIN;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LCD_BL_PORT, &gpio);
    GPIO_ResetBits(LCD_BL_PORT, LCD_BL_PIN);    /* 先关背光，等初始化完再开，
                                                   避免上电瞬间花屏刺眼 */
}

/**
  * @brief  FSMC 配置：把 NE4 当作 16 位 SRAM 来访问 LCD
  * @note   时序参数按 8080 并口屏的典型值给（写周期约 100 ns 量级）。
  *         ILI9488 的写时序要求 tWR ≥ 15 ns、tRC ≥ 45 ns，
  *         在 72 MHz、HCLK=72 MHz 下：
  *           写：ADDSET=3, DATAST=3 → (3+1)+(3+1) = 8 HCLK ≈ 111 ns  满足
  *           读：ADDSET=3, DATAST=6 → (3+1)+(6+1) = 11 HCLK ≈ 153 ns 满足
  *         如果偶尔出现花屏/雪花点，把 DATAST 调大 1~2 即可。
  */
static void LCD_FSMC_Init(void)
{
    FSMC_NORSRAMInitTypeDef        fsmc;
    FSMC_NORSRAMTimingInitTypeDef  readTiming;
    FSMC_NORSRAMTimingInitTypeDef  writeTiming;

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_FSMC, ENABLE);

    /* ---- 读时序（读 ID、读 GRAM 用） ---- */
    readTiming.FSMC_AddressSetupTime       = 3;
    readTiming.FSMC_AddressHoldTime        = 1;
    readTiming.FSMC_DataSetupTime          = 6;
    readTiming.FSMC_BusTurnAroundDuration  = 0;
    readTiming.FSMC_CLKDivision            = 0;
    readTiming.FSMC_DataLatency            = 0;
    readTiming.FSMC_AccessMode             = FSMC_AccessMode_A;

    /* ---- 写时序（画图用） ---- */
    writeTiming.FSMC_AddressSetupTime      = 3;
    writeTiming.FSMC_AddressHoldTime       = 1;
    writeTiming.FSMC_DataSetupTime         = 3;
    writeTiming.FSMC_BusTurnAroundDuration = 0;
    writeTiming.FSMC_CLKDivision           = 0;
    writeTiming.FSMC_DataLatency           = 0;
    writeTiming.FSMC_AccessMode            = FSMC_AccessMode_A;

    fsmc.FSMC_Bank                  = LCD_FSMC_BANK;
    fsmc.FSMC_DataAddressMux        = FSMC_DataAddressMux_Disable;
    fsmc.FSMC_MemoryType            = FSMC_MemoryType_SRAM;
    fsmc.FSMC_MemoryDataWidth       = FSMC_MemoryDataWidth_16b;
    fsmc.FSMC_BurstAccessMode       = FSMC_BurstAccessMode_Disable;
    fsmc.FSMC_AsynchronousWait      = FSMC_AsynchronousWait_Disable;
    fsmc.FSMC_WaitSignalPolarity    = FSMC_WaitSignalPolarity_Low;
    fsmc.FSMC_WrapMode              = FSMC_WrapMode_Disable;
    fsmc.FSMC_WaitSignalActive      = FSMC_WaitSignalActive_BeforeWaitState;
    fsmc.FSMC_WriteOperation        = FSMC_WriteOperation_Enable;
    fsmc.FSMC_WaitSignal            = FSMC_WaitSignal_Disable;
    fsmc.FSMC_ExtendedMode          = FSMC_ExtendedMode_Enable;  /* 读写用不同时序 */
    fsmc.FSMC_WriteBurst            = FSMC_WriteBurst_Disable;
    fsmc.FSMC_ReadWriteTimingStruct = &readTiming;
    fsmc.FSMC_WriteTimingStruct     = &writeTiming;

    FSMC_NORSRAMInit(&fsmc);
    FSMC_NORSRAMCmd(LCD_FSMC_BANK, ENABLE);
}

/*==============================================================================
 *                              对外接口
 *============================================================================*/

void LCD_Init(void)
{
    uint16_t id;

    LCD_GPIO_Init();
    LCD_FSMC_Init();

    /* 硬件复位：拉低 >10 us（这里给 10 ms 富余），再拉高等 120 ms */
    GPIO_ResetBits(GPIOD, GPIO_Pin_3);
    LCD_DelayMs(10);
    GPIO_SetBits(GPIOD, GPIO_Pin_3);
    LCD_DelayMs(120);

    id = ILI93xx_ReadID();
    printf("[LCD] 面板 ID = 0x%04X\r\n", (unsigned int)id);

    if (id == 0x0000u || id == 0xFFFFu)
    {
        printf("[LCD] 读不到有效 ID —— 面板不会正常显示。请依次检查：\r\n");
        printf("      1) LCD_RS_BIT 是否为 10（lcd.h，FSMC_A10 接 LCD_RS）\r\n");
        printf("      2) FSMC 数据线 PD0/PD1/PD8~PD11/PD14/PD15 + PE7~PE15 是否接好\r\n");
        printf("      3) 排针是否插反 / 是否插到底\r\n");
    }

    ILI93xx_Init();

    LCD_BackLight(1);
    LCD_Clear(BLACK);

    printf("[LCD] 初始化完成：%ux%u 横屏，16bpp\r\n",
           (unsigned int)LCD_W, (unsigned int)LCD_H);
}

uint16_t LCD_ReadID(void)
{
    return ILI93xx_ReadID();
}

void LCD_BackLight(uint8_t on)
{
    if (on)
    {
        GPIO_SetBits(LCD_BL_PORT, LCD_BL_PIN);
    }
    else
    {
        GPIO_ResetBits(LCD_BL_PORT, LCD_BL_PIN);
    }
}

/*==============================================================================
 *                              绘图原语
 *============================================================================*/

void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    /* 裁剪：越界会让面板把像素写到窗口外的奇怪位置（表现为屏幕边缘有杂点） */
    if (x0 >= LCD_W) { x0 = LCD_W - 1u; }
    if (y0 >= LCD_H) { y0 = LCD_H - 1u; }
    if (x1 >= LCD_W) { x1 = LCD_W - 1u; }
    if (y1 >= LCD_H) { y1 = LCD_H - 1u; }
    if (x1 < x0) { x1 = x0; }
    if (y1 < y0) { y1 = y0; }

    LCD_WR_REG(0x2A);                       /* Column Address Set */
    LCD_WR_DATA(x0 >> 8); LCD_WR_DATA(x0 & 0xFF);
    LCD_WR_DATA(x1 >> 8); LCD_WR_DATA(x1 & 0xFF);

    LCD_WR_REG(0x2B);                       /* Page Address Set */
    LCD_WR_DATA(y0 >> 8); LCD_WR_DATA(y0 & 0xFF);
    LCD_WR_DATA(y1 >> 8); LCD_WR_DATA(y1 & 0xFF);

    LCD_WR_REG(0x2C);                       /* Memory Write —— 之后的数据都是像素 */
}

void LCD_FillRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    uint32_t total;
    uint32_t i;

    /* 先归一化成闭区间再往下走。
     * 这一步不能省：万一调用方传进来 x1 < x0，
     * 下面算像素个数的减法会溢出成一个天文数字，把面板刷爆。 */
    if (x1 < x0) { x1 = x0; }
    if (y1 < y0) { y1 = y0; }

    if (x0 >= LCD_W || y0 >= LCD_H) { return; }

    LCD_SetWindow(x0, y0, x1, y1);

    /* 上面的 LCD_SetWindow 已经做过裁剪，这里按裁剪后的范围算像素个数 */
    if (x1 >= LCD_W) { x1 = LCD_W - 1u; }
    if (y1 >= LCD_H) { y1 = LCD_H - 1u; }

    total = (uint32_t)(x1 - x0 + 1u) * (uint32_t)(y1 - y0 + 1u);

    /* 连续写 GRAM：地址自动自增，所以这里只管灌数据 */
    for (i = 0; i < total; i++)
    {
        LCD_WR_DATA(color);
    }
}

void LCD_Fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color)
{
    LCD_FillRect(sx, sy, ex, ey, color);
}

void LCD_Clear(uint16_t color)
{
    LCD_FillRect(0, 0, LCD_W - 1u, LCD_H - 1u, color);
}

void LCD_SetPixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= LCD_W || y >= LCD_H) { return; }
    LCD_SetWindow(x, y, x, y);
    LCD_WR_DATA(color);
}

/**
  * @brief  Bresenham 直线
  * @note   刻意不逐点调 LCD_SetPixel（那会为每个像素重设一次窗口），
  *         这里先把每个像素位置收集起来不现实（RAM 有限），
  *         所以还是逐点，但界面的曲线绘制量很小（一帧几百个像素），可以接受。
  *         真正的大面积绘制（面板底、进度条、字模）都走 LCD_FillRect。
  */
void LCD_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    int16_t  dx, dy, sx, sy, err;
    uint16_t guard = 0;

    dx = (int16_t)((x1 > x0) ? (x1 - x0) : (x0 - x1));
    dy = (int16_t)((y1 > y0) ? (y1 - y0) : (y0 - y1));
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = (int16_t)(dx - dy);

    for (;;)
    {
        LCD_SetPixel(x0, y0, color);

        if (x0 == x1 && y0 == y1) { break; }
        if (++guard > 2048u) { break; }        /* 兜底，防止参数异常时死循环 */

        {
            int16_t e2 = (int16_t)(err << 1);
            if (e2 > -dy) { err = (int16_t)(err - dy); x0 = (uint16_t)((int16_t)x0 + sx); }
            if (e2 <  dy) { err = (int16_t)(err + dx); y0 = (uint16_t)((int16_t)y0 + sy); }
        }
    }
}

void LCD_DrawRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    LCD_DrawLine(x0, y0, x1, y0, color);
    LCD_DrawLine(x0, y1, x1, y1, color);
    LCD_DrawLine(x0, y0, x0, y1, color);
    LCD_DrawLine(x1, y0, x1, y1, color);
}

/******************* (C) COPYRIGHT 2026 GB27930 Project *****END OF FILE********/
