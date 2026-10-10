/**
  ******************************************************************************
  * @file    ctiic.c
  * @brief   电容触摸屏专用软件 I2C 实现
  ******************************************************************************
  */

#include "ctiic.h"

/* 需要 NULL：stm32f10x.h 不带标准库头文件，这里显式包含一下。 */
#include <stddef.h>

/*==============================================================================
 *                              默认引脚
 *
 *   精英板的电容触摸模块，I2C 最常用的是 PB6(SCL) / PB7(SDA)
 *   —— 和板载 24C02 共用一条总线（两个器件地址不同，互不干扰）。
 *   如果探测不到，touch.c 会自动再去试 ctiic 候选表里的其它组合。
 *============================================================================*/
#define CT_SCL_PORT_DEF     GPIOB
#define CT_SCL_PIN_DEF      GPIO_Pin_6
#define CT_SDA_PORT_DEF     GPIOB
#define CT_SDA_PIN_DEF      GPIO_Pin_7

#define CT_SCL_RCC_DEF      RCC_APB2Periph_GPIOB
#define CT_SDA_RCC_DEF      RCC_APB2Periph_GPIOB

static ct_iic_pins_t s_pins;

/** ACK 等待轮询上限（官方用 250），够慢的从机也能应答 */
#define CT_ACK_WAIT     250u

/*==============================================================================
 *                              内部工具
 *============================================================================*/

/** 把 GPIO_Pin_x 掩码转成 0~15 的引脚号 */
static uint8_t pin_index(uint16_t pin)
{
    uint8_t i;

    for (i = 0; i < 16u; i++)
    {
        if (pin == (uint16_t)(1u << i))
        {
            return i;
        }
    }
    return 0xFFu;
}

/** 取某个引脚所在的 RCC_APB2 时钟位 */
static uint32_t port_rcc(GPIO_TypeDef *port)
{
    if (port == GPIOA) { return RCC_APB2Periph_GPIOA; }
    if (port == GPIOB) { return RCC_APB2Periph_GPIOB; }
    if (port == GPIOC) { return RCC_APB2Periph_GPIOC; }
    if (port == GPIOD) { return RCC_APB2Periph_GPIOD; }
    if (port == GPIOE) { return RCC_APB2Periph_GPIOE; }
    if (port == GPIOF) { return RCC_APB2Periph_GPIOF; }
    if (port == GPIOG) { return RCC_APB2Periph_GPIOG; }
    return 0u;
}

/**
  * @brief  直接改 CRL/CRH，把引脚切成「输出推挽」或「上拉输入」
  *
  *   为什么不用 GPIO_Init()：每传一个 bit 都要切换一次方向，
  *   一次触摸扫描要切上百次，用 GPIO_Init() 会把扫描时间拖到毫秒级。
  *   直接写配置寄存器只要几条指令。
  */
/* 引脚工作模式 */
#define PIN_PP_OUT      0       /* 0011 推挽输出 50MHz */
#define PIN_PU_IN       1       /* 1000 输入 + 上拉（ODR=1） */
#define PIN_OD_OUT      2       /* 0111 开漏输出 50MHz */

/**
  * @brief  直接改 CRL/CRH，切换引脚工作模式
  *
  *   为什么不用 GPIO_Init()：每传一个 bit 都要动一次配置，
  *   一次触摸扫描要切上百次，用 GPIO_Init() 会把扫描拖到毫秒级。
  *   直接写配置寄存器只要几条指令。
  */
static void pin_mode(GPIO_TypeDef *port, uint16_t pin, uint8_t mode)
{
    uint8_t  idx = pin_index(pin);
    volatile uint32_t *cr;
    uint32_t shift;
    uint32_t val;

    if (idx == 0xFFu) { return; }

    cr    = (idx < 8u) ? &port->CRL : &port->CRH;
    shift = (uint32_t)(idx & 7u) * 4u;

    val  = *cr;
    val &= ~(0xFUL << shift);
    switch (mode)
    {
        case PIN_PU_IN:  val |= (0x8UL << shift); port->ODR |= pin; break;
        case PIN_OD_OUT: val |= (0x7UL << shift); break;
        default:         val |= (0x3UL << shift); break;
    }
    *cr = val;
}

/*----------------------------------------------------------------------------
 * SDA 用「开漏输出」而不是「推挽 / 输入 来回切」
 *
 * 这是照抄正点原子官方 ctiic.c 的关键一点，官方注释原文：
 *     "SDA引脚模式设置,开漏输出,上拉,
 *      这样就不用再设置IO方向了, 开漏输出的时候(=1), 也可以读取外部信号的高低电平"
 *
 * 好处有两个：
 *   1. 免掉每个 bit 的方向切换 —— 切换那一瞬间引脚状态不确定，
 *      如果 ACK 正好在这个窗口采样，读到的 0/1 就是随机的。
 *      这正是之前"没有器件也能读到 ID、有器件却读不出 ID"的原因。
 *   2. 主机写 1 时是"释放"总线而不是主动拉高，
 *      从机拉低时不会出现推挽对推挽的短路。
 *
 * 前提：总线上必须有上拉电阻（正点原子的触摸模块上有）。
 * 所以 CT_IIC_Init() 会先放开 SDA 读一次 —— 读不到高电平就说明没上拉，
 * 这时自动退回"推挽 + 上拉输入"的老办法，两种板子都能用。
 *--------------------------------------------------------------------------*/
static uint8_t s_od_ok = 1;     /* 1 = 总线上有上拉，可以走开漏方案 */

static void scl_out(void) { pin_mode(s_pins.scl_port, s_pins.scl_pin, PIN_PP_OUT); }

static void sda_out(void)
{
    pin_mode(s_pins.sda_port, s_pins.sda_pin,
             s_od_ok ? PIN_OD_OUT : PIN_PP_OUT);
}

static void sda_in(void)
{
    /* 开漏模式下写 1 就是释放，IDR 随时能读，不需要切方向 */
    if (!s_od_ok)
    {
        pin_mode(s_pins.sda_port, s_pins.sda_pin, PIN_PU_IN);
    }
}

static void scl_h(void) { s_pins.scl_port->BSRR = s_pins.scl_pin; }
static void scl_l(void) { s_pins.scl_port->BRR  = s_pins.scl_pin; }
static void sda_h(void) { s_pins.sda_port->BSRR = s_pins.sda_pin; }
static void sda_l(void) { s_pins.sda_port->BRR  = s_pins.sda_pin; }

static uint8_t sda_read(void)
{
    return (s_pins.sda_port->IDR & s_pins.sda_pin) ? 1u : 0u;
}

/**
  * @brief  I2C 位延时
  * @note   不做精确标定，靠循环次数凑出约 100 kHz 上下。
  *         触摸屏数据量小，快一点慢一点都无所谓；
  *         关键是「慢到模块能跟上」，所以宁可偏慢。
  */
static void iic_delay(void)
{
    volatile uint8_t i;
    for (i = 0; i < 20u; i++)
    {
        ;
    }
}

/*==============================================================================
 *                              对外：引脚配置
 *============================================================================*/

void CT_IIC_SetPins(const ct_iic_pins_t *pins)
{
    if (pins != NULL)
    {
        s_pins = *pins;
    }
    else
    {
        s_pins.scl_port = CT_SCL_PORT_DEF;
        s_pins.scl_pin  = CT_SCL_PIN_DEF;
        s_pins.sda_port = CT_SDA_PORT_DEF;
        s_pins.sda_pin  = CT_SDA_PIN_DEF;
        s_pins.name     = "PB6/PB7";
    }
}

const ct_iic_pins_t *CT_IIC_GetPins(void)
{
    return &s_pins;
}

void CT_IIC_Init(void)
{
    uint32_t rcc;

    if (s_pins.scl_port == NULL)
    {
        CT_IIC_SetPins(NULL);
    }

    rcc = port_rcc(s_pins.scl_port) | port_rcc(s_pins.sda_port);
    RCC_APB2PeriphClockCmd(rcc, ENABLE);

    /* 空闲状态：两根线都拉高 */
    scl_out(); sda_out();
    scl_h();   sda_h();

    /* ---- 探测总线上有没有上拉电阻 ----
     * 开漏方案的前提是：SDA 在"释放"（写 1）状态下能被上拉电阻拉到高电平。
     * 这里先按开漏放开 SDA，等几个位时间再读回：
     *     读回 1 -> 有上拉，走开漏方案（官方做法，最稳）
     *     读回 0 -> 悬空，开漏永远读不到从机，自动退回
     *               "推挽输出 + 上拉输入"的老方案
     * 这样两种板子都能用，不会因为模块上没上拉就彻底失灵。 */
    iic_delay();
    iic_delay();
    iic_delay();
    iic_delay();
    s_od_ok = sda_read();

    if (!s_od_ok)
    {
        sda_in();               /* 切回上拉输入，后续按推挽方案走 */
    }
}

void CT_IIC_Release(void)
{
    scl_out(); sda_out();
    scl_h();   sda_h();
}

/*==============================================================================
 *                              时序原语
 *============================================================================*/

static void iic_start(void)
{
    sda_out();
    sda_h(); scl_h(); iic_delay();
    sda_l();          iic_delay();
    scl_l();          iic_delay();
}

static void iic_stop(void)
{
    sda_out();
    scl_l(); sda_l(); iic_delay();
    scl_h();          iic_delay();
    sda_h();          iic_delay();
}

/** 发一个字节，返回从机应答（0 = 有应答） */
static uint8_t iic_send_byte(uint8_t dat)
{
    uint8_t i;
    uint8_t ack;

    sda_out();
    scl_l();

    for (i = 0; i < 8u; i++)
    {
        if (dat & 0x80u) { sda_h(); } else { sda_l(); }
        dat = (uint8_t)(dat << 1);
        iic_delay();
        scl_h();
        iic_delay();
        scl_l();
        iic_delay();
    }

    /* 第 9 个时钟读应答
     *
     * 照抄官方 ct_iic_wait_ack()：先把 SDA 放开（写 1 = 释放），
     * 再把 SCL 拉高，然后**轮询等待**从机把 SDA 拉低。
     * 之前这里是"拉高后只采样一次"，只要从机拉得稍慢一点就会被判成 NACK。 */
    sda_h();                    /* 释放 SDA，交给从机 */
    iic_delay();
    scl_h();
    iic_delay();

    ack = 1u;                   /* 先当作没应答 */
    for (i = 0; i < CT_ACK_WAIT; i++)
    {
        if (sda_read() == 0u)   /* 从机把 SDA 拉低了 = ACK */
        {
            ack = 0u;
            break;
        }
        iic_delay();
    }

    scl_l();
    iic_delay();

    return ack;     /* 0 = ACK */
}

/** 收一个字节；ack = 1 表示还要继续读（回 NACK 给从机） */
static uint8_t iic_recv_byte(uint8_t ack)
{
    uint8_t i;
    uint8_t dat = 0;

    sda_in();
    sda_h();                    /* 释放 SDA，让从机来驱动 */
    for (i = 0; i < 8u; i++)
    {
        scl_l();
        iic_delay();
        scl_h();
        iic_delay();
        dat = (uint8_t)(dat << 1);
        if (sda_read()) { dat |= 1u; }
        iic_delay();
    }

    /* 主机应答 */
    scl_l();
    sda_out();
    if (ack) { sda_l(); } else { sda_h(); }
    iic_delay();
    scl_h();
    iic_delay();
    scl_l();
    iic_delay();
    sda_h();

    return dat;
}

/*==============================================================================
 *                              通用传输
 *============================================================================*/

/**
  * @brief  写：START + dev + (reg) + data + STOP
  * @param  dev     7 位地址左移 1 位后的值（写地址）
  * @param  reg     寄存器地址
  * @param  reg_len 寄存器地址字节数（1 = FT，2 = GT）
  */
static uint8_t iic_write(uint8_t dev, uint16_t reg, uint8_t reg_len,
                         const uint8_t *data, uint16_t len)
{
    uint16_t i;
    uint8_t  err = 0;

    iic_start();

    if (iic_send_byte(dev) != 0u) { err = 1; goto done; }

    if (reg_len == 2u)
    {
        if (iic_send_byte((uint8_t)(reg >> 8)) != 0u) { err = 1; goto done; }
    }
    if (iic_send_byte((uint8_t)(reg & 0xFFu)) != 0u) { err = 1; goto done; }

    for (i = 0; i < len; i++)
    {
        if (iic_send_byte(data[i]) != 0u) { err = 1; goto done; }
    }

done:
    iic_stop();
    return err;
}

/**
  * @brief  读：START + dev写 + reg + START + dev读 + 数据… + STOP
  */
static uint8_t iic_read(uint8_t dev_w, uint8_t dev_r, uint16_t reg,
                        uint8_t reg_len, uint8_t *buf, uint16_t len)
{
    uint16_t i;
    uint8_t  err = 0;

    iic_start();

    if (iic_send_byte(dev_w) != 0u) { err = 1; goto done; }

    if (reg_len == 2u)
    {
        if (iic_send_byte((uint8_t)(reg >> 8)) != 0u) { err = 1; goto done; }
    }
    if (iic_send_byte((uint8_t)(reg & 0xFFu)) != 0u) { err = 1; goto done; }

    iic_start();                        /* 重复起始条件 */
    if (iic_send_byte(dev_r) != 0u) { err = 1; goto done; }

    for (i = 0; i < len; i++)
    {
        buf[i] = iic_recv_byte((i + 1u < len) ? 1u : 0u);
    }

done:
    iic_stop();
    return err;
}

/*==============================================================================
 *                              对外接口
 *============================================================================*/

uint8_t CT_IIC_WriteReg8(uint8_t dev, uint8_t reg, uint8_t data)
{
    return iic_write(dev, (uint16_t)reg, 1u, &data, 1u);
}

uint8_t CT_IIC_ReadReg8(uint8_t dev, uint8_t reg, uint8_t *buf, uint16_t len)
{
    return iic_read(dev, (uint8_t)(dev | 1u), (uint16_t)reg, 1u, buf, len);
}

uint8_t CT_IIC_WriteReg16(uint8_t dev, uint16_t reg, uint8_t data)
{
    return iic_write(dev, reg, 2u, &data, 1u);
}

uint8_t CT_IIC_ReadReg16(uint8_t dev, uint16_t reg, uint8_t *buf, uint16_t len)
{
    return iic_read(dev, (uint8_t)(dev | 1u), reg, 2u, buf, len);
}

uint8_t CT_IIC_Probe(uint8_t dev)
{
    uint8_t ack;

    iic_start();
    ack = iic_send_byte(dev);
    iic_stop();

    return ack;     /* 0 = 有器件应答 */
}

/******************* (C) COPYRIGHT 2026 GB27930 Project *****END OF FILE********/
