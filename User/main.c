/**
  ******************************************************************************
  * @file    User/main.c
  * @author  GB27930 BMS Simulator Project
  * @version V1.0.0
  * @date    2025
  * @brief   GB/T 27930-2015 BMS 报文模拟器 —— 主程序
  *
  * 硬件平台：正点原子 STM32F103ZET6 精英板
  *   - CAN1：PA11(CAN_RX) / PA12(CAN_TX)，外接 TJA1050，板载 120Ω 终端电阻
  *   - 总线：250 kbps，29 位扩展帧（GB/T 27930-2015 规定）
  *   - USART1：PA9/PA10，115200-8-N-1，用于调试日志
  *   - LED：红灯 PB5（故障指示），绿灯 PE5（充电中指示）
  *
  * 程序结构
  * --------
  *   初始化阶段：
  *     ① USART_Config()      串口调试输出（printf 重定向到 USART1）
  *     ② LED_GPIO_Config()   状态指示灯
  *     ③ CAN_Config()        CAN1 位定时/滤波器/接收中断
  *     ④ BMS_Protocol_Init() 协议层 + SysTick 1ms 时基 + 电池模型复位
  *     ⑤ UI_Init()           本地界面（ILI9488 480x320 + 电容触摸）
  *
  *   主循环（无操作系统，前后台架构）：
  *     前台：CAN1_RX0 中断把报文塞进 can.c 的软件环形队列
  *     后台：
  *       a) 排空接收队列并逐帧交给协议层处理（状态机迁移 + J1939 组包）
  *       b) BMS_Protocol_Tick() 推进状态机、周期发送、多帧流控、电池仿真
  *       c) BMS_Protocol_PrintStatus() 每秒打印一次运行摘要
  *       d) UI_Tick() 触摸扫描与界面刷新（主界面 2Hz / 曲线 1Hz）
  *
  *   主循环无任何阻塞延时，因此 CAN 报文处理延迟在微秒级；
  *   所有时间判定都基于 SysTick 毫秒计数，与主循环执行频率无关。
  *
  * 编译说明（Keil MDK5）
  * --------------------
  *   工程文件：Project/yehuoF103.uvprojx
  *   器件选择：STM32F103ZE
  *   预处理宏：USE_STDPERIPH_DRIVER, STM32F10X_HD
  *   头文件路径：..\Libraries\CMSIS; ..\Libraries\STM32F10x_StdPeriph_Driver\inc;
  *              ..\User; ..\User\Led; ..\User\Usart; ..\User\Can;
  *              ..\User\Bms; ..\User\Ui
  *   必须勾选：Target -> Use MicroLIB（否则 printf 无法重定向到串口）
  *   直接 Build（F7）即可，无需任何手工配置。
  ******************************************************************************
  */

#include "stm32f10x.h"
#include "bsp_led.h"
#include "bsp_usart.h"
#include "can.h"
#include "bms_protocol.h"
#include "ui_app.h"

/* 一次主循环最多处理的接收帧数：
 * 限幅的目的是保证在极端高负载下 BMS_Protocol_Tick() 仍能及时被调用，
 * 从而不影响周期报文的发送精度。 */
#define MAIN_RX_BATCH_MAX    16u

int main(void)
{
    CAN_Frame_t frame;
    uint8_t     processed;

    /*------------------------------------------------------------------
     * ① 串口调试输出（USART1, 115200, 8N1）
     *    内部同时完成 NVIC 优先级分组设置（NVIC_PriorityGroup_2）
     *----------------------------------------------------------------*/
    USART_Config();

    printf("\r\n\r\n");
    printf("[BOOT] STM32F103ZET6 GB/T 27930 BMS 报文模拟器启动...\r\n");
    printf("[BOOT] HSE = 8 MHz, SYSCLK = 72 MHz, APB1 = 36 MHz\r\n");
    printf("[BOOT] CAN1 = PA11/PA12, 波特率 %s, 扩展帧模式\r\n", CAN_BAUDRATE_TEXT);

    /*------------------------------------------------------------------
     * ② LED 状态指示
     *----------------------------------------------------------------*/
    LED_GPIO_Config();
    LED_R(OFF);
    LED_G(OFF);
    printf("[BOOT] LED 初始化完成 (红=PB5 故障 / 绿=PE5 充电中)\r\n");

    /*------------------------------------------------------------------
     * ③ CAN1 初始化
     *----------------------------------------------------------------*/
    CAN_Config();
    printf("[BOOT] CAN1 初始化完成，进入正常模式并等待总线同步...\r\n");

    /*------------------------------------------------------------------
     * ④ 协议层初始化（含 SysTick 1 ms 时基与电池模型）
     *----------------------------------------------------------------*/
    BMS_Protocol_Init();

    /*------------------------------------------------------------------
     * ⑤ 本地图形界面（3.5 寸 ILI9488 480x320 + 电容触摸）
     *    内部会完成 LCD_Init()/TP_Init() 并画好首帧。
     *    放在最后初始化：LCD 初始化约 100~300 ms，先让 CAN 与协议层
     *    就绪，避免上电瞬间错过充电机的握手报文。
     *----------------------------------------------------------------*/
    UI_Init();
    printf("[BOOT] 本地界面初始化完成（LCD 480x320 + 触摸）\r\n");


    printf("[BOOT] 初始化完毕，等待充电机（I.MX6ULL 监控终端）发送 CHM 握手报文\r\n\r\n");

    /*------------------------------------------------------------------
     * 主循环
     *----------------------------------------------------------------*/
    while (1)
    {
        /* ---- a) 排空 CAN 接收队列（限幅 16 帧/轮，保证 Tick 实时性） ---- */
        processed = 0;
        while (processed < MAIN_RX_BATCH_MAX && CAN_GetRxFrame(&frame) != 0)
        {
            BMS_Protocol_OnFrame(&frame);
            processed++;
        }

        /* ---- b) 协议层周期任务：状态机 + 周期发送 + 多帧流控 + 仿真 ---- */
        BMS_Protocol_Tick();

        /* ---- d) 本地界面：触摸扫描 + 数据刷新（内部自带节奏控制） ----
         *      不会阻塞：最坏情况是一次整屏重绘（约 60~120 ms），
         *      而它每 500 ms 才发生一次，对 CAN 实时链路无影响。 */
        UI_Tick();


        /* ---- c) 每秒打印一次运行摘要 ---- */
        BMS_Protocol_PrintStatus();
    }
}

/******************* (C) COPYRIGHT 2025 GB27930 BMS Simulator *****END OF FILE***/
