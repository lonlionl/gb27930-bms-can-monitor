/**
  ******************************************************************************
  * @file    User/stm32f10x_it.c
  * @author  MCD Application Team / GB27930 BMS Simulator Project
  * @version V3.5.0
  * @date    2025
  * @brief   中断服务函数集中定义文件
  *
  * 本工程实际使用到的中断：
  *   ① SysTick_Handler         —— 1 ms 系统节拍，为 GB/T 27930 协议层提供时基
  *   ② USB_LP_CAN1_RX0_IRQHandler —— CAN1 FIFO0 接收中断（定义在 Can/can.c 中）
  *   ③ CAN1_SCE_IRQHandler     —— CAN1 状态变化/错误中断（定义在 Can/can.c 中）
  *   ④ USART1_IRQHandler       —— 串口接收中断（本工程未使用接收，保持空实现）
  *
  * 注意：STM32F103 大容量产品中 CAN1_RX0 与 USB 低优先级中断共用中断向量 20，
  *       向量名为 USB_LP_CAN1_RX0_IRQHandler，本工程未使用 USB 外设。
  ******************************************************************************
  * @attention
  *
  * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
  * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
  * TIME. AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY
  * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
  * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
  * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
  *
  * <h2><center>&copy; COPYRIGHT 2011 STMicroelectronics</center></h2>
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "stm32f10x_it.h"
#include "bms_protocol.h"
#include "ui_app.h"

/** @addtogroup STM32F10x_StdPeriph_Template
  * @{
  */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/

/******************************************************************************/
/*            Cortex-M3 Processor Exceptions Handlers                         */
/******************************************************************************/

/**
  * @brief  This function handles NMI exception.
  */
void NMI_Handler(void)
{
}

/**
  * @brief  This function handles Hard Fault exception.
  */
void HardFault_Handler(void)
{
  /* 进入死循环，便于用调试器查看寄存器定位问题 */
  while (1)
  {
  }
}

/**
  * @brief  This function handles Memory Manage exception.
  */
void MemManage_Handler(void)
{
  while (1)
  {
  }
}

/**
  * @brief  This function handles Bus Fault exception.
  */
void BusFault_Handler(void)
{
  while (1)
  {
  }
}

/**
  * @brief  This function handles Usage Fault exception.
  */
void UsageFault_Handler(void)
{
  while (1)
  {
  }
}

/**
  * @brief  This function handles SVCall exception.
  */
void SVC_Handler(void)
{
}

/**
  * @brief  This function handles Debug Monitor exception.
  */
void DebugMon_Handler(void)
{
}

/**
  * @brief  This function handles PendSVC exception.
  */
void PendSV_Handler(void)
{
}

/**
  * @brief  SysTick 中断服务函数 —— 1 ms 系统节拍
  * @note   由 BMS_Protocol_Init() 中的 SysTick_Config(SystemCoreClock / 1000)
  *         配置为每 1 ms 触发一次；BMS_SysTick_Handler() 内部只做一次
  *         32 位自增，无任何耗时操作，因此不需要在中断中清标志位
  *         （SysTick 的 COUNTFLAG 由内核在读取时自动清除）。
  */
void SysTick_Handler(void)
{
  BMS_SysTick_Handler();
  /* 界面毫秒时基：只做一次自增，不在这里绘图。
   * 绘图全部放在主循环的 UI_Tick() 里，中断里绝不碰 LCD。 */
  UI_Tick_1ms();
}

/******************************************************************************/
/*                 STM32F10x Peripherals Interrupt Handlers                   */
/*                                                                            */
/*  说明：CAN1 的两个中断服务函数（USB_LP_CAN1_RX0_IRQHandler /              */
/*        CAN1_SCE_IRQHandler）定义在 Can/can.c 中，原因是它们需要直接访问   */
/*        驱动内部的软件接收队列与统计变量。启动文件中的同名符号为 WEAK，     */
/*        链接时会自动采用 can.c 中的强符号，不会有重复定义冲突。             */
/******************************************************************************/

/**
  * @}
  */

/******************* (C) COPYRIGHT 2011 STMicroelectronics *****END OF FILE****/
