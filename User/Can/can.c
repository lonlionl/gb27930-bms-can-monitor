/**
  ******************************************************************************
  * @file    Can/can.c
  * @author  GB27930 BMS Simulator Project
  * @version V1.0.0
  * @date    2025
  * @brief   CAN1 底层驱动实现（250 kbps / 扩展帧 / 接收中断 + 软件环形队列）
  *
  * 设计要点：
  *   1. 硬件滤波器配置为「放行全部扩展数据帧」，BMS 侧需要接收充电机的
  *      CHM / CRM / CTS / CML / CRO / CST / TP.CM 等全部报文，因此不做 ID 过滤；
  *   2. 接收使用 FIFO0 + RX0 中断，中断内仅搬运数据到软件环形队列，
  *      协议解析全部在主循环完成，保证中断服务时间 < 10 us；
  *   3. 使能状态变化中断（SCE），用于监测总线错误、错误被动、Bus-Off 并支持恢复。
  ******************************************************************************
  */

#include "can.h"
#include <stdio.h>

/*==============================================================================
 *                              私有变量
 *============================================================================*/

/* --- 软件接收环形队列（单生产者=ISR，单消费者=主循环）--- */
static volatile CAN_Frame_t s_rx_queue[CAN_RX_QUEUE_SIZE];
static volatile uint16_t    s_rx_head = 0;   /* 写指针（ISR 修改） */
static volatile uint16_t    s_rx_tail = 0;   /* 读指针（主循环修改） */

/* --- 统计量 --- */
static volatile uint32_t s_rx_frame_cnt  = 0;
static volatile uint32_t s_rx_overflow   = 0;
static volatile uint32_t s_tx_frame_cnt  = 0;
static volatile uint32_t s_tx_fail_cnt   = 0;
static volatile uint32_t s_err_cnt       = 0;
static volatile uint8_t  s_last_err_code = 0;
static volatile uint8_t  s_bus_off_flag  = 0;

/*==============================================================================
 *                              私有函数
 *============================================================================*/

/**
  * @brief  配置 CAN1 使用的 GPIO（PA11=RX 上拉输入, PA12=TX 复用推挽）
  */
static void CAN_GPIO_Config(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(CAN_GPIO_CLK, ENABLE);

    /* CAN_RX (PA11)：上拉输入（TJA1050 输出为推挽，此处上拉保证空闲电平稳定） */
    GPIO_InitStructure.GPIO_Pin  = CAN_RX_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(CAN_GPIO_PORT, &GPIO_InitStructure);

    /* CAN_TX (PA12)：复用推挽输出 */
    GPIO_InitStructure.GPIO_Pin  = CAN_TX_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(CAN_GPIO_PORT, &GPIO_InitStructure);
}

/**
  * @brief  配置 CAN1 的 NVIC 中断（RX0 接收中断 + SCE 状态变化中断）
  */
static void CAN_NVIC_Config(void)
{
    NVIC_InitTypeDef NVIC_InitStructure;

    /* 中断优先级分组：2 位抢占 + 2 位子优先级
     * 注意：NVIC_PriorityGroupConfig 在整个工程中只能调用一次，
     *      本工程由 bsp_usart.c 首次调用，这里做幂等保护。 */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

    /* --- CAN1 RX0 接收中断（高优先级，保证不丢帧）--- */
    NVIC_InitStructure.NVIC_IRQChannel = CAN_RX_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = CAN_RX_IRQ_PREEMPT_PRIORITY;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = CAN_RX_IRQ_SUB_PRIORITY;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    /* --- CAN1 SCE 状态变化中断（错误监测）--- */
    NVIC_InitStructure.NVIC_IRQChannel = CAN_SCE_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = CAN_RX_IRQ_PREEMPT_PRIORITY;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = CAN_SCE_IRQ_SUB_PRIORITY;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
}

/**
  * @brief  配置 CAN1 硬件滤波器
  * @note   32 位屏蔽位模式，屏蔽码只校验 IDE(bit2) 与 RTR(bit1)：
  *           IDE = 1 -> 只接收扩展帧
  *           RTR = 0 -> 只接收数据帧（丢弃远程帧）
  *         ID 位全部不参与比较，即放行所有扩展数据帧。
  *         （Linux 端要求做硬件滤波，STM32 端作为协议从站需全收，
  *           具体 ID 合法性判断由 bms_protocol.c 的协议层完成）
  */
static void CAN_Filter_Config(void)
{
    CAN_FilterInitTypeDef CAN_FilterInitStructure;

    CAN_FilterInitStructure.CAN_FilterNumber = 0;
    CAN_FilterInitStructure.CAN_FilterMode   = CAN_FilterMode_IdMask;
    CAN_FilterInitStructure.CAN_FilterScale  = CAN_FilterScale_32bit;

    /* ID 寄存器：EXID = 0, IDE = 1, RTR = 0 */
    CAN_FilterInitStructure.CAN_FilterIdHigh     = 0x0000;
    CAN_FilterInitStructure.CAN_FilterIdLow      = 0x0004;  /* bit2 = IDE */

    /* 屏蔽寄存器：只比较 IDE 与 RTR 两位 */
    CAN_FilterInitStructure.CAN_FilterMaskIdHigh = 0x0000;
    CAN_FilterInitStructure.CAN_FilterMaskIdLow  = 0x0006;  /* bit2|bit1 */

    CAN_FilterInitStructure.CAN_FilterFIFOAssignment = CAN_Filter_FIFO0;
    CAN_FilterInitStructure.CAN_FilterActivation     = ENABLE;
    CAN_FilterInit(&CAN_FilterInitStructure);
}

/*==============================================================================
 *                              对外接口实现
 *============================================================================*/

/**
  * @brief  CAN1 初始化：时钟 / GPIO / 位定时 / 滤波器 / 中断 / 使能
  * @note   必须在 NVIC_PriorityGroupConfig 之后或与本函数内部保持一致
  */
void CAN_Config(void)
{
    CAN_InitTypeDef CAN_InitStructure;

    /* 1. 打开 CAN1 时钟（挂载在 APB1） */
    RCC_APB1PeriphClockCmd(CAN_CLK, ENABLE);

    /* 2. GPIO 初始化 */
    CAN_GPIO_Config();

    /* 3. CAN 单元复位，保证从干净状态开始配置 */
    CAN_DeInit(CANx);
    CAN_StructInit(&CAN_InitStructure);

    /* 4. 位定时配置（250 kbps @ APB1 36MHz） */
    CAN_InitStructure.CAN_TTCM = DISABLE;                     /* 时间触发通信模式关闭 */
    CAN_InitStructure.CAN_ABOM = ENABLE;                      /* 自动离线管理：Bus-Off 后自动恢复 */
    CAN_InitStructure.CAN_AWUM = ENABLE;                      /* 自动唤醒模式 */
    CAN_InitStructure.CAN_NART = DISABLE;                     /* 禁止自动重传 = DISABLE，即允许自动重传 */
    CAN_InitStructure.CAN_RFLM = DISABLE;                     /* FIFO 不锁定，溢出时覆盖旧报文 */
    CAN_InitStructure.CAN_TXFP = ENABLE;                      /* 发送 FIFO 优先级：按请求顺序发送 */
    CAN_InitStructure.CAN_Mode = CAN_Mode_Normal;             /* 正常工作模式 */

    CAN_InitStructure.CAN_SJW  = CAN_BT_SJW;
    CAN_InitStructure.CAN_BS1  = CAN_BT_BS1;
    CAN_InitStructure.CAN_BS2  = CAN_BT_BS2;
    CAN_InitStructure.CAN_Prescaler = CAN_BT_PRESCALER;

    CAN_Init(CANx, &CAN_InitStructure);

    /* 5. 硬件滤波器 */
    CAN_Filter_Config();

    /* 6. 中断配置：FIFO0 消息挂起中断 + 状态变化中断 */
    CAN_ITConfig(CANx, CAN_IT_FMP0, ENABLE);   /* FIFO0 非空中断 */
    CAN_ITConfig(CANx, CAN_IT_ERR,  ENABLE);   /* 错误 / 状态变化中断 */

    CAN_NVIC_Config();

    /* 7. 统计量清零 */
    CAN_ClearStatistics();

    /* 8. 上电自检：把「控制器到底有没有真的挂到总线上」直接打出来
     *
     *    为什么要专门查这一遍
     *    --------------------
     *    标准库的 CAN_Init() 内部有两处「等待 INAK 变化」的循环，**都没有超时**：
     *    如果 CANH/CANL 被短接、收发器没供电、或者总线上一直是显性电平，
     *    控制器看不到 11 个连续隐性位。这时程序在别的地方看起来完全正常
     *    （屏幕照画、串口照打），但它既不发帧也不产生 ACK，
     *    对端只能观察到 Bus-Off —— 这类故障光看现象很难和「接线反了」区分开。
     *
     *    这里同时把 PA11/PA12 的 GPIO 配置读回来：
     *    PA11 必须是浮空输入(0x4)，PA12 必须是复用推挽(0xB)。
     *    如果被别的初始化（例如 LCD / 触摸的引脚探测）改掉了，这里一眼就能看出来。
     */
    {
        uint32_t msr = CANx->MSR;
        uint32_t crh = GPIOA->CRH;
        uint8_t  tec = 0;
        uint8_t  rec = 0;
        uint8_t  lec = 0;

        CAN_GetBusCounters(&tec, &rec, &lec);

        printf("[CAN] 自检: MSR=0x%08X  TEC=%u REC=%u LEC=%u  GPIOA->CRH=0x%08X\r\n",
               (unsigned int)msr, (unsigned int)tec,
               (unsigned int)rec, (unsigned int)lec, (unsigned int)crh);

        /* PA11 的配置位在 CRH[15:12]，PA12 在 CRH[19:16]。
         *
         * PA11(CAN_RX) 正确值是 0x4(浮空输入) 或 0x8(上拉输入) ——
         * 标准库例程普遍用 GPIO_Mode_IPU，也就是 0x8，两者都算正常。
         * （之前这里只认 0x4，把正确的 0x8 报成了"引脚失效"，是误报。）
         * PA12(CAN_TX) 必须是 0xB(复用推挽 50MHz)。 */
        {
            uint8_t pa11 = (uint8_t)((crh >> 12) & 0x0Fu);
            uint8_t pa12 = (uint8_t)((crh >> 16) & 0x0Fu);

            if ((pa11 != 0x4u && pa11 != 0x8u) || pa12 != 0xBu)
            {
                printf("[CAN] 警告: CAN 引脚配置异常 PA11=0x%X PA12=0x%X"
                       "（期望 PA11=0x4 或 0x8，PA12=0xB）\r\n",
                       (unsigned int)pa11, (unsigned int)pa12);
            }
        }

        if ((msr & CAN_MSR_INAK) != 0u)
        {
            printf("[CAN] !! 严重: 控制器仍停在初始化模式(INAK=1)，不会发帧也不会产生 ACK\r\n");
            printf("[CAN] !! 常见原因: 1) CANH/CANL 短路或接反; 2) 收发器未供电/损坏;\r\n");
            printf("[CAN] !!           3) 总线上一直有节点发显性电平(波特率不一致)\r\n");
        }
        else if ((msr & CAN_MSR_SLAK) != 0u)
        {
            printf("[CAN] !! 控制器处于睡眠模式(SLAK=1)\r\n");
        }
        else
        {
            printf("[CAN] 控制器已进入正常模式，具备发送与应答(ACK)能力\r\n");
        }

        if (lec == 0x03u)
        {
            printf("[CAN] 提示: 最近一次错误是 ACK 错误 —— 总线上没有其它节点应答\r\n");
        }
    }
}

/**
  * @brief  发送一帧扩展数据帧
  * @param  ext_id : 29 位扩展帧 ID（不含 IDE 标志）
  * @param  data   : 数据指针，可为 NULL（len 为 0 时）
  * @param  len    : 数据长度 0~8
  * @retval 1 = 成功送入发送邮箱；0 = 失败（无空闲邮箱 / 参数非法）
  */
uint8_t CAN_SendFrame(uint32_t ext_id, const uint8_t *data, uint8_t len)
{
    CanTxMsg TxMessage;
    uint8_t  i;
    uint8_t  mailbox;

    if (len > 8)
    {
        return 0;
    }

    TxMessage.ExtId = ext_id & 0x1FFFFFFF;   /* 29 位扩展 ID */
    TxMessage.IDE   = CAN_Id_Extended;       /* 扩展帧 */
    TxMessage.RTR   = CAN_RTR_Data;          /* 数据帧 */
    TxMessage.DLC   = len;

    for (i = 0; i < len; i++)
    {
        TxMessage.Data[i] = (data != 0) ? data[i] : 0x00;
    }
    for (i = len; i < 8; i++)
    {
        TxMessage.Data[i] = 0x00;            /* 未使用字节清零，便于抓包分析 */
    }

    mailbox = CAN_Transmit(CANx, &TxMessage);

    if (mailbox == CAN_TxStatus_NoMailBox)
    {
        s_tx_fail_cnt++;
        return 0;
    }

    s_tx_frame_cnt++;
    return 1;
}

/**
  * @brief  从软件接收队列取出一帧（非阻塞）
  * @param  frame : 输出参数
  * @retval 1 = 取到数据；0 = 队列为空
  */
uint8_t CAN_GetRxFrame(CAN_Frame_t *frame)
{
    const volatile CAN_Frame_t *src;

    if (frame == 0)
    {
        return 0;
    }

    if (s_rx_head == s_rx_tail)
    {
        return 0;   /* 空队列 */
    }

    /* 逐字段拷贝，不要写成 *frame = (CAN_Frame_t)s_rx_queue[...]。
     * 原因：s_rx_queue 声明为 volatile，而 ARM Compiler 5（Keil MDK）
     * 不允许在结构体类型转换中丢弃 volatile 限定，会直接报
     *     error: #119: cast to type "CAN_Frame_t" is not allowed
     * 逐字段读取既符合 C90，也天然无竞争：生产者只写 head 指向的槽位，
     * 消费者只读 tail 指向的槽位，而 head != tail 时两者永不重叠。 */
    src = &s_rx_queue[s_rx_tail];

    frame->id       = src->id;
    frame->len      = src->len;
    frame->reserved = 0;
    frame->data[0]  = src->data[0];
    frame->data[1]  = src->data[1];
    frame->data[2]  = src->data[2];
    frame->data[3]  = src->data[3];
    frame->data[4]  = src->data[4];
    frame->data[5]  = src->data[5];
    frame->data[6]  = src->data[6];
    frame->data[7]  = src->data[7];

    s_rx_tail = (uint16_t)((s_rx_tail + 1u) & CAN_RX_QUEUE_MASK);
    return 1;
}

/**
  * @brief  查询软件接收队列中待处理帧数
  */
uint16_t CAN_GetRxQueueCount(void)
{
    return (uint16_t)((s_rx_head - s_rx_tail) & CAN_RX_QUEUE_MASK);
}

uint32_t CAN_GetRxFrameCount(void)    { return s_rx_frame_cnt;  }
uint32_t CAN_GetRxOverflowCount(void) { return s_rx_overflow;   }
uint32_t CAN_GetTxFrameCount(void)    { return s_tx_frame_cnt;  }
uint32_t CAN_GetTxFailCount(void)     { return s_tx_fail_cnt;   }
uint32_t CAN_GetErrorCount(void)      { return s_err_cnt;       }
uint8_t  CAN_GetErrorCodeRaw(void)   { return s_last_err_code; }
uint8_t  CAN_IsBusOff(void)           { return s_bus_off_flag;  }

/**
  * @brief  读取 CAN 总线错误计数器与最近错误码
  * @note   CAN_ESR 寄存器布局：
  *           bit 31:24 = REC[7:0]   接收错误计数器
  *           bit 23:16 = TEC[7:0]   发送错误计数器
  *           bit  6:4  = LEC[2:0]   最近错误码
  *           bit  7    = TXERR      TEC 已超过 255
  */
void CAN_GetBusCounters(uint8_t *tec, uint8_t *rec, uint8_t *lec)
{
    uint32_t esr = CANx->ESR;

    if (tec != 0) { *tec = (uint8_t)((esr >> 16) & 0xFFu); }
    if (rec != 0) { *rec = (uint8_t)((esr >> 24) & 0xFFu); }
    if (lec != 0) { *lec = (uint8_t)((esr >> 4)  & 0x07u); }
}

void CAN_ClearStatistics(void)
{
    s_rx_frame_cnt  = 0;
    s_rx_overflow   = 0;
    s_tx_frame_cnt  = 0;
    s_tx_fail_cnt   = 0;
    s_err_cnt       = 0;
    s_last_err_code = 0;
    s_bus_off_flag  = 0;
}

/**
  * @brief  从 Bus-Off 状态恢复：重新初始化 CAN 单元
  * @note   CAN_ABOM = ENABLE 时硬件会自动恢复，本函数作为软件兜底
  */
void CAN_RecoverBusOff(void)
{
    CAN_InitTypeDef CAN_InitStructure;

    CAN_DeInit(CANx);
    CAN_StructInit(&CAN_InitStructure);

    CAN_InitStructure.CAN_TTCM = DISABLE;
    CAN_InitStructure.CAN_ABOM = ENABLE;
    CAN_InitStructure.CAN_AWUM = ENABLE;
    CAN_InitStructure.CAN_NART = DISABLE;
    CAN_InitStructure.CAN_RFLM = DISABLE;
    CAN_InitStructure.CAN_TXFP = ENABLE;
    CAN_InitStructure.CAN_Mode = CAN_Mode_Normal;
    CAN_InitStructure.CAN_SJW  = CAN_BT_SJW;
    CAN_InitStructure.CAN_BS1  = CAN_BT_BS1;
    CAN_InitStructure.CAN_BS2  = CAN_BT_BS2;
    CAN_InitStructure.CAN_Prescaler = CAN_BT_PRESCALER;

    CAN_Init(CANx, &CAN_InitStructure);
    CAN_Filter_Config();

    CAN_ITConfig(CANx, CAN_IT_FMP0, ENABLE);
    CAN_ITConfig(CANx, CAN_IT_ERR,  ENABLE);

    s_bus_off_flag = 0;
}

/*==============================================================================
 *                              中断服务函数
 *============================================================================*/

/**
  * @brief  CAN1 RX0 中断服务函数（向量名 USB_LP_CAN1_RX0_IRQHandler）
  * @note   STM32F103 大容量产品中 CAN1_RX0 与 USB 低优先级中断共用向量 20。
  *         本工程未使用 USB，因此该向量专用于 CAN 接收。
  */
void CAN_RX_IRQHandler(void)
{
    CanRxMsg RxMessage;
    uint16_t next_head;

    /* 一次中断把所有挂起的 FIFO0 报文搬完，避免高负载下频繁进出中断 */
    while (CAN_GetITStatus(CANx, CAN_IT_FMP0) != RESET)
    {
        CAN_Receive(CANx, CAN_FIFO0, &RxMessage);

        s_rx_frame_cnt++;

        /* 只处理扩展帧数据帧 */
        if ((RxMessage.IDE == CAN_Id_Extended) && (RxMessage.RTR == CAN_RTR_Data))
        {
            next_head = (uint16_t)((s_rx_head + 1u) & CAN_RX_QUEUE_MASK);

            if (next_head == s_rx_tail)
            {
                /* 队列满：丢弃最新帧并计数（也可改为覆盖最旧帧，此处选择保旧帧） */
                s_rx_overflow++;
            }
            else
            {
                s_rx_queue[s_rx_head].id  = RxMessage.ExtId & 0x1FFFFFFF;
                s_rx_queue[s_rx_head].len = RxMessage.DLC & 0x0F;
                s_rx_queue[s_rx_head].data[0] = RxMessage.Data[0];
                s_rx_queue[s_rx_head].data[1] = RxMessage.Data[1];
                s_rx_queue[s_rx_head].data[2] = RxMessage.Data[2];
                s_rx_queue[s_rx_head].data[3] = RxMessage.Data[3];
                s_rx_queue[s_rx_head].data[4] = RxMessage.Data[4];
                s_rx_queue[s_rx_head].data[5] = RxMessage.Data[5];
                s_rx_queue[s_rx_head].data[6] = RxMessage.Data[6];
                s_rx_queue[s_rx_head].data[7] = RxMessage.Data[7];

                s_rx_head = next_head;
            }
        }

        /* 清除 FIFO0 释放中断，允许硬件继续写入下一帧 */
        CAN_FIFORelease(CANx, CAN_FIFO0);
    }
}

/**
  * @brief  CAN1 状态变化中断（错误被动 / 错误警告 / Bus-Off / 唤醒）
  */
void CAN_SCE_IRQHandler(void)
{
    if (CAN_GetITStatus(CANx, CAN_IT_ERR) != RESET)
    {
        s_err_cnt++;
        s_last_err_code = (uint8_t)(CANx->ESR & 0xFF);   /* 低 8 位为 LEC 错误码 */

        if (CAN_GetFlagStatus(CANx, CAN_FLAG_BOF) != RESET)
        {
            s_bus_off_flag = 1;   /* 已进入 Bus-Off，主循环负责告警 */
        }

        CAN_ClearITPendingBit(CANx, CAN_IT_ERR);
    }
}

/******************* (C) COPYRIGHT 2025 GB27930 BMS Simulator *****END OF FILE****/
