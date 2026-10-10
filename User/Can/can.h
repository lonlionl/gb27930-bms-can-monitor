/**
  ******************************************************************************
  * @file    Can/can.h
  * @author  GB27930 BMS Simulator Project
  * @version V1.0.0
  * @date    2025
  * @brief   CAN1 底层驱动接口（GB/T 27930-2015 BMS 报文模拟器）
  *
  * 硬件说明（正点原子 STM32F103ZET6 精英板）：
  *   - CAN1 使用 PA11(CAN_RX) / PA12(CAN_TX)，外接 TJA1050 收发器，板载 120R 终端电阻
  *   - PA11/PA12 为 CAN1 的默认引脚，无需 AFIO 重映射（重映射为 PB8/PB9）
  *   - 总线波特率 250 kbps，APB1 = 36 MHz
  *
  * 位定时计算（250 kbps）：
  *   位时间 = 1 / 250000 = 4 us
  *   取 9 个 Tq：Tq = 4us / 9 = 0.4444 us
  *   波特率预分频 = 36 MHz * 0.4444 us = 16
  *   BS1 = 6 Tq, BS2 = 2 Tq, SJW = 1 Tq, 总 Tq = 1 + 6 + 2 = 9
  *   采样点 = (1 + 6) / 9 = 77.8 %（符合 CiA 推荐的 75%~87.5%）
  ******************************************************************************
  */

#ifndef __CAN_H
#define __CAN_H

#include "stm32f10x.h"

/*==============================================================================
 *                              波特率配置
 *  通过 CAN_BAUDRATE_SEL 宏选择总线波特率，默认 250 kbps（GB/T 27930 规定）
 *============================================================================*/
#define CAN_BAUDRATE_125K            1
#define CAN_BAUDRATE_250K            2
#define CAN_BAUDRATE_500K            3

#define CAN_BAUDRATE_SEL             CAN_BAUDRATE_250K

#if   (CAN_BAUDRATE_SEL == CAN_BAUDRATE_125K)
  /* 36MHz / 16 = 2.25MHz, 18Tq -> 125kbps */
  #define CAN_BT_PRESCALER            16u
  #define CAN_BT_BS1                    CAN_BS1_15tq
  #define CAN_BT_BS2                    CAN_BS2_2tq
  #define CAN_BAUDRATE_TEXT          "125 kbps"
#elif (CAN_BAUDRATE_SEL == CAN_BAUDRATE_250K)
  /* 36MHz / 16 = 2.25MHz, 9Tq -> 250kbps */
  #define CAN_BT_PRESCALER            16u
  #define CAN_BT_BS1                    CAN_BS1_6tq
  #define CAN_BT_BS2                    CAN_BS2_2tq
  #define CAN_BAUDRATE_TEXT          "250 kbps"
#elif (CAN_BAUDRATE_SEL == CAN_BAUDRATE_500K)
  /* 36MHz / 8 = 4.5MHz, 9Tq -> 500kbps */
  #define CAN_BT_PRESCALER            8u
  #define CAN_BT_BS1                    CAN_BS1_6tq
  #define CAN_BT_BS2                    CAN_BS2_2tq
  #define CAN_BAUDRATE_TEXT          "500 kbps"
#else
  #error "CAN_BAUDRATE_SEL 取值非法"
#endif

#define CAN_BT_SJW                      CAN_SJW_1tq

/*==============================================================================
 *                              硬件资源宏
 *============================================================================*/
#define CANx                         CAN1
#define CAN_CLK                      RCC_APB1Periph_CAN1

#define CAN_GPIO_CLK                 RCC_APB2Periph_GPIOA
#define CAN_GPIO_PORT                GPIOA
#define CAN_RX_GPIO_PIN              GPIO_Pin_11          /* CAN1_RX */
#define CAN_TX_GPIO_PIN              GPIO_Pin_12          /* CAN1_TX */

/* 中断向量名（STM32F103 大容量产品：RX0 与 USB 低优先级中断共用向量） */
#define CAN_RX_IRQn                  USB_LP_CAN1_RX0_IRQn
#define CAN_RX_IRQHandler            USB_LP_CAN1_RX0_IRQHandler
#define CAN_SCE_IRQn                 CAN1_SCE_IRQn
#define CAN_SCE_IRQHandler           CAN1_SCE_IRQHandler

#define CAN_RX_IRQ_PREEMPT_PRIORITY  1                    /* 抢占优先级 */
#define CAN_RX_IRQ_SUB_PRIORITY      0                    /* 子优先级   */
#define CAN_SCE_IRQ_SUB_PRIORITY     1

/*==============================================================================
 *                              接收队列
 *  ISR 只做「取帧 + 入环形队列」两件事，把耗时的协议解析留给主循环，
 *  这是工业级驱动的标准做法，避免中断服务时间过长导致丢帧。
 *============================================================================*/
#define CAN_RX_QUEUE_SIZE            64u                  /* 必须是 2 的幂 */
#define CAN_RX_QUEUE_MASK            (CAN_RX_QUEUE_SIZE - 1u)

/** @brief CAN 帧结构（与协议层解耦，只保留必要字段） */
typedef struct
{
    uint32_t id;        /* 29 位扩展帧 ID（已剥离 IDE/RTR 标志位） */
    uint8_t  data[8];   /* 数据域，最多 8 字节 */
    uint8_t  len;       /* 有效数据长度 DLC (0~8) */
    uint8_t  reserved;  /* 对齐填充，保持结构体 4 字节对齐 */
} CAN_Frame_t;

/*==============================================================================
 *                              对外接口
 *============================================================================*/

void     CAN_Config(void);
uint8_t  CAN_SendFrame(uint32_t ext_id, const uint8_t *data, uint8_t len);
uint8_t  CAN_GetRxFrame(CAN_Frame_t *frame);
uint16_t CAN_GetRxQueueCount(void);

uint32_t CAN_GetRxFrameCount(void);      /* 累计收到的 CAN 帧数          */
uint32_t CAN_GetRxOverflowCount(void);   /* 软件队列溢出丢帧数           */
uint32_t CAN_GetTxFrameCount(void);      /* 累计成功发送的 CAN 帧数      */
uint32_t CAN_GetTxFailCount(void);       /* 发送失败（无空闲邮箱）次数   */
uint32_t CAN_GetErrorCount(void);        /* 累计总线错误中断次数         */
uint8_t  CAN_GetErrorCodeRaw(void);     /* 最近一次 CAN_ESR 错误码      */
uint8_t  CAN_IsBusOff(void);             /* 1 = 总线关闭状态             */

/**
  * @brief  读取 CAN 总线错误计数器与最近错误码（直接读 CAN_ESR 寄存器）
  * @param  tec 发送错误计数器 TEC（0~255，只增不减到 255）
  * @param  rec 接收错误计数器 REC（0~255）
  * @param  lec 最近错误码 LEC：
  *             0=无错 1=填充错误 2=格式错误 3=ACK错误
  *             4=位隐性错误 5=位显性错误 6=CRC错误 7=软件设置
  * @note   排查物理层问题最有用的三个数：
  *           REC 持续增长 → 本节点在总线上看到了电平，但解不出帧（波特率/极性/接线错）
  *           REC 恒为 0 且收不到任何帧 → 本节点根本没看到总线活动（收发器/接线/终端电阻）
  *           LEC=3(ACK错误) → 本节点发了帧但没人应答（对端不在线或没在 ACK）
  */
void     CAN_GetBusCounters(uint8_t *tec, uint8_t *rec, uint8_t *lec);
void     CAN_RecoverBusOff(void);        /* 从 Bus-Off 恢复（重新初始化）*/
void     CAN_ClearStatistics(void);

#endif /* __CAN_H */
