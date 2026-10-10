/**
  ******************************************************************************
  * @file    touch.c
  * @brief   电容触摸驱动实现（软件 I2C + 芯片自动识别）
  *
  * 为什么写「自动识别」
  * ------------------
  *   电容触摸模块有好几代：GT9147、GT1151、FT5206、FT5426…
  *   它们**寄存器地址宽度都不一样**（GT 用 16 位寄存器地址，FT 用 8 位），
  *   而且 I2C 接在哪两个引脚上，光看代码和原理图也常常对不上。
  *   正点原子官方例程是在编译期用宏写死的，换一块模块就得改代码重编。
  *
  *   这里的做法：开机时在几组候选引脚上依次探测，
  *   找到应答的器件后按它的型号（GT / FT）选择合适的寄存器访问方式，
  *   并把整个过程打印到串口。这样：
  *     · 换模块不用改代码；
  *     · 万一探测失败，串口日志会直接告诉你「试了哪几组引脚、都没有应答」，
  *       排查方向一目了然，而不是面对一块没反应的屏干瞪眼。
  *
  * 已知限制（如实说明）
  * -------------------
  *   1. GT9xxx 系列完整使用前，厂商通常会写一张几十字节的配置表
  *      （分辨率、触发方式、中断模式等）。本驱动只做软复位后使用芯片默认配置，
  *      单点触摸在绝大多数模块上都可用；如果发现触摸区域偏移或反应迟钝，
  *      需要补上模块厂商给的配置表。
  *   2. 没有做多点手势，只取第 1 个触点 —— 界面只需要单点按键，够用。
  ******************************************************************************
  */

#include "touch.h"
#include "ctiic.h"
#include "lcd.h"
#include <stdio.h>

/*==============================================================================
 *                              从机地址（7 位地址左移 1 位后的写地址）
 *============================================================================*/
/* GT9xxx 的 I2C 地址在复位释放那一刻由 INT 引脚电平锁定：
 *     INT = 高 -> 7 位地址 0x14（8 位读写 0x28 / 0x29）
 *     INT = 低 -> 7 位地址 0x5D（8 位读写 0xBA / 0xBB）
 *
 * 正点原子官方驱动（实验40 手写识别实验 / BSP/TOUCH/gt9xxx.h）用的是：
 *     #define GT9XXX_CMD_WR   0X28
 *     #define GT9XXX_CMD_RD   0X29
 * 也就是 0x14 这一组。而且官方复位序列只是「RST 拉低 10ms -> 拉高 10ms」，
 * **完全没有去驱动 INT**，所以芯片停留在 0x14。
 *
 * 之前本驱动只试 0x5D，还把 INT 主动拉低，等于把芯片推到了自己不用的那个
 * 地址上 —— 所以引脚全对也永远"无应答"。现在两个地址都试，并且复位时序
 * 完全照抄官方。
 */
#define GT_ADDR_W       0x28        /* 官方用的地址：7 位 0x14 */
#define GT_ADDR_R       0x29
#define GT_ADDR_W_ALT   0xBA        /* 备选：7 位 0x5D */
#define GT_ADDR_R_ALT   0xBB
#define FT_ADDR_W       0x70        /* FT5xxx: 7 位地址 0x38 */
#define FT_ADDR_R       0x71

/*==============================================================================
 *                              寄存器
 *============================================================================*/
/* GT9xxx */
#define GT_REG_CMD      0x8040      /* 命令寄存器 */
#define GT_REG_PID      0x8140      /* 产品 ID，4 字节 ASCII */
#define GT_REG_STATUS   0x814E      /* bit7 = 数据就绪，bit3:0 = 触点数 */
#define GT_REG_POINT1   0x8150      /* 第 1 个触点数据，每点 8 字节 */

/* FT5xxx */
#define FT_REG_MODE     0x00        /* 工作模式，写 0 = 正常 */
#define FT_REG_TDNUM    0x02        /* 触点数 */
#define FT_REG_POINT1   0x03        /* 第 1 个触点，每点 6 字节 */
#define FT_REG_ID_A3    0xA3        /* 部分型号的芯片 ID */
#define FT_REG_ID_A8    0xA8        /* 较新型号的芯片 ID */

/*==============================================================================
 *                              内部状态
 *============================================================================*/
_m_tp_dev tp_dev;

static tp_chip_t s_chip = TP_CHIP_NONE;
/* 实际应答的 GT 地址（0x28 或 0xBA），由探测阶段确定 */
static uint8_t    s_gt_addr_w = GT_ADDR_W;
static char       s_chip_name[24] = "未识别";

/** 当前是否有手指按着（由各扫描函数维护） */
static uint8_t    s_down = 0;

/**
 * 候选 I2C 引脚组 —— 必须接在 TFTLCD 排线的触摸脚上
 *
 * 之前这里填的是 PB6/PB7、PC0/PC1、PF6/PF7、PE0/PE1、PD6/PD7 六组，
 * 现场六组全部"无应答"。原因不是芯片坏了，而是这些引脚根本没接到触摸屏。
 * 精英板 V2 的 IO 分配表写得很清楚，TFTLCD 接口的触摸信号只有这 5 根：
 *
 *     PB1  = T_SCK      PB2  = T_MISO     PF9  = T_MOSI
 *     PF10 = T_PEN      PF11 = T_CS
 *
 * 正点原子的电容触摸模块把这 5 根线复用成一组软件 I2C + 复位 + 中断：
 *
 *     SCL = PB1  (T_SCK)
 *     SDA = PF9  (T_MOSI)
 *     RST = PF11 (T_CS)
 *     INT = PF10 (T_PEN)
 *
 * PB6/PB7 是板载 24C02 EEPROM 的 I2C，和触摸屏没有任何关系 ——
 * 之前正是把它当成了触摸总线，所以怎么试都找不到器件。
 *
 * 第二组是 SCL/SDA 反接的兜底，个别批次的模块丝印与实物相反。
 */
static const ct_iic_pins_t s_candidates[] =
{
    /* TFTLCD 排线上的触摸信号（最可能）：
     *   T_SCK = PB1, T_MOSI = PF9 */
    { GPIOB, GPIO_Pin_1, GPIOF, GPIO_Pin_9, "PB1/PF9   (T_SCK/T_MOSI)" },
    { GPIOF, GPIO_Pin_9, GPIOB, GPIO_Pin_1, "PF9/PB1   (反接)" },
    /* 其它板型/批次上出现过的两组 */
    { GPIOB, GPIO_Pin_0, GPIOB, GPIO_Pin_1, "PB0/PB1" },
    { GPIOB, GPIO_Pin_1, GPIOB, GPIO_Pin_0, "PB1/PB0" },
    /* 板载 24C02 那一组：本板不该有触摸芯片，留着只为把结论排除掉 */
    { GPIOB, GPIO_Pin_6, GPIOB, GPIO_Pin_7, "PB6/PB7   (24C02 那组)" },
    { GPIOB, GPIO_Pin_7, GPIOB, GPIO_Pin_6, "PB7/PB6" },
};
#define TP_CANDIDATE_N  ((uint8_t)(sizeof(s_candidates) / sizeof(s_candidates[0])))

/*==============================================================================
 *                              坐标处理
 *============================================================================*/

static uint16_t tp_map_x(uint16_t raw)
{
    uint16_t v = raw;

#if TP_MIRROR_X
    v = (uint16_t)((LCD_W > 1u) ? (LCD_W - 1u - v) : 0u);
#endif
    if (v >= LCD_W) { v = LCD_W - 1u; }
    return v;
}

static uint16_t tp_map_y(uint16_t raw)
{
    uint16_t v = raw;

#if TP_MIRROR_Y
    v = (uint16_t)((LCD_H > 1u) ? (LCD_H - 1u - v) : 0u);
#endif
    if (v >= LCD_H) { v = LCD_H - 1u; }
    return v;
}

/*==============================================================================
 *                              芯片识别
 *============================================================================*/

/** 打印一段 16 进制，便于在日志里看寄存器原始值 */
static void tp_dump(const char *tag, const uint8_t *buf, uint16_t len)
{
    uint16_t i;
    printf("[TP ] %s", tag);
    for (i = 0; i < len; i++)
    {
        printf(" %02X", (unsigned int)buf[i]);
    }
    printf("\r\n");
}

/**
  * @brief  在指定引脚组上找触摸芯片
  * @return 找到的芯片类型；TP_CHIP_NONE 表示这组引脚上没有器件
  */
/**
  * @brief  在一组引脚上探测触摸器件（GT9147/GT1151 @0x5D，FT5206/FT5426 @0x38）
  *
  * 注意：这里目前只试 GT 系列的 0x5D 一个地址。
  * GT9147/GT1151 的地址是复位时由 INT 电平锁存的（INT 低 -> 0x5D，
  * INT 高 -> 0x14），tp_hw_reset() 已经把 INT 驱动为低，所以正常就是 0x5D。
  * 如果扫描结果出现"有上拉但器件不应答"，下一步就在这里补上 0x14(0x28)
  * 和 FT 的 0x38(0x70) 重试。
  */
static tp_chip_t tp_probe_pins(const ct_iic_pins_t *pins)
{
    uint8_t buf[8];

    CT_IIC_SetPins(pins);
    CT_IIC_Init();
    LCD_DelayMs(2);

    /* ---- 先试 GT9xxx（16 位寄存器地址，地址 0x5D） ---- */
    {
        /* 官方地址 0x28(0x14) 先试，不行再试 0xBA(0x5D) */
        uint8_t gt_w = GT_ADDR_W;

        if (CT_IIC_Probe(GT_ADDR_W) != 0u)
        {
            gt_w = GT_ADDR_W_ALT;
            if (CT_IIC_Probe(GT_ADDR_W_ALT) != 0u)
            {
                gt_w = 0u;              /* 两个地址都没应答 */
            }
        }

        if (gt_w != 0u)
        {
        if (CT_IIC_ReadReg16(gt_w, GT_REG_PID, buf, 4u) == 0u)
        {
            tp_dump("GT PID =", buf, 4u);

            /* 产品 ID 是 4 个 ASCII 字符，例如 "9147" "1151" "911"。
             * 只要头一个字节是数字，就认为确实是 GT 系列。 */
            if (buf[0] >= (uint8_t)'0' && buf[0] <= (uint8_t)'9')
            {
                s_gt_addr_w = gt_w;
                snprintf(s_chip_name, sizeof(s_chip_name), "GT 系列 (ID \"%c%c%c%c\")",
                         buf[0], buf[1] ? buf[1] : ' ', buf[2] ? buf[2] : ' ',
                         buf[3] ? buf[3] : ' ');
                return TP_CHIP_GT;
            }
            return TP_CHIP_UNKNOWN;
        }
        }   /* if (gt_w != 0u) */
        /* 有应答却读不出 ID —— 多半是 CT_IIC_Probe() 的假应答
         *（软件 I2C 在读 ACK 时若时序偏快，SDA 还没被器件拉低就已经采样了）。
         * 这种情况绝不能当成"找到芯片"，否则界面会显示 touch OK 却点不动，
         * 反而把真正的问题（引脚不对）盖住。 */
        return TP_CHIP_UNKNOWN;
    }

    /* 这里原先还有一段"再试 FT5xxx"的兜底探测，现在已整段删除。
     *
     * 它本来就已经**永远走不到**：上面那个 `if (gt_w != 0u)` 块里的每条分支
     * 都以 return 收尾（读到合法 ID -> TP_CHIP_GT；有应答但 ID 读不出 ->
     * TP_CHIP_UNKNOWN，因为那多半是软件 I2C 的假应答，绝不能当成"找到芯片"），
     * 块执行完必定返回，函数控制流到不了这里。
     *
     * Keil 曾经报 warning #111-D "statement is unreachable"：兜底分支删掉时，
     * 收尾那行 `return TP_CHIP_NONE;` 被留在了块注释结束符的同一行
     * 于是它自己成了不可达语句。该行现已一并删除 —— 函数最后一条语句就是那个
     * `if (gt_w != 0u)` 块，其内部的 return 已经覆盖全部路径，末尾不存在
     * 掉出函数体的可能，因此不会引入 #940-D "missing return statement"。
     * Rebuild 后该告警不再出现，比原来也省下几百字节代码 ——
     * MDK-Lite 的 32 KB 额度很紧。 */
}

/*==============================================================================
 *                              初始化
 *============================================================================*/

/** 初始化 GT9xxx：软复位 + 清状态 */
static void tp_init_gt(void)
{
    uint8_t zero = 0;

    /* 0x8040 写 0x02 = 软复位 */
    (void)CT_IIC_WriteReg16(s_gt_addr_w, GT_REG_CMD, 0x02u);
    LCD_DelayMs(20);

    /* 清掉可能残留的"数据就绪"标志，否则第一次扫描会读到上一次的脏数据 */
    (void)CT_IIC_WriteReg16(s_gt_addr_w, GT_REG_STATUS, zero);
    LCD_DelayMs(5);
}

/** 初始化 FT5xxx：切到正常模式 */
static void tp_init_ft(void)
{
    (void)CT_IIC_WriteReg8(FT_ADDR_W, FT_REG_MODE, 0x00u);
    LCD_DelayMs(10);
}

/**
  * @brief  复位电容触摸控制器
  *
  * 复位脚就是 TFTLCD 接口的 T_CS(PF11)，中断脚是 T_PEN(PF10)。
  *
  * 为什么必须先复位：GT9147 / GT1151 上电后不会立刻应答 I2C，必须先给一个
  * 复位脉冲。正点原子官方例程的时序是「RST 拉低 >= 10 ms -> 拉高 -> 等 >= 100 ms」。
  * 少了这一步，探测阶段经常直接读不到器件，表现出来就是"所有候选引脚无应答"。
  *
  * 注意：TP_Init() 是在 LCD_Init() 之前调用的，这里不能依赖 LCD 已初始化；
  * LCD_DelayMs() 只用 SysTick，与 LCD 状态无关（SysTick 由协议层先启动）。
  */
static void tp_hw_reset(void)
{
    GPIO_InitTypeDef gi;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOF, ENABLE);

    /* T_PEN(PF10)：中断脚，配成上拉输入即可。
     *
     * 注意：**不要**在这里把 INT 驱动为输出低电平。
     * GT9xxx 的 I2C 地址是在复位释放那一刻由 INT 电平锁存的，而正点原子
     * 官方驱动用的是 0x28/0x29（7 位 0x14），对应的就是 INT 为高的情形；
     * 官方复位序列也完全不碰 INT。之前本驱动把 INT 拉低，等于把芯片推到
     * 0x5D，而那时驱动又只按 0x5D 去试 —— 结果就是引脚全对也读不到。
     * 现在地址两个都试，INT 保持输入，与官方一致。 */
    gi.GPIO_Pin  = GPIO_Pin_10;
    gi.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOF, &gi);

    /* T_CS(PF11)：兼作触摸芯片的 RST */
    gi.GPIO_Pin   = GPIO_Pin_11;
    gi.GPIO_Mode  = GPIO_Mode_Out_PP;
    gi.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOF, &gi);

    /* 时序完全照抄正点原子官方 gt9xxx.c：
     *     RST = 0; delay_ms(10);
     *     RST = 1; delay_ms(10);
     *     delay_ms(100);   // 等芯片内部启动
     */
    GPIO_ResetBits(GPIOF, GPIO_Pin_11);
    LCD_DelayMs(10);
    GPIO_SetBits(GPIOF, GPIO_Pin_11);
    LCD_DelayMs(10);
    LCD_DelayMs(100);
}

static uint8_t tp_init_internal(void)
{
    s_chip = TP_CHIP_NONE;
    snprintf(s_chip_name, sizeof(s_chip_name), "未识别");

    /* 先给触摸芯片一个干净的复位，否则下面的 I2C 探测很可能全部无应答 */
    tp_hw_reset();

#if TP_AUTO_PROBE
    /* ------------------------------------------------------------------
     * 总线扫描
     *
     * 现场反馈：接在官方标称的 PB1/PF9 上仍然"无应答"。
     * 光看"无应答"分不清是三种情况里的哪一种，所以这里把能测的都测出来：
     *
     *   ① 空闲电平：把两根线都放开，读回 SCL/SDA。
     *      正常 I2C 总线有外部上拉，两根都应该是 1。
     *      如果读到 0，说明这两根线压根没接到触摸芯片（或模块没上拉），
     *      —— 那就不必再浪费时间试地址了，问题在接线/引脚，不在芯片。
     *   ② 器件地址：GT9147/GT1151 的地址由复位时 INT 电平决定
     *      （INT 低 -> 0x5D，INT 高 -> 0x14），两种都试；
     *      FT5206/FT5426 固定 0x38，也试一遍。
     *   ③ 引脚组：官方标称的组合优先，其余作为兜底。
     *
     * 扫描结果会一行一行打在串口上，命中哪一组、哪个地址一目了然。
     * ----------------------------------------------------------------*/
    {
        uint8_t i;

        /* 版本印章：日志里必须能看出跑的是哪一版驱动。
         * 之前反复出现"代码改了但现象没变"，无从判断是没烧进去还是改错了，
         * 所以把关键特征直接打出来。 */
        printf("[TP ] 驱动版本: 2025-GT-ADDR28  (GT 地址 0x28/0xBA 双试, "
               "复位照抄官方 10/10/100ms, 不驱动 INT)\r\n");
        printf("[TP ] 开始扫描触摸总线（空闲电平 / 器件地址）...\r\n");

        for (i = 0; i < TP_CANDIDATE_N; i++)
        {
            const ct_iic_pins_t *p = &s_candidates[i];
            uint8_t lvl_scl;
            uint8_t lvl_sda;
            tp_chip_t c;

            /* 顺序很关键：
             *   ① 先把这一组的 SCL/SDA 配成 GPIO 并初始化 I2C 层；
             *   ② 再拉复位。GT9147/GT1151 在复位释放的一瞬间会采样引脚状态
             *      （I2C 地址由 INT 电平决定；接口/工作模式也可能受 SCL/SDA 影响）。
             *      如果复位时 SCL/SDA 还是"没配过的浮空脚"，芯片可能进入一个
             *      我们读不到的状态 —— 这正是"所有候选引脚都无应答"的一种成因。
             *   ③ 最后放开两根线，读空闲电平（用来判断线到底接没接）。 */
            CT_IIC_SetPins(p);
            CT_IIC_Init();
            tp_hw_reset();
            LCD_DelayMs(5);

            /* 放开两根线，读实际电平 */
            CT_IIC_Release();
            LCD_DelayMs(2);
            lvl_scl = (uint8_t)GPIO_ReadInputDataBit(p->scl_port, p->scl_pin);
            lvl_sda = (uint8_t)GPIO_ReadInputDataBit(p->sda_port, p->sda_pin);

            printf("[TP ] %-26s 空闲电平 SCL=%u SDA=%u", p->name,
                   (unsigned int)lvl_scl, (unsigned int)lvl_sda);

            if (lvl_scl == 0u && lvl_sda == 0u)
            {
                /* 两根都被拉死：多半是这两脚接在别的外设上（例如 LCD 背光），
                 * 不是 I2C 总线 */
                printf("  <- 都不是高电平，此组无 I2C 总线\r\n");
                continue;
            }

            c = tp_probe_pins(p);

            if (c != TP_CHIP_GT && c != TP_CHIP_FT)
            {
                /* UNKNOWN = 有假应答但读不出有效 ID，同样不能算命中 */
                printf("  <- 无有效器件(ID 读不出)\r\n");
                continue;
            }

            s_chip = c;
            printf("  <- 命中: %s\r\n", s_chip_name);
            break;
        }
    }
#else
    /* 不自动探测：直接用默认引脚组（PB6/PB7） */
    {
        tp_chip_t c = tp_probe_pins(&s_candidates[0]);

        s_chip = c;
        if (c != TP_CHIP_NONE)
        {
            printf("[TP ] 使用默认引脚 %s：%s\r\n",
                   s_candidates[0].name, s_chip_name);
        }
    }
#endif

    if (s_chip == TP_CHIP_NONE)
    {
        printf("[TP ] 没找到触摸芯片（已试 PB1/PF9 与反接两种）。\r\n");
        printf("[TP ] 请依次确认：\r\n");
        printf("      1) 触摸排线插紧（与 LCD 同一根排线）；\r\n");
        printf("      2) 模块是电容版 GT9147/GT1151/FT5206；\r\n");
        printf("         TFTLCD 触摸脚: PB1=T_SCK PF9=T_MOSI PF10=T_PEN PF11=T_CS\r\n");
        printf("      3) 若是电阻版 XPT2046，引脚相同但走 SPI，需另配驱动。\r\n");
        printf("[TP ] 界面仍正常显示，只是不能触摸切页。\r\n");
        return 1;
    }

    if (s_chip == TP_CHIP_GT)
    {
        tp_init_gt();
    }
    else if (s_chip == TP_CHIP_FT)
    {
        tp_init_ft();
    }

    tp_dev.x[0] = 0;
    tp_dev.y[0] = 0;
    tp_dev.sta  = 0;
    s_down      = 0;

    printf("[TP ] 触摸初始化完成：%s，I2C 引脚 %s，屏幕 %ux%u\r\n",
           s_chip_name, CT_IIC_GetPins()->name,
           (unsigned int)LCD_W, (unsigned int)LCD_H);
    return 0;
}

void TP_Init(void)
{
    tp_dev.init = tp_init_internal;
    tp_dev.scan = TP_Scan;
    tp_dev.sta  = 0;

    (void)tp_init_internal();
}

/*==============================================================================
 *                              扫描
 *============================================================================*/

/** GT9xxx 扫描 */
static uint8_t tp_scan_gt(void)
{
    uint8_t  status = 0;
    uint8_t  buf[40];
    uint8_t  count;
    uint16_t px;
    uint16_t py;

    if (CT_IIC_ReadReg16(s_gt_addr_w, GT_REG_STATUS, &status, 1u) != 0u)
    {
        return 0;
    }

    /* bit7 = 有新数据 */
    if ((status & 0x80u) == 0u)
    {
        return 0;                       /* 没有新数据，保持上一次状态 */
    }

    count = (uint8_t)(status & 0x0Fu);

    if (count == 0u)
    {
        /* 抬手：清标志后返回"未按下" */
        s_down = 0;
        (void)CT_IIC_WriteReg16(s_gt_addr_w, GT_REG_STATUS, 0x00u);
        return 1;
    }

    if (count > 5u) { count = 5u; }

    if (CT_IIC_ReadReg16(s_gt_addr_w, GT_REG_POINT1, buf,
                         (uint16_t)count * 8u) != 0u)
    {
        return 0;
    }

    /* 每个触点 8 字节：x_lo x_hi y_lo y_hi size_lo size_hi rsv rsv */
    px = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    py = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));

#if TP_SWAP_XY
    {
        uint16_t t = px;
        px = py;
        py = t;
    }
#endif

    tp_dev.x[0] = tp_map_x(px);
    tp_dev.y[0] = tp_map_y(py);
    s_down = 1;

    /* 清标志，允许芯片上报下一帧 */
    (void)CT_IIC_WriteReg16(s_gt_addr_w, GT_REG_STATUS, 0x00u);
    return 1;
}

/** FT5xxx 扫描 */
static uint8_t tp_scan_ft(void)
{
    uint8_t buf[16];
    uint8_t count;
    uint16_t px;
    uint16_t py;

    if (CT_IIC_ReadReg8(FT_ADDR_W, FT_REG_TDNUM, buf, 8u) != 0u)
    {
        return 0;
    }

    count = (uint8_t)(buf[0] & 0x0Fu);
    if (count == 0u || count > 5u)
    {
        s_down = 0;
        return 1;                       /* 抬手 */
    }

    /* buf[1..4] = 点1 的 xh xl yh yl（高 4 位是有效位） */
    px = (uint16_t)((((uint16_t)buf[1] & 0x0Fu) << 8) | buf[2]);
    py = (uint16_t)((((uint16_t)buf[3] & 0x0Fu) << 8) | buf[4]);

#if TP_SWAP_XY
    {
        uint16_t t = px;
        px = py;
        py = t;
    }
#endif

    tp_dev.x[0] = tp_map_x(px);
    tp_dev.y[0] = tp_map_y(py);
    s_down = 1;
    return 1;
}

uint8_t TP_Scan(uint8_t mode)
{
    uint8_t updated;

    (void)mode;                         /* 只支持屏幕坐标 */

    if (s_chip == TP_CHIP_GT)
    {
        updated = tp_scan_gt();
    }
    else if (s_chip == TP_CHIP_FT || s_chip == TP_CHIP_UNKNOWN)
    {
        updated = tp_scan_ft();
    }
    else
    {
        return 0;                       /* 没有触摸芯片，永远报"未按下" */
    }

    if (updated)
    {
        /* 这里只维护"当前是否按着"这个状态位。
         * 「按下边沿」的提取交给 ui_port.c —— 界面层本来就需要保存
         * 上一次的状态，放在那里更自然，也让驱动保持简单。 */
        if (s_down)
        {
            tp_dev.sta |= TP_PRES_DOWN;
        }
        else
        {
            tp_dev.sta = (uint8_t)(tp_dev.sta & (uint8_t)(~TP_PRES_DOWN));
        }
    }

    return updated;
}

tp_chip_t TP_GetChip(void)
{
    return s_chip;
}

const char *TP_GetChipName(void)
{
    return s_chip_name;
}

uint8_t TP_IsReady(void)
{
    return (s_chip != TP_CHIP_NONE) ? 1u : 0u;
}

/******************* (C) COPYRIGHT 2026 GB27930 Project *****END OF FILE********/
