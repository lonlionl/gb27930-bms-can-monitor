# STM32 端 BMS 报文模拟器（GB/T 27930-2015）

本文档描述 `Project/yehuoF103.uvprojx` 这个 Keil 工程本身：它做什么、怎么编译烧录、屏幕上显示什么、协议与电池模型按什么规则运行。所有描述以 `User/` 下的当前源码为准。

---

## 一、工程功能

STM32F103ZET6 在本项目里扮演的是**电动汽车上的 BMS（电池管理系统）**：它按照 GB/T 27930-2015 的规定，通过 250 kbps、29 位扩展帧的 CAN 总线与另一块 I.MX6ULL 开发板（充电机侧监控终端）通信，完整走完握手、辨识、参数配置、充电准备、充电、结束六个阶段，并在这条总线上周期上报自己"这块电池"的电压、电流、SOC、单体电压、温度。

整套系统里两端的位置：

```
  I.MX6ULL（充电机 + 监控终端）                STM32F103ZET6（本工程，BMS）
  ┌────────────────────────────┐              ┌────────────────────────────┐
  │ 主界面 / 数据曲线 / 历史   │              │ 3.5" ILI9488 只读界面      │
  │ 「充电」「停止」按钮       │              │ 状态机 + J1939 多帧收发    │
  │ 报文解析、SQLite 存档      │              │ 电池模型（SOC/电压/温度）  │
  └──────────┬─────────────────┘              └──────────┬─────────────────┘
             │  CANH / CANL, 250 kbps, 扩展帧              │
             └─────────────────────────────────────────────┘
```

具体分工：

- STM32 不主动发起充电。它在空闲态只做两件事：周期广播电池状态（BSM，250 ms 一帧），每秒发一帧心跳。收到 I.MX 发来的 CHM 握手报文才开始一次充电会话。
- STM32 是协议从站：所有周期报文由它的状态机和 1 ms 时基驱动，接收侧不做 ID 过滤，由协议层按 29 位 ID 精确匹配。
- STM32 的本地屏幕只显示，不接受任何输入：无触摸、无曲线界面、无历史采样缓冲（见第五节）。
- 电量（SOC）与充电阶段存在 STM32 的 Flash 最后一页，跨会话、跨断电保持（见第八节）。

---

## 二、硬件要求

| 项目 | 要求 |
|------|------|
| 主控板 | 正点原子 STM32F103ZET6 精英板（STM32F103ZET6，512 KB Flash / 64 KB SRAM，HSE 8 MHz，SYSCLK 72 MHz，APB1 36 MHz） |
| CAN | 板载 CAN 收发器（TJA1050 一类）。CAN1 使用 PA11(CAN_RX) / PA12(CAN_TX)，为默认引脚，未做 AFIO 重映射 |
| 终端电阻 | CANH 与 CANL 之间需要 120 Ω。断开两端电源后量 CANH–CANL 应约为 60 Ω（两端各 120 Ω 并联） |
| 显示屏 | 3.5 寸 ILI9488，480×320 横屏，16 位色（RGB565），走 FSMC，插在板载 TFTLCD 排针上 |
| 调试串口 | USART1，PA9(TX) / PA10(RX)，115200-8-N-1 |
| 指示灯 | 红灯 PB5（故障）、绿灯 PE5（充电中）。两颗灯都是低电平点亮 |

### CAN 接线

```
   STM32 精英板                       I.MX6ULL 开发板
     CANH  ───────────────────────────  CANH
     CANL  ───────────────────────────  CANL
     GND   ───────────────────────────  GND     （必须共地）
```

同名相连，不要交叉。CANH/CANL 接反时差分极性相反，两端都会既不收也不应答：STM32 侧表现为 `TEC` 涨到 128、`LEC=3(ACK错误)`、`REC=0`，I.MX 侧表现为 `bus-off` 且接收帧数为 0。

CAN 与 LCD 不冲突：LCD 走 FSMC 数据/地址总线，CAN 走 PA11/PA12，插着屏也能正常通信。

屏上的触摸排线（精英板 V2 为 PB1/PF9/PF10/PF11 一类）在本工程里没有使用。板载屏的触摸芯片在这块板子上探测不到，STM32 侧也不需要人工输入，触摸驱动不在界面调用链上（源码仍在工程里并参与编译，见第三节）。

---

## 三、目录结构

`User/` 下按功能分成六个模块，全部由工程文件 `Project/yehuoF103.uvprojx` 编译：

```
User/
├── main.c                 主程序：初始化顺序 + 前后台主循环
├── stm32f10x_it.c         中断向量：SysTick_Handler（挂 1 ms 时基）
├── stm32f10x_conf.h       标准外设库裁剪配置
│
├── Bms/                   协议层与状态机（本工程的核心）
│   ├── bms_protocol.h     报文 PGN / CAN ID / 数据域布局、状态机枚举、
│   │                      模拟电池参数、UI 显示快照结构体
│   └── bms_protocol.c     报文组包、J1939 TP 发送状态机、BMS 状态机、
│                          电池模型仿真、Flash 掉电记忆、串口日志
│
├── Can/                   CAN 底层驱动
│   ├── can.h              波特率宏表、CAN_Frame_t、统计与恢复接口
│   └── can.c              位定时 250 kbps、硬件滤波、RX0+SCE 中断、
│                          64 帧软件环形队列、Bus-Off 恢复、上电自检
│
├── Lcd/                   LCD / 触摸驱动
│   ├── lcd.h / lcd.c      FSMC 总线访问与绘图原语，480×320 横屏
│   ├── ili93xx.c          ILI9488 初始化序列与读 ID（0xD3）
│   ├── ctiic.h / .c       触摸屏专用软件 I2C（引脚运行时可切换）
│   └── touch.h / touch.c  电容触摸（GT9147 / GT1151 / FT5206 自动识别）
│
├── Ui/                    本地界面
│   ├── ui_port.h / .c     适配层：唯一直接调用 LCD 驱动的文件，对外只暴露 uip_* 接口
│   ├── ui_app.h / .c      应用层：布局、格式化、静态层/动态层局部重绘
│   └── font16.h           16×16 点阵字库，由 tools/gen_font.py 生成
│
├── Usart/                 串口
│   ├── bsp_usart.h        USART1 宏定义（115200-8-N-1）
│   └── bsp_usart.c        GPIO/时钟/中断配置、fputc 重定向（printf 到串口）
│
└── Led/                   指示灯
    ├── bsp_led.h          PB5/PE5 引脚宏，LED_R()/LED_G()
    └── bsp_led.c          推挽输出初始化
```

模块之间的依赖是单向的：

- `Lcd/` 不认识 `Ui/`，`Ui/ui_port.c` 是唯一把两者接起来的地方。换 LCD 驱动版本只改 `User/Ui/ui_port.c` 顶部那块"驱动适配块"（`UIP_LCD_WIDTH()` / `UIP_LCD_HEIGHT()` / `UIP_LCD_FILL()` 三个宏）。
- `Ui/` 不认识 `Bms/` 的内部变量。界面通过 `BMS_Protocol_GetUiSnapshot()` 一次性取回一份纯数据快照，界面怎么读都不会影响状态机。
- `Bms/` 通过 `Can/` 的 `CAN_SendFrame()` / `CAN_GetRxFrame()` 收发，不直接碰寄存器。

`User/Lcd/` 下的 `touch.c` / `ctiic.c` 仍在工程文件里、仍会被编译，但没有任何调用点。链接器会把它们逐个函数、逐个段剥掉：`Project/Listings/yehuoF103.map` 里列着 48 条 `Removing touch.o(...)` / `Removing ctiic.o(...)` 记录（含 `Removing touch.o(i.tp_probe_pins), (180 bytes).`、`Removing touch.o(.conststring), (264 bytes).`），镜像里这两个目标文件的占用是 0 字节。

---

## 四、Keil5 编译与烧录

### 工程与配置

工程文件：`Project\yehuoF103.uvprojx`。用 Keil uVision5 打开后直接 Build（F7）即可，以下配置已经预置在工程里：

| 位置 | 设置 |
|------|------|
| Target 页 | 器件 `STM32F103ZE`，Xtal 8.0 MHz，勾选 `Use MicroLIB` |
| C/C++ 页 → Define | `USE_STDPERIPH_DRIVER, STM32F10X_HD` |
| C/C++ 页 → Include Paths | `..\Libraries\CMSIS; ..\Libraries\STM32F10x_StdPeriph_Driver\inc; ..\User; ..\User\Led; ..\User\Usart; ..\User\Can; ..\User\Bms; ..\User\Ui; ..\User\Lcd` |
| C/C++ 页 → Optimization | Level 2（工程文件里 `<Optim>2</Optim>`） |
| Output 页 | `Create HEX File` 勾选，输出到 `Project\Objects\` |
| 工具链 | ARM Compiler 5（Armcc V5.06 update 7），`<uAC6>0</uAC6>` |

`Use MicroLIB` 必须勾选：`bsp_usart.c` 里的 `fputc()` 重定向依赖它，不勾选时 printf 不会输出到串口。

### 体积

MDK-Lite 免费版对镜像有 32 KB 硬上限，超了链接器直接报：

```
Error: L6047U: The size of this image (xxxxx bytes) exceeds the maximum allowed
```

本工程实测固件体积（Armcc V5.06 update 7，工程优化级别 `<Optim>2</Optim>`，即 **-O2**）：

```
Program Size: Code=21264  RO-data=5908  RW-data=236  ZI-data=2572
".\Objects\yehuoF103.axf" - 0 Error(s), 0 Warning(s).
```

`Total RO Size` / `Total RW Size` / `Total ROM Size` 三行不在 Keil 的 Build Output 窗口里：Build Output 只有 `Program Size` 一行，这三行由链接器写进 map 文件。本工程的 map 文件是 `Project/Listings/yehuoF103.map`，三行位于 "Image component sizes" 段末尾：

```
        Total RO  Size (Code + RO Data)                27172 (  26.54kB)
        Total RW  Size (RW Data + ZI Data)              2808 (   2.74kB)
        Total ROM Size (Code + RO Data + RW Data)      27408 (  26.77kB)
```

小写 `kB` 是 Keil 的写法，按 1024 换算：`27408 ( 26.77kB)` 即 27408 ÷ 1024 = 26.77。

**体积公式**（口径：**本项目统一按 1024 换算**）：

```
Total ROM = Code + RO-data + RW-data    ← 烧进 Flash 的总量（RW 的初值也要占 Flash，
                                            运行时再被拷到 RAM）
Total RAM = RW-data + ZI-data            ← 需要保留的 RAM 总量

Total ROM = 21264 + 5908 + 236 = 27408 字节 = 26.77 KB
Total RAM =            236 + 2572 =  2808 字节 =  2.74 KB
```

即 Flash 占 **26.77 KB**（27408 ÷ 1024 = 26.77），距 MDK-Lite 的 32 KB 上限还有 **5360 字节 = 5.23 KB**；RAM 占 **2.74 KB**。

各模块的 Flash / RAM 占用（取自 `Project/Listings/yehuoF103.map` 的 "Image component sizes" 表）。**列名与顺序照抄 map**：map 的表头是 `Code (inc. data)`、`RO Data`、`RW Data`、`ZI Data`、`Debug`，**其中第 2 列 `(inc. data)` 是第 1 列 `Code` 的"其中"部分**（编译器放进 `.text` 的小常量与字面量池），**它不是 `RO-data`**。所以下表把这一列单独列出来：算体积时只用 `Code` / `RO-data` / `RW-data` / `ZI-data` 四列相加，`(inc. data)` 只是明细、不参与求和：

| 目标文件 | Code (inc. data) | *其中* (inc. data) | RO-data | RW-data | ZI-data | 说明 |
|----------|-----------------|--------------------|---------|---------|---------|------|
| `bms_protocol.o` | 8258 | 2946 | 701 | 124 | 88 | 协议组包、状态机、电池模型、printf 字符串（RO-data 里大部分是 `BMS_LOG` 格式串） |
| `ui_app.o` | 3910 | 468 | 0 | 36 | 432 | 界面布局与格式化 |
| `ui_port.o` | 1100 | 64 | **4292** | 4 | 0 | `font16.h` 的点阵与 GBK 映射表 —— **4292 字节在 `RO-data` 列**：它是 `const` 数组，链接器放进只读段 `.constdata`（map 里写作 `0x08005440 … Data RO … .constdata ui_port.o`，是 Flash 地址），**只吃 Flash、不吃 RAM**；这个目标文件真正的 RAM 只有 `.data` 的 4 字节（`s_w` / `s_h`） |
| `can.o` | 1338 | 224 | 430 | 28 | 1024 | 驱动本体 + 上电自检字符串 |
| `stm32f10x_can.o` | 1350 | 22 | 0 | 0 | 0 | 标准外设库 |
| `lcd.o` + `ili93xx.o` | 976 + 324 | 284 + 8 | 70 + 0 | 0 + 0 | 0 + 0 | LCD 驱动 |
| `printf8.o`（MicroLIB） | 1142 | 54 | 0 | 0 | 0 | printf 家族（在下面的库合计里） |
| *(库合计)* `Library Totals` | 1522 | 70 | 0 | 4 | 0 | MicroLIB 里被引用到的那些 |
| **`Grand Totals`** | **21264** | **4612** | **5908** | **236** | **2572** | 与 `Program Size` 行逐项一致 |

> 上表的 `Grand Totals` 一行等于 Build Output 里的 `Program Size: Code=21264 RO-data=5908 RW-data=236 ZI-data=2572`，可用来核对 map 与 Build Output 是否来自同一次构建。
>
> `ui_port.o` 的 4292 字节字库在 map 的 `RO-data` 列（`.constdata` 段，Flash 地址），`RW-data` 只有 4 字节：**字库只占 Flash、不占 RAM**（RAM 的 2.74 KB = RW-data 236 + ZI-data 2572，与字库无关）。
>
> `Code (inc. data)` 各目标文件行与库成员行**逐行**相加是 21228 字节，与 `Grand Totals` 的 21264 差 36 字节，这 36 字节就是 map 里 `(incl. Padding)` 的两行（目标文件 30 + 库 6）。用 `Object Totals` + `Library Totals` 两行相加则恰好是 21264。

`User/Ui/font16.h` 里的只读数据构成：

| 数据块 | 组成 | 字节数 |
|--------|------|--------|
| `g_font_data[]` | ASCII 95 × 16 + 汉字 77 × 32 | 1520 + 2464 = 3984 |
| `g_font_gbk_map[]` | GBK 索引，4 字节/条 × 77 | 308 |
| **合计** | | **4292** |

`FONT_CJK_COUNT` 为 **77u**，这 4292 字节落在 `ui_port.o` 的 `RO-data` 列（Flash），不占 RAM。

要压体积，优先级从高到低是：改 `User/Ui/ui_port.h` 的 `UI_ENABLE` 为 0（整个界面不参与编译 —— 按 `Project/Listings/yehuoF103.map`，`ui_port.o` + `ui_app.o` + `lcd.o` + `ili93xx.o` 四个目标文件合计 `5396 + 3946 + 1046 + 324` = **10712 字节 = 10.46 KB** Flash，另省 **472 字节** RAM）、裁剪字库（见第十节）、提高优化等级。

**三处编译口径**（引用 "0 warning" 这类结论时，编译器版本与开关要一并写出）：**Keil 端**（Armcc V5.06 update 7，`<Optim>2</Optim>` 即 `-O2`）为 **`0 Error / 0 Warning`**；**Linux 虚拟机端**（Ubuntu 18.04 + gcc 7.5.0，`-Wall -Wextra -Werror`）为 **0 error / 0 warning**；**板子 Debian 10 端**的较新 gcc 会额外报 **2 个 `-Wformat-truncation`**（`ui.c:401`、`gui.c:313`，属既有代码）。固件体积与告警数一律以 Keil 的 `Rebuild` 输出为准。

### 烧录

1. 连接仿真器（ST-Link 或 CMSIS-DAP），`Options for Target` → `Debug` 页选中对应调试器，勾选 `Reset and Run`。
2. `Flash` → `Download`（F8）。
3. 用 USB-TTL 接 PA9/PA10 打开串口助手，115200-8-N-1，按复位键看启动日志（见第九节）。

改了 `font16.h`、`bms_protocol.h` 这类头文件后要用 `Rebuild`（重新编译全部）而不是 `Build`，否则可能出现头文件已改、目标文件没重编的情况。

---

## 五、运行时的界面

界面只读。屏幕按 480×320 设计；若实际屏更宽或更高，`UI_Init()` 会把整体居中（`lx()` / `ly()` 两个偏移函数）。

刷新节奏：`UI_Tick()` 每 500 ms 取一次快照并重绘。绘制分两层——静态层（底色、标题栏、两个面板、所有固定标签与单位）只在首次绘制；动态层里每个数值各自带一块矩形和一份"上次内容"备忘（颜色也进比较键），内容与颜色都没变就一个像素都不碰。稳态下每帧只改几百个像素。

### 屏幕内容

```
┌ 标题栏 ─────────────────────────────────────────────────────────────┐
│ 国标充电监控终端                                                    │
├─────────────────────────────────────────────────────────────────────┤
│ 总电压      480.0  V        [████████████░░░░░░░░░░]                │
│ 总电流        0.0  A        [░░░░░░░░░░░░░░░░░░░░░░]                │
│ 荷电状态     45.0  %        [█████████░░░░░░░░░░░░░]                │
│ ┌ 信息面板 ───────────────────────────────────────────────────────┐ │
│ │ 最高单体 #65      温度  25 / 19 C                               │ │
│ │ 单体电压 3.000 V  允许 584.0 V 100.0 A                          │ │
│ │ 累计电量 0.0 kWh  时长 00:00:00                                 │ │
│ │ 会话 #0   模式 恒流   充电机 未就绪                             │ │
│ ├  CAN 面板 ─────────────────────────────────────────────────────┤ │
│ │ CAN 250 kbps                                                    │ │
│ │ 接收 0 帧        发送 0 帧                                      │ │
│ │ 状态 待机        异常 无                                        │ │
│ │            只读显示，曲线见 i.MX 端                             │ │
│ └─────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────┘
```

逐项说明：

| 位置 | 内容 | 数据来源 / 规则 |
|------|------|-----------------|
| 标题栏 | `国标充电监控终端`（青色），高 35 像素 | 固定文案，无按钮 |
| 总电压 | 32 像素大字，格式 `480.0`，右下角单位 `V` | 快照 `voltage_x10`。颜色：达到 584.0 V 变红，达到 95% 变黄，否则蓝 |
| 总电压进度条 | 量程 0 ~ 584.0 V | `voltage_x10 / limit_v_x10`，变化小于 8‰ 不重画 |
| 总电流 | 32 像素大字，格式 `0.0`，单位 `A` | 快照 `current_x10`。颜色：达到 100.0 A 变红，达到 95% 变黄，否则青 |
| 总电流进度条 | 量程 0 ~ 100.0 A | `current_x10 / limit_i_x10` |
| 荷电状态 | 32 像素大字，格式 `45.0`，单位 `%` | 快照 `soc_x10`。SOC ≥ 90.0% 显示黄色，否则绿色 |
| 荷电状态进度条 | 量程 0 ~ 100.0%（SOC 本身即千分比） | `soc_x10` |
| 最高单体 # | 两位十进制，如 `65` | 编号随 SOC 漂移，算法见第七节 |
| 温度 | `25 / 19`，前者橙色（最高温度），后者青色（最低温度），单位 `C` | 协议里温度带 −50 ℃ 偏移，屏幕上显示的是加回偏移后的摄氏度 |
| 单体电压 | `3.000`，三位小数，绿色 | `voltage_x10 × 10 / 160`，单位 V |
| 允许 | `584.0 V` 与 `100.0 A` | 最高允许充电总电压 / 最高允许充电电流，固定值 |
| 累计电量 | `0.0`，单位 `kWh`，黄色 | 本次会话累计充电电量，P = U×I 按秒累加 |
| 时长 | `HH:MM:SS`，超过 99 小时按 99 截断 | 本次会话累计充电秒数 |
| 会话 # | 十进制，从 1 开始，每次收到 CHM 加一 | 快照 `session_id` |
| 模式 | `恒压` 或 `恒流` | 快照 `charge_mode`，由 `BMS_CV_SOC_X10`（950，即 SOC ≥ 95.0%）决定，与 BCL 报文的模式字节同一个门限 |
| 充电机 | `就绪`（绿）或 `未就绪`（灰） | 最近一次收到的 CRO 是否为 0xAA |
| CAN 250 kbps | 青色小字，总线参数说明 | 固定文案 |
| 接收 / 发送 | 各最多 5 位十进制，超过 99999 停在 99999 | 快照 `rx_count` / `tx_count`，只统计充电会话里的报文（见第九节） |
| 状态 | `待机` / `充电中` / `充满` / `放电中` | 这是**充电阶段** `s_phase`，不是协议状态。颜色依次为灰 / 绿 / 青 / 黄 |
| 异常 | `BMS_ErrorStr()` 的结果，无错时灰色，有错时红色 | 字库已覆盖它的全部 8 条文案，见第十节 |
| 底部提示 | `只读显示，曲线见 i.MX 端`（居中，灰色） | 固定文案 |

屏幕上的"状态"一行显示的是充电阶段（`s_phase`），不是协议状态机状态。协议状态（空闲 / 握手 / 辨识 / 参数配置 / 充电准备 / 充电中 / 结束 / 故障）只往串口日志打。

屏幕上还有第二个 SOC 门限：SOC ≥ 90.0% 时荷电状态那个数值转黄色。它是纯显示装饰，不参与任何协议判断，因此留在 `ui_app.c` 里当字面量（`s->soc_x10 >= 900`），没有并进 `BMS_CV_SOC_X10`（`Ui/` 不引用 `Bms/` 的内部宏）。在 90.0%~94.9% 这一段，屏幕上 SOC 已变黄而模式一行仍显示"恒流"：两个门限各自独立，互不影响。

### 触摸

没有。`User/Ui/ui_port.c` 里没有 `UIP_TOUCH_*` 系列宏，也不 `#include "touch.h"`，只有两个恒返回 0 的兼容桩：

```c
uint8_t uip_touch_read(uint16_t *x, uint16_t *y) { return 0u; }   /* 永远"没有触摸" */
uint8_t uip_touch_ok(void)                        { return 0u; }   /* 永远"触摸不可用" */
```

`ui_app.c` 里同样没有触摸扫描、没有界面切换、没有历史采样缓冲，`UI_IsCurveScreen()` 恒返回 0，`UI_ClearHistory()` 是空函数。标题栏不画按钮。

`UI_Init()` 会打印一行界面版本用于确认烧录的固件：

```
[UI ] 界面版本: 2025-UI-READONLY (只读显示，无触摸、无曲线)
[UI ] UI ready: 480x320
```

---

## 六、协议实现要点

物理层：CAN 2.0B，250 kbps，29 位扩展帧。位定时 = 预分频 16、BS1 6 Tq、BS2 2 Tq、SJW 1 Tq，共 9 Tq，采样点 77.8%。

29 位 ID 结构：`优先(3) | DP(1) | R(1) | PF(8) | PS(8) | SA(8)`。GB/T 27930 中优先级统一为 6，所有报文 `PF < 240`（PDU1），所以 PS 字段就是目标地址。充电机地址 0x56，BMS 地址 0xF4。

### 实现的报文

BMS 发送：

| 报文 | PGN | 29 位 CAN ID | 周期 | 长度 | 实现位置 |
|------|-----|--------------|------|------|----------|
| BHM | 0x2700 | `0x182756F4` | 握手态 250 ms | **2** | 收到 CHM 立即回一帧，之后握手态周期重发。数据域 B1-B2 = 最高允许充电总电压（0.1 V/位，小端） |
| BRM | 0x0200 | TP 多帧 | 辨识态一次 | 41 | `Build_BRM()`，6 包 |
| BCP | 0x0600 | TP 多帧 | 参数配置态一次 | 13 | `Build_BCP()`，2 包。13 字节字段：单体最高允许电压(0.01V)、最高允许电流(0.1A,−400A)、标称总能量(0.1kWh)、最高允许总压(0.1V)、最高允许温度(1℃,−50℃)、整车 SOC(**0.1 %/位**)、当前电池电压(0.1V) |
| BRO | 0x0900 | `0x100956F4` | 充电准备态 250 ms | 1 | 数据域 B1 = 0xAA |
| BCL | 0x1000 | `0x181056F4` | 充电态 50 ms | **5** | B1-B2 电压需求、B3-B4 电流需求（0.1 A，−400 A 偏移）、B5 充电模式（0x01 恒压 / 0x02 恒流，按 `BMS_CV_SOC_X10` 即 SOC ≥ 95.0% 切恒压）。**标准 BCL 只有这 5 字节**，没有"允许充电电压/电流"（那两个量在 BCP） |
| BCS | 0x1100 | `0x1C1156F4` | 充电态 250 ms | 8（标准 9） | 实测电压/电流、最高单体电压**及组号**（1-12 位电压、13-16 位组号）、SOC（1 %/位）、剩余充电时间。**标准是 9 字节、超 CAN 单帧上限，当前按 8 字节发，属已知偏差** |
| BSM | 0x1300 | `0x181356F4` | 250 ms，不分状态 | **7** | 见下 |
| BST | 0x1900 | `0x101956F4` | 结束态 **10 ms** × 3 次 | 4 | B1 = 中止原因 |
| BSD | 0x1C00 | `0x181C56F4` | 结束态一次（进入 500 ms 后） | **7** | 中止统计 |
| TP.CM | 0xEC00 | `0x1CEC56F4` | 按需 | 8 | RTS |
| TP.DT | 0xEB00 | `0x1CEB56F4` | 按需 | 8 | 数据包 |
| 心跳 | — | `0x18FFF4F4` | 空闲态 1000 ms | 8 | 非国标 ID，用于物理层排查 |

表中的周期与 GB/T 27930-2015 一致：BCL 50 ms（`GB_T_BCL_PERIOD` = `50u`）、BCS / BSM 各 250 ms（`GB_T_BCS_PERIOD` = `250u`、`GB_T_BSM_PERIOD` = `250u`）、**BST 10 ms**（`GB_T_BST_PERIOD` = `10u`，标准表 5）。充电机侧的 **CTS 是 500 ms**（标准表 4，i.MX 端 `T_CTS_PERIOD_MS` = `500u`）。周期报文频次一律以本表为准。

有意保留的四处扩展（设计选择，不是实现疏漏）：

| 扩展 | 与国标的差异 | 为什么保留 |
|------|--------------|------------|
| BSM 不分状态广播 | 国标只在充电阶段发 BSM，本工程待机 / 充满 / 掉电阶段也一直发 | i.MX 相当于车机上的电池监控软件，非充电状态同样要看到实时电量。板上实测：待机时 BSM 4.0 Hz（60 秒 240 帧），加 1 Hz 心跳共 5 帧/秒、总线负载 0.30 %；且待机帧不入库（实测数据库原始帧 0），不占数据库 |
| 心跳报文 `0x18FFF4F4` | 非国标 ID（PF=0xFF 属 PDU2 群发），1 Hz，只在空闲态发 | 让对端一条 `candump` 就能确认物理链路通不通，不需要万用表 |
| J1939 TP 超时统一取 1000 ms | J1939-21 规定 T1=750 / T2=1250 / T3=1250 / T4=1050 四个独立定时器，本工程简化为单一 `GB_T_TP_TIMEOUT` = 1000 ms + 3 次重试 | 功能上可用，代码量小；不是逐字对齐标准 |
| BMS 不主动上线 | 本工程由充电机侧（i.MX）发起 CHM，BMS 只被动响应 | 模拟器定位：i.MX 是主站，STM32 是从站 |

这四处是设计选择，不是实现疏漏。`GB_T_SIM_PERIOD`（`1000u`）是电池模型的仿真步长、不是协议周期；心跳周期 `BMS_HEARTBEAT_PERIOD`（`1000u`）是非国标报文自己的周期。

BMS 接收并处理的：

| 报文 | 29 位 CAN ID | 处理方式 |
|------|--------------|----------|
| CHM | `0x1826F456` | 空闲态收到则回 BHM、会话号 +1、复位会话统计、进入握手态 |
| CRM | `0x1801F456` | 握手态收到则进入辨识态（打印充电机编号与区域号） |
| CML | `0x1808F456` | 解析最高/最低输出电压、最大/最小输出电流并打印，不驱动状态机 |
| CTS | `0x1807F456` | 解析**压缩 BCD** 的「秒 分 时 日 月 年」（标准表 13 的顺序，不是年月日时分秒）并打印，不驱动状态机 |
| CRO | `0x100AF456` | `0x00` 未就绪 / `0xAA` 就绪 / `0xFF` 无效；充电准备态下收到 `0xAA` 即进入充电态 |
| CST | `0x101AF456` | 充电态 / 充电准备态下收到即进入结束态，中止原因置 0x20 |
| TP.CM | `0x1CECF456` | CTS(0x11) / EndOfMsgACK(0x13) / Abort(0xFF) |
| TP.DT | `0x1CEBF456` | 本工程充电机侧不发长报文，只计数不解析 |

`GB_ID_CSD`（`0x181DF456`）在头文件里有宏定义，但接收分支里没有对应 case，收到 CSD 只会被 `LogFrame()` 打出来。

### BSM 的广播规则

BSM 是全工程唯一不分状态发送的国标报文，固定 250 ms 一次（`GB_T_BSM_PERIOD`），由 `BMS_Protocol_Tick()` 里的全局计时器 `s_next_bsm_ms` 驱动：

```c
if (s_tick_ms >= s_next_bsm_ms) {
    s_next_bsm_ms = s_tick_ms + GB_T_BSM_PERIOD;
    SendBsmTelemetry();
}
```

BSM 走两条不同的通道：

- 协议状态是 `CHARGING` 或 `CHARGING_READY` 时走 `SendOne()`：计入发送统计、打印十六进制。
- 其它状态（待机 / 充满 / 掉电 / 握手等）走 `SendOneQuiet()`：帧照样发出去，但不进统计、不上日志。

头文件里 `GB_T_BSM_PERIOD` 是 `250u`（国标值），改这个宏即可改 BSM 的实际周期。「不分状态广播」是有意保留的扩展：国标只在充电阶段发 BSM，本工程让它全程广播。

BSM 数据域（标准表 20，**7 字节**）：B1 最高单体电压**所在编号**（1/位，**1 偏移**）、B2 最高温度（1 ℃，−50 ℃ 偏移）、B3 最高温度检测点编号（1 偏移）、B4 最低温度（1 ℃，−50 ℃ 偏移）、B5 最低温度检测点编号（1 偏移）、B6 四个 2 位状态字段（SPN3090 单体电压过高/过低、SPN3091 整车 SOC 过高/过低、SPN3092 充电过电流、SPN3093 温度过高）、B7 三个 2 位状态字段（SPN3094 绝缘状态、SPN3095 输出连接器连接状态、SPN3096 充电允许）+ 2 位未定义（按标准 7.9 **填 1**）。绝缘与连接器两项项目没有检测能力，按「不可信状态(10)」上报。**标准 BSM 里没有电压字段**，只有「最高单体电压所在编号」。

### 状态机

```
                    ┌──────────────────────────────┐
                    │            IDLE              │◄──────────────┐
                    │ 空闲：周期发 BSM + 心跳       │               │
                    └──────────────┬───────────────┘               │
                        收到 CHM   │  回 BHM、会话号+1             │
                                   ▼                               │
                    ┌──────────────────────────────┐               │
                    │         HANDSHAKE            │  5 s 无 CRM   │
                    │ 握手：250 ms 周期重发 BHM     ├──────┐        │
                    └──────────────┬───────────────┘      │        │
                        收到 CRM   │                      │        │
                                   ▼                      │        │
                    ┌──────────────────────────────┐      │        │
                    │         IDENTIFY             │      │        │
                    │ 辨识：发 BRM（TP，41 字节）   │      │        │
                    └──────────────┬───────────────┘      │        │
                      TP 完成      │   5 s 超时 / TP 失败 │        │
                                   ▼                      │        │
                    ┌──────────────────────────────┐      │        │
                    │       PARAM_CONFIG           │      │        │
                    │ 参数配置：发 BCP（TP，13 字节）│     │        │
                    └──────────────┬───────────────┘      │        │
                      TP 完成      │   5 s 超时 / TP 失败 │        │
                                   ▼                      │        │
                    ┌──────────────────────────────┐      │        │
                    │      CHARGING_READY          │      │        │
                    │ 充电准备：250 ms 周期发 BRO   │      │        │
                    └──────────────┬───────────────┘      │        │
                    收到 CRO=0xAA   │   5 s 超时           │        │
                                   ▼                      ▼        │
                    ┌──────────────────────────────┐   ┌──────────────┐  │
                    │         CHARGING             │   │ EnterError() │  │
                    │ 充电：BCL 50 ms / BCS 250 ms │   │ 协议类错误     │  │
                    │       BSM 250 ms              │   │ 回空闲态待下次CHM│  │
                    │ 5 s 无充电机报文 / SOC=100%   │   └──────────────┘  │
                    └──────────────┬───────────────┘   └──────────────┘  │
                         结束       │                           │
                                   ▼                           │
                    ┌──────────────────────────────┐            │
                    │         STOPPING             │            │
                    │ 结束：BST×3、BSD×1，3 s 后回空闲│          │
                    └──────────────┬───────────────┘            │
                                   └──────────────────────────┘

     Bus-Off（任意状态都可能发生）►►► EnterError(BMS_ERR_BUS_OFF)
                                         │
                                         ▼
                    ┌──────────────────────────────┐
                    │            FAULT             │
                    │ 故障：红灯常亮，驻留 10 s     │
                    │ 到点调 BMS_Protocol_Reset() │
                    └──────────────┬───────────────┘
                                   └──────►─────── IDLE
```

状态枚举（`BMS_State_t`）与串口上打印的名字：

| 枚举 | 串口名 | 进入条件 | 退出条件 |
|------|--------|----------|----------|
| `BMS_ST_IDLE` | `空闲(IDLE)` | 上电、复位、结束态走完 3 秒 | 收到 CHM |
| `BMS_ST_HANDSHAKE` | `握手(HANDSHAKE)` | 收到 CHM | 收到 CRM；或 5 s 超时 → 错误 |
| `BMS_ST_IDENTIFY` | `辨识(IDENTIFY)` | 收到 CRM | BRM 的 TP 传完；或 5 s 超时 / TP 失败 → 错误 |
| `BMS_ST_PARAM_CONFIG` | `参数配置(PARAM_CONFIG)` | BRM 传完 | BCP 的 TP 传完；或 5 s 超时 / TP 失败 → 错误 |
| `BMS_ST_CHARGING_READY` | `充电准备(READY)` | BCP 传完 | 收到 CRO=0xAA；或 5 s 超时 → 错误 |
| `BMS_ST_CHARGING` | `充电中(CHARGING)` | 收到 CRO=0xAA | 收到 CST / SOC 满 100% / 5 s 无充电机报文 → 结束态或错误 |
| `BMS_ST_STOPPING` | `结束(STOPPING)` | 收到 CST，或 SOC 达到 100% | 驻留 3 s 后回空闲 |
| `BMS_ST_FAULT` | `故障(FAULT)` | CAN 总线 Bus-Off | 驻留 10 s 后调 `BMS_Protocol_Reset()` 自动复位 |

`BMS_Protocol_Reset()` 是一次协议层整体复位：`TP_Reset()`、清会话统计、重新从 Flash 读回电量，然后把 `s_state` 直接置成 `BMS_ST_IDLE`、清错误码、红灯熄灭、充电阶段回到"待机"，并打一行 `[BMS] 协议层复位, 回到空闲态`。它不经过 `SetState()`，与状态绑定那两项副作用由它自己处理。Bus-Off 触发的那 10 秒故障态结束后，屏幕上是"待机"、红灯灭，不会停在"充电中"。

异常处理统一走 `EnterError()`：置错误码、打印告警、红灯常亮，然后按错误性质分两条路——

```c
static void EnterError(BMS_Error_t err, const char *note)
{
    s_error = err;
    BMS_LOG("\r\n*** [BMS] 告警: %s (%s) ***\r\n\r\n", BMS_ErrorStr(err), note);
    LED_R(ON);

    if (err == BMS_ERR_BUS_OFF)
    {
        SetState(BMS_ST_FAULT);     /* 硬件类故障：进故障态，10 秒后自动复位重试 */
    }
    else
    {
        SetState(BMS_ST_IDLE);      /* 协议类超时：回空闲态等充电机重发 CHM */
    }
}
```

两类错误的语义不同：总线关闭是**硬件类**故障，控制器已被硬件关掉，需要"进故障态 → 驻留 10 秒 → 复位重试"这条完整的自愈路径，串口上能看到 `故障(FAULT)`；CRM / CML / CRO / 充电阶段超时、TP 失败、充电机主动中止这些是**协议类**错误，属于对方报文未到齐，回到空闲态等下一次 CHM 即可，不走故障态。

错误码有 CRM 超时、CML/CTS 超时、CRO 超时、充电阶段报文超时、J1939 多帧传输失败、充电机主动中止、CAN 总线 Bus-Off 七种，另加"无"。

Bus-Off 检测块在 `BMS_Protocol_Tick()` 里，每拍先看 `CAN_IsBusOff()`：

```c
if (CAN_IsBusOff())
{
    if (s_state != BMS_ST_FAULT)
    {
        EnterError(BMS_ERR_BUS_OFF, "CAN 总线 Bus-Off, 重新初始化控制器");
    }
    CAN_RecoverBusOff();
    s_passive_since_ms = 0;
    return;
}
```

`s_state != BMS_ST_FAULT` 这个判断不能省：Bus-Off 期间控制器不断被重新初始化，这个标志会在若干拍里持续为真。`EnterError()` 是无条件 `BMS_LOG()` 的（不像 `SetState()` 那样状态没变就早退），没有这个判断的话每一拍都会重打一遍告警、把串口刷满。`CAN_RecoverBusOff()` 与 `s_passive_since_ms = 0` 留在判断之外，因此即使已在故障态，控制器该恢复仍会恢复。

除了 Bus-Off 这一条，其余六种错误码仍全部落到空闲态。

协议状态由 `SetState()` 统一维护：只有状态真的变化才更新时间戳、打印日志，并处理两件与状态绑定的副作用——进入握手态时把充电阶段置为"充电中"，进入结束态或空闲态时把"充电中"改成"充满"或"放电中"（见第七节），进入结束态时复位 BST/BSD 的发送计数。

### J1939 多帧传输（TP）

发送方向用一个四态机（`TP_IDLE` / `TP_WAIT_CTS` / `TP_SENDING` / `TP_DONE` / `TP_ABORT`）：

```
   TP_Start(PGN, buf, len)
        │  包数 = (len + 6) / 7
        ▼
   TP_WAIT_CTS ──── 发 RTS（0x10 + 总长小端 + 包数 + FF + PGN 小端 24 位）────────┐
        │                                                                          │
        │ 收到 CTS(0x11)                                                           │
        ▼                                                                          │
   TP_SENDING ── 逐包发 DT（序号 1 基，7 字节数据，不足补 0xFF）                    │
        │                                                                          │
        ├─ 若对端在 CTS 里限定了块大小 BS，发满一块后回到 TP_WAIT_CTS ──────────────┘
        │
        └─ 全部发完 → TP_DONE（不等待 EndOfMsgACK，收到时只打印日志）
```

超时与重试：等待 CTS 超过 1000 ms（`GB_T_TP_TIMEOUT`）重发 RTS，最多 3 次（`GB_T_TP_RETRY`），仍无响应则转 `TP_ABORT`，状态机侧报 `J1939 多帧传输失败`。CTS 里的 STmin 按毫秒生效，包间隔小于它时不发下一包。

本工程只发两种多帧：BRM（41 字节 / 6 包）和 BCP（13 字节 / 2 包）。接收方向不处理 TP 长报文。

### 接收通路

- 硬件滤波器为 32 位屏蔽位模式，屏蔽码只比较 IDE(bit2) 与 RTR(bit1)，即只收扩展数据帧、不做 ID 过滤。协议层按 29 位 ID 精确匹配。
- RX0 中断把 FIFO0 里的挂起报文一次性搬进 `can.c` 的 64 帧软件环形队列，中断内只做搬运与计满判断。
- 主循环每轮最多取 16 帧（`MAIN_RX_BATCH_MAX`）交给 `BMS_Protocol_OnFrame()`，限幅的目的是保证高负载下 `BMS_Protocol_Tick()` 仍能及时被调用，不影响周期报文发送精度。
- 队列满时丢弃最新帧并累加溢出计数，该计数会出现在每秒的状态行里。

### 总线异常与自动恢复

- `CAN_ABOM = ENABLE`，硬件层面 Bus-Off 自动恢复。
- 软件层面：`BMS_Protocol_Tick()` 检测到 Bus-Off 标志就调 `EnterError(BMS_ERR_BUS_OFF, ...)`——协议状态进 `BMS_ST_FAULT`（红灯常亮，串口打告警），随后重新初始化控制器。故障态驻留 10 秒后由 `BMS_Protocol_Reset()` 复位回空闲态，重新等 CHM。STM32 遇到总线关闭时进故障态、10 秒后自动重试，不会继续在总线上发送。
- 另一条兜底：检测到 `TEC ≥ 128`（错误被动）持续 5 秒，也重新初始化控制器以释放被"等待重传"的帧占满的三个发送邮箱。这一条**不改协议状态**，只是把控制器拉回来。两条合起来的作用是现场把线接好之后，STM32 会在 5~10 秒内自动恢复通信，不需要按复位键。
- `CAN_Config()` 结束时会打印一行自检，读回 `CAN_MSR`、`TEC/REC/LEC` 与 `GPIOA->CRH`，用于判断控制器是否真的离开了初始化模式、PA11/PA12 有没有被别的初始化改掉（PA11 允许 0x4 浮空输入或 0x8 上拉输入，PA12 必须是 0xB 复用推挽）。

---

## 七、充放电状态的模拟规则

电池模型住在 `bms_protocol.c` 的 `Sim_Step()` 里，每 1000 ms（`GB_T_SIM_PERIOD`）推进一拍。它由四个阶段的枚举 `Sim_Phase_t` 驱动，这个阶段就是屏幕上"状态"那一行显示的东西。

```c
typedef enum {
    SIM_PHASE_STANDBY = 0,   /* 待机   */
    SIM_PHASE_CHARGING,      /* 充电中 */
    SIM_PHASE_FULL,          /* 充满   */
    SIM_PHASE_DISCHARGE      /* 放电中 */
} Sim_Phase_t;
```

### SOC 的上涨条件

只有同时满足"阶段 = 充电中"且"协议状态 = `BMS_ST_CHARGING`"的那一拍，SOC 才上涨：

```
s_charge_seconds++;
s_soc_x10 += SIM_SOC_STEP_X10;         /* +5，即每拍 +0.5 % */
if (s_soc_x10 > 1000) s_soc_x10 = 1000; /* 上限 100.0 %  */
```

也就是每秒 +0.5%（1 秒一拍），从**出厂 45.0%** 充到 100.0% 需要 110 秒。起始 SOC 越低、充得越久：SOC 存在 Flash 里跨会话保持（见第八节），除首次上电外每一轮都接着上一轮的电量，因此存在从 2.0% 起充的会话（90 秒窗口、实际充电约 80 秒，SOC 2.0% → 42.0%）。

阶段进入"充电中"的时刻是收到 CHM、协议状态切到握手态的那一瞬间（`SetState()` 里做的），不是进入 `BMS_ST_CHARGING` 的时刻。这两者之间的握手 / 辨识 / 参数配置 / 充电准备四个阶段里，阶段字段保持"充电中"，但每一拍直接返回、不涨电——屏幕上显示"充电中"而数字暂时不动，属正常现象。

### SOC 停止上涨的条件

SOC 到 100.0% 的那一拍，`Sim_Step()` 把阶段切成 `SIM_PHASE_FULL` 并清零保持计数器，同时打印：

```
[BMS] *** 电池已充满, 停止充电, 转入掉电 ***
```

状态机侧在同一拍（或下一拍）检测到 `s_phase == SIM_PHASE_FULL || s_soc_x10 >= 1000`，把中止原因置 0x04（SOC 达到目标值）并进入结束态，发 BST×3 与 BSD×1，3 秒后回空闲态。

### 充满之后

阶段停在"充满"不动，`s_full_ticks` 每拍加一，累计到 `SIM_FULL_HOLD_TICKS = 5`（5 秒）后自动转为"放电中"，并打印 `[BMS] 开始按充电同速率掉电`。这 5 秒的保持是为了等状态机把 BST/BSD 收尾报文发完（结束态本身驻留 3 秒）。

### 停止充电后

充电机按了停止（发 CST），或 BMS 自己中止，状态机切到结束态或空闲态时，`SetState()` 里有一段必须同步的逻辑：

```c
if (st == BMS_ST_STOPPING || st == BMS_ST_IDLE) {
    if (s_phase == SIM_PHASE_CHARGING) {
        s_phase      = (s_soc_x10 >= 1000u) ? SIM_PHASE_FULL : SIM_PHASE_DISCHARGE;
        s_full_ticks = 0;
    }
}
```

也就是说：没充满就停，直接进"放电中"；已经满了就按"充满"处理，走上面那 5 拍再掉电。这一段是保证屏幕上的阶段与协议状态不脱节的关键——协议层停了、阶段字段还留在"充电中"的话，屏幕会一直显示"充电中"、SOC 还继续往上爬。

### 掉电

```c
case SIM_PHASE_DISCHARGE:
    if (s_soc_x10 >= SIM_DISCHARGE_STEP_X10) s_soc_x10 -= SIM_DISCHARGE_STEP_X10;
    else                                     s_soc_x10 = 0u;
```

掉电速率与充电速率完全一致（`SIM_DISCHARGE_STEP_X10` 就是 `SIM_SOC_STEP_X10`），每拍 −0.5%。掉到 0.0% 后保持 0，不会变成负数（先判断再减，无符号数不做直接减法）。

"放电中"阶段没有自动退出条件，会一直保持下去，直到收到新的 CHM（`SetState(BMS_ST_HANDSHAKE)` 把阶段改回"充电中"）或协议层复位。所以放电途中点"充电"能重新充上，是符合设计的行为。

### 待机

上电后、以及 Flash 里没有有效记录时，阶段是"待机"：不充电也不放电，SOC 保持不变。

### 电压、电流、温度、单体

这些量都由 SOC 推导，全部用整数运算，不出现浮点：

| 量 | 公式 | 取值范围 |
|----|------|----------|
| 总压 | `v = 4800 + (soc − 450) × (5840 − 4800) / (1000 − 450)`，单位 0.1 V | SOC 45.0% → 480.0 V；SOC 100.0% → 584.0 V；SOC 0.0% → 约 395.0 V。钳位在 320.0 ~ 584.0 V |
| 电流 | 仅当"阶段 = 充电中"且"协议状态 = `BMS_ST_CHARGING`"时非零。SOC < 95.0%：恒流 100.0 A；SOC ≥ 95.0%：`1000 × (1000 − soc) / 50`，下限 2.0 A（涓流） | 0.0 ~ 100.0 A。其它阶段恒为 0 |
| 最高温度 | `t = 25 + (soc − 450) / 50`，即每 5% SOC 变化 1 ℃。上钳 50 ℃（55 − 5），下钳 10 ℃ | 实测大致 16 ~ 36 ℃ |
| 最低温度 | `t − 6` | — |
| 最高单体电压 | `s_voltage_x10 × 10 / 160`，单位 0.01 V | SOC 45% → 3.00 V；SOC 100% → 3.65 V（正好等于单体上限） |
| 最高单体编号 | `(soc / 7) % 160 + 1` | 1 ~ 160，随 SOC 缓慢漂移 |
| 累计电量 | 每拍 `voltage_x10 × current_x10 / 3600000`，单位 0.1 kWh | 因为电流只在充电阶段非零，实际只在充电阶段累加 |
| 累计充电秒数 | 只有 SOC 真正 +0.5% 的那一拍才自增 | 即"真正在充电的秒数" |

各阶段的报文侧行为汇总：

| 阶段 | SOC | 电压 | 电流 | 累计电量 | 屏幕状态行 |
|------|-----|------|------|----------|------------|
| 待机 | 不变 | 按 SOC 算出 | 0 | 不累加 | 待机（灰） |
| 充电中（握手 ~ 充电准备） | 不变 | 按 SOC 算出 | 0 | 不累加 | 充电中（绿） |
| 充电中（`BMS_ST_CHARGING`） | 每拍 +0.5% | 随 SOC 上升 | 100.0 A 或恒压段衰减值 | 累加 | 充电中（绿） |
| 充满 | 不变 | 按 SOC 算出（100% → 584.0 V） | 0 | 不累加 | 充满（青） |
| 放电中 | 每拍 −0.5%，到 0 停 | 随 SOC 下降 | 0 | 不累加 | 放电中（黄） |

### 进入新会话时 SOC 不复位

收到 CHM 开始新会话时只调用 `Sim_Reset()`，它清的是会话统计（累计秒数、累计电量、中止原因、充满保持计数、电流），**不碰 SOC 与阶段**。电量是电池的属性，必须跨会话、跨重启保持，存在 Flash 里（下一节）。

---

## 八、Flash 电量保存

SOC 与充电阶段存在 STM32F103ZE 的 Flash 最后一页：

```c
#define BMS_FLASH_MAGIC   0x424D          /* 'B','M' */
#define BMS_FLASH_ADDR    0x0807F800UL    /* 512 KB 型号的最后一页，2 KB */
```

记录结构 8 字节：

```c
typedef struct {
    uint16_t magic;    /* 0x424D */
    uint16_t soc_x10;  /* SOC，0.1 % */
    uint16_t phase;    /* 充电阶段枚举 */
    uint16_t chk;      /* magic ^ soc_x10 ^ phase */
} bms_flash_rec_t;
```

写入方式（`Sim_FlashSave()`）：`FLASH_Unlock()` → 清标志 → 擦除整页 → 写 4 个半字 → `FLASH_Lock()`。

写入时机（`Sim_Step()` 末尾）：

- `s_saved_soc` 还是 0xFFFF（本次上电后还没写过）→ 立即写一次；
- SOC 与上次写入值相差达到 50（5.0%）→ 写；
- 否则距上次写入达到 10000 ms（10 秒）→ 写。

一次仿真拍 SOC 只动 0.5%，所以正常运行下大约每 10 秒落盘一次，Flash 擦写寿命足够。

上电读取（`Sim_FlashLoad()`）在 `BMS_Protocol_Init()` 和 `BMS_Protocol_Reset()` 里都会调用，顺序是先把 SOC 置成出厂初值再让 Flash 覆盖它：

1. `s_soc_x10 = SIM_START_SOC_X10 (45.0%)`、`s_voltage_x10 = 480.0 V`、`s_temp_max_c = 25 ℃`、`s_phase = SIM_PHASE_STANDBY`；
2. `Sim_FlashLoad()` 逐项校验魔数、异或校验、取值范围（SOC ≤ 1000，阶段 ≤ 放电中）；
3. 任一校验不过（首次上电时整页是 0xFF）就保留出厂初值，不影响启动；
4. 校验通过则用 Flash 里的 SOC 与阶段覆盖；
5. 若读回的阶段是"充电中"（说明上次是运行中被断电的），改成"放电中"——上电时不可能正在充电。

读回成功时串口打印：

```
[BMS] 掉电记忆: 读回上次电量 62.5%, 阶段=2
```

---

## 九、串口输出说明

USART1，PA9/PA10，115200-8-N-1，`printf` 通过 `fputc()` 重定向。日志分四类。

### 1. 启动横幅（`main.c` + `BMS_Protocol_Init()`）

```
[BOOT] STM32F103ZET6 GB/T 27930 BMS 报文模拟器启动...
[BOOT] HSE = 8 MHz, SYSCLK = 72 MHz, APB1 = 36 MHz
[BOOT] CAN1 = PA11/PA12, 波特率 250 kbps, 扩展帧模式
[BOOT] LED 初始化完成 (红=PB5 故障 / 绿=PE5 充电中)
[CAN] 自检: MSR=0x00000000  TEC=0 REC=0 LEC=0  GPIOA->CRH=0x888B84B4
[CAN] 控制器已进入正常模式，具备发送与应答(ACK)能力
[BOOT] CAN1 初始化完成，进入正常模式并等待总线同步...

========================================================
 GB/T 27930-2015  BMS 报文模拟器 (STM32F103ZET6)
 充电机地址 0x56   BMS 地址 0xF4
 电池: CATL   LFP-512V100A  512.0 V / 100.0 Ah  160 串
 初始 SOC 45.0%   初始总压 480.0 V
 当前状态: 空闲(IDLE)
========================================================

[UI ] 界面版本: 2025-UI-READONLY (只读显示，无触摸、无曲线)
[UI ] UI ready: 480x320
[BOOT] 本地界面初始化完成（LCD 480x320 + 触摸）
[BOOT] 初始化完毕，等待充电机（I.MX6ULL 监控终端）发送 CHM 握手报文
```

`GPIOA->CRH` 那一位解出来应该是 PA11=0x8（上拉输入）、PA12=0xB（复用推挽）。自检发现异常时会改成警告行：

```
[CAN] 警告: CAN 引脚配置异常 PA11=0x.. PA12=0x..（期望 PA11=0x4 或 0x8，PA12=0xB）
[CAN] !! 严重: 控制器仍停在初始化模式(INAK=1)，不会发帧也不会产生 ACK
[CAN] !! 常见原因: 1) CANH/CANL 短路或接反; 2) 收发器未供电/损坏;
[CAN] !!           3) 总线上一直有节点发显性电平(波特率不一致)
```

`[BOOT] 本地界面初始化完成（LCD 480x320 + 触摸）` 这一行是 `main.c` 里的固定文案，文案中的"触摸"不表示界面具备触摸功能，界面本身没有触摸。

### 2. 每秒状态摘要（`BMS_Protocol_PrintStatus()`）

固定三行，第一行是状态与驻留时间，第二行是电池数据，第三行是统计与 CAN 物理层诊断：

```
[   123 s] 状态=充电中(CHARGING)          驻留= 35 s  错误=无
         电压=513.0 V  电流=100.0 A  SOC=62.5 %  单体=3.20 V  温度=22~28 C
         累计充电 35 秒, 累计电量 3.5 kWh; CAN 收 138 帧 / 发 92 帧
         CAN总线: TEC=0 REC=0 LEC=0(无错) 错误中断 0 次   [心跳已发 78 帧]
```

- 第三行末尾会按需追加 `[接收队列溢出 N 帧]`（软件队列满丢帧）与 `[发送失败 N 次]`（无空闲发送邮箱）。
- 第四行是物理层诊断行，`LEC` 会被翻成中文：无错 / 填充错误 / 格式错误 / ACK错误(无人应答) / 位隐性错误 / 位显性错误 / CRC错误。Bus-Off 时行尾追加 `*** Bus-Off! ***`。
- 心跳帧计数只在空闲态增长；一旦收到 CHM 就停止发心跳。

### 3. 帧级日志（`LogFrame()`，由 `BMS_DEBUG_FRAME_LOG` 控制）

每一帧收发都打印方向、ID、长度与十六进制数据：

```
<- 0x1826F456 [8] 01 00 01 00 00 00 00 00
-> 0x182756F4 [8] 01 00 01 00 00 00 00 00
-> 0x181056F4 [5] 14 1E 13 88 02
```

待机时的 BSM 与心跳走静默发送（`SendOneQuiet()`），不进这条通道：BSM 每 250 ms 发一帧，但待机时不打印，串口不会被帧级日志刷屏。

充电态下这一路日志的密度由周期决定：BCL 50 ms 一帧、BCS / BSM 各 250 ms 一帧，按此推算约 28 行/秒（推算值，未实测）。

### 4. 事件与告警（由 `BMS_DEBUG_ENABLE` 控制）

- 状态迁移：`[BMS] 状态切换: 空闲(IDLE) -> 握手(HANDSHAKE)`
- 会话开始：`[BMS] 会话 #1 开始`
- 收到关键报文：CHM / CRM / CML / CTS / CRO / CST 各一行，带解析出的字段
- TP 流程：`[TP] 启动多帧发送` / `[TP] -> RTS` / `[TP] <- CTS` / `[TP] -> DT ...` / `[TP] 多帧发送完成` / 超时重试
- 电池模型：充满、转入掉电、开始掉电
- 结束统计：`[BMS] 本次会话 #1 结束: 中止原因=0x04 充电 110 秒, 累计电量 10.3 kWh, SOC=100%`（从 45.0% 充满到 100.0% 是 110 拍）
- 告警：`*** [BMS] 告警: 等待 CRM 超时 (5 秒内未收到 CRM) ***`
- Bus-Off（由 `EnterError()` 打，进故障态）：`*** [BMS] 告警: CAN 总线 Bus-Off (CAN 总线 Bus-Off, 重新初始化控制器) ***`
- 故障态 10 秒到点复位：`[BMS] 协议层复位, 回到空闲态`
- 总线异常（不驱动状态机的那一条）：错误被动 `TEC ≥ 128`、重新初始化控制器

两个开关都在 `User/Bms/bms_protocol.h`：`BMS_DEBUG_FRAME_LOG`（帧级日志）、`BMS_DEBUG_ENABLE`（全部协议日志，置 0 时 `BMS_LOG()` 展开成空语句）。

### 通信是否正常的判据

按顺序看这四件事：

1. **空闲态是否有心跳。** STM32 进入空闲态后每秒发一帧 `0x18FFF4F4`，8 字节为 `A5 xx 5A yy 00 00 00 00`（xx 是心跳序号低字节，yy 是接收队列长度）。在 I.MX 上执行 `timeout 6 candump -e can0` 能看到它，说明 STM32→I.MX 方向通、且 I.MX 在正常应答。看不到则问题在硬件侧（见第十节）。
2. **诊断行的三个计数器。** 每秒一行的 `CAN总线: TEC=? REC=? LEC=?`：
   - `TEC` 持续涨到 128 且 `LEC=3(ACK错误(无人应答))`：STM32 在发，但总线上没有别的节点应答。对端不在线、只通了一半、或对端处于 listen-only 模式。
   - `REC` 持续增长：总线上有电平，但解不出帧——波特率不一致、极性接反、接线错误。
   - `REC=0` 且收不到任何帧：本节点根本没看到总线活动——收发器没供电、没接、芯片坏、终端电阻缺失。
   - `TEC=0 REC=0 LEC=0` 而收不到帧：总线电气正常，是对端真的没在发。
3. **状态是否按序迁移。** I.MX 按下"充电"后，STM32 的日志应依次出现 `状态切换: 空闲(IDLE) -> 握手(HANDSHAKE)`、`-> 辨识(IDENTIFY)`、`-> 参数配置(PARAM_CONFIG)`、`-> 充电准备(READY)`、`-> 充电中(CHARGING)`，中间夹着 `[TP] 多帧发送完成` 两行（BRM 6 包、BCP 2 包）。任何一步停住超过 5 秒都会打出对应告警并回到空闲态。
4. **通信计数是否在涨。** `CAN 收 N 帧` 应随 I.MX 的周期报文增长；`[接收队列溢出]`、`[发送失败]` 应始终保持 0。

如果看到的是 `状态切换: ... -> 故障(FAULT)`，就是 CAN 总线关闭（Bus-Off）：告警行会带 `CAN 总线 Bus-Off, 重新初始化控制器`，红灯常亮，10 秒后自动打出 `[BMS] 协议层复位, 回到空闲态`。这类情况查线（接反、短路、终端电阻、收发器供电），不用按复位键。

---

## 十、常见问题排查

### 10.1 CAN 不通的排查顺序

STM32 侧的现象通常是 `CAN 收 0 帧 / 发 0 帧`，或 `TEC` 涨到 128 并报 `LEC=3(ACK错误)`。按下面顺序排除，每一步都能用串口日志或 I.MX 端的命令收口：

**第一步：看 STM32 的上电自检。**

```
[CAN] 自检: MSR=... TEC=0 REC=0 LEC=0  GPIOA->CRH=0x888B84B4
[CAN] 控制器已进入正常模式，具备发送与应答(ACK)能力
```

如果打的是 `控制器仍停在初始化模式(INAK=1)`，说明控制器根本没挂上总线（CANH/CANL 短路、收发器没供电、总线上一直是显性电平）。标准库 `CAN_Init()` 内部两处"等 INAK"的循环没有超时，这种情况下程序在别处看起来完全正常（屏幕照画、串口照打），但既不发帧也不产生 ACK。如果打的是 `警告: CAN 引脚配置异常`，检查 PA11 是不是 0x4 或 0x8、PA12 是不是 0xB。

**第二步：确认是"收不到"还是"发不出去"。**

空闲态每秒一帧心跳 `0x18FFF4F4`。在 I.MX 上：

```bash
sudo ip link set can0 down
sudo ip link set can0 type can bitrate 250000 restart-ms 100 listen-only off
sudo ip link set can0 up
ip -details link show can0
timeout 6 candump -e can0
```

- candump 能看到 `18FFF4F4`：STM32→I.MX 方向通，物理层没问题。
- 看不到，而 STM32 报 `TEC=128 LEC=3`：STM32 确实在驱动总线（能完整发完一帧才可能在 ACK 槽报错，说明收发器是好的），但没人应答。
- 两边都看不到，STM32 的 `TEC` 也不动：STM32 侧收发器根本没驱动总线。

**第三步：查 listen-only。**

`ip -details link show can0` 的输出里若带 `LISTEN-ONLY`，控制器只收不发、永远不发 ACK，STM32 的 ACK 错误是必然的，与线路好坏无关。这是诊断时留下的状态，必须显式关掉：

```bash
sudo ip link set can0 type can bitrate 250000 restart-ms 100 listen-only off
```

**第四步：查接线与极性。**

CANH 接 CANH、CANL 接 CANL，两板共地。接反时差分极性相反，显性位被判成隐性，两边都收不到、也都不 ACK，现象与"线没接"几乎一样。极性只能在一端对调验证。

**第五步：查终端电阻与收发器供电。**

断电，量 CANH–CANL，应为约 60 Ω（两端各 120 Ω 并联）。120 Ω 说明只有一端有电阻，∞ 说明都没接，0 Ω 说明短路。

野火 EBF6ULL B1 Pro 底板上的两个 TJA1042 收发器共用一条独立电源轨 `CAN_3V3`，需要经 J8（2×4 排针）用跳线帽短接 1、2 脚才有电，出厂默认不接。J8 未短接时两侧都在发、都收不到，`candump` 没有任何输出：STM32 只有 ACK 错误、没有位错误，I.MX 的 rx 计数恒为 0。旁边长得一样的 J7 是 RS485 电源，不能插错。

**第六步：确认对端是哪一路 can。**

板上 CAN1 端子对应 Linux 的 `can0`，CAN2 端子对应 `can1`。只启用一路设备树插件时，另一路收发器虽然挂在总线上但控制器没使能，不会 ACK，现象与插错端子完全一样。用 `ip -br link | grep can` 确认有哪些接口，再对每一路都配好位定时后用 candump 抓心跳判断线插在哪一路。

**第七步（软件兜底）。** 即使前面都没问题，STM32 侧还有两条自动恢复：Bus-Off 走 `EnterError()` 进故障态、重新初始化控制器，10 秒后自动复位回空闲态重试；`TEC ≥ 128` 持续 5 秒也重新初始化控制器以释放被"等待重传"的帧占满的三个发送邮箱（这一条不改协议状态）。所以现场把线接好之后，STM32 会在 5~10 秒内自行恢复，不必按复位键。如果必须按复位才能恢复，说明板子上跑的固件里没有这两条逻辑。

### 10.2 编译超出 32 KB

链接报：

```
Error: L6047U: The size of this image (xxxxx bytes) exceeds the maximum allowed
for this version of the linker
```

该报错来自 Keil MDK-Lite 免费版的 32 KB 镜像上限，与代码本身无关。处理顺序：

1. **确认字库是 STM32 档位生成的。** 用通用档位生成的字库比 STM32 档位大 8 KB 以上。检查 `User/Ui/font16.h` 开头的宏：

   ```
   #define FONT_ASCII_COUNT    95u     /* ASCII 0x20~0x7E */
   #define FONT_CJK_COUNT      77u     /* 汉字个数 */
   #define FONT_CJK_BASE       1520u   /* 95 × 16 */
   ```

   `FONT_CJK_COUNT` 是几百就说明用的是通用档位，按 10.3 重新生成。

2. **把优化等级提到 Level 2。** 工程里是 Level 2（`<Optim>2</Optim>`，即 `-O2`）。提高优化等级是省 Flash 最直接的一项：同一份代码用 `arm-none-eabi-gcc` 从 -O0 提到 -O2，代码量实测降 22.6%（56557 → 43753 字节）。

3. **把 `User/Ui/ui_port.h` 的 `UI_ENABLE` 置 0。**

   ```c
   #ifndef UI_ENABLE
   #define UI_ENABLE       1       /* 改成 0 = 不要本地界面 */
   #endif
   ```

   置 0 后 `ui_port.c` 与 `ui_app.c` 退化成空实现，整个工程不再依赖 `User/Lcd/` 下的任何文件，省下 `ui_port.o` + `ui_app.o` + `lcd.o` + `ili93xx.o` 四个目标文件合计 **10712 字节 = 10.46 KB** Flash（见第四节）。CAN 与 BMS 模拟器功能完全不受影响，`main.c` / `stm32f10x_it.c` 一行都不用改。这个宏是 `#ifndef` 三段式写的，也可以在 Keil 的 `C/C++` → `Define` 里加 `UI_ENABLE=0` 覆盖。

4. **核对链接器是否在裁未用函数。** map 文件里应有 `Removing Unused input sections from the image.` 这一段。本工程的 `touch.o` / `ctiic.o` 因为没有任何调用点，全部段都被剥掉了。

改完 `UI_ENABLE` 或字库后要 `Rebuild`。

### 10.3 字库的生成与重新生成

`User/Ui/font16.h` 由 `tools/gen_font.py` 生成，不手工修改。生成命令（在仓库根目录执行）：

```bash
python3 tools/gen_font.py --profile stm32 --out-encoding gbk > User/Ui/font16.h
```

脚本做四件事：

1. 用 Pillow 渲染字形：汉字用 Noto Sans CJK，ASCII 用等宽字体，自动挑选能塞进 8×16 / 16×16 单元的字号。
2. **扫描源码里的字符串字面量**收集用字（注释里的中文被忽略）。STM32 档位扫的是 `User/Ui/ui_app.c` 与 `User/Ui/ui_app.h`，另外补上 `UI_DYNAMIC_TEXT_STM32` 里那组"由协议层提供、但会画到屏上"的字——现在收的是 `BMS_ErrorStr()` 的**全部 8 条文案**（`无` / `等待 CRM 超时` / `等待 CML 超时` / `等待 CRO 超时` / `充电阶段报文超时` / `J1939 多帧传输失败` / `充电机主动中止` / `CAN 总线 Bus-Off`）。`BMS_StateStr()` 的状态名不收：界面「状态」那一行画的是充电阶段，协议状态名只走串口。
3. 输出点阵数组 `g_font_data[]`：ASCII 95 个，每个压成 16 字节（只占每行高 8 位）；汉字每个 32 字节。汉字块起始偏移 `FONT_CJK_BASE = 95 × 16 = 1520`。
4. 额外输出 GBK 双字节码到字模下标的映射表 `g_font_gbk_map[]`（需要 `FONT_WITH_GBK_MAP`），供 GBK 源码的 STM32 端二分查找。Linux 端不定义这个宏，用不到这张表。

几个必须知道的约束：

- **`--profile stm32` 不能省。** 通用档位会把 Linux 端界面的文案也收进来，字模数翻倍。当前 STM32 档位是 77 个汉字：字模 `g_font_data[]` 3984 字节（ASCII 95 × 16 = 1520 加汉字 77 × 32 = 2464），GBK 索引表 `g_font_gbk_map[]` 308 字节（77 × 4），合计 4292 字节。
- **脚本必须在仓库根目录跑**，源码路径是相对仓库根解析的。若扫描不到任何字，脚本会退回内置的 `CJK_TEXT` 表并打警告。
- **输出编码必须是 GBK。** Keil 工程里的源码是 GBK，字符串字面量就是 GBK 字节，界面按 GBK 双字节查表，不走 Unicode 转换。`--out-encoding gbk` 直接落盘成 Keil 认的编码。
- **改了界面文案就要重新生成。** 加汉字后 `FONT_CJK_COUNT` 与 `FONT_CJK_BASE` 都会变，`g_font_data[]` 全部重排。
- **`FONT_CJK_BASE` 必须按数组实际长度计算。** 若按 `ascii_count × asc_rowbytes`（= 95 而不是 1520）计算，每个汉字都会从错误的下标取字模，烧录后的表现是屏幕上的汉字笔画错位成碎片，而串口日志完全正常。脚本直接按数组实际长度计算并加断言。

生成后如果界面文案是 UTF-8 写的，还要用 `tools/to_gbk.py` 把 `User/` 下的界面与驱动源码统一回 GBK/CRLF：

```bash
python tools/to_gbk.py            # UTF-8 -> GBK/CRLF
python tools/to_gbk.py --check    # 只检查，不改动
python tools/to_gbk.py --to-utf8  # 改中文之前先转成 UTF-8
```

前两条会改动文件，`--check` 只看不动。**`--check` 现在返回 1**：`User/Ui/font16.h` 是纯 LF（552 行），
是 `gen_font.py` 改成 CRLF 输出之前生成的那一份，也是 `to_gbk.py` 覆盖的 12 个文件里唯一的例外
（其余 11 个都报 `CRLF=n LF=0`）。要让 `--check` 返回 0，两种方式都可以，改的东西不同：

- **只改行尾（不动字模）**：`python tools/to_gbk.py`。`User/Ui/font16.h` 就在它覆盖的 12 个文件名单里，
  默认方向是 UTF-8 -> GBK + CRLF；该文件已是 GBK，因此只有这 552 行 LF 被换成 CRLF，字符数据一字不改。
  不需要 Linux，也不需要字体。
- **重新生成字库内容**：本节开头那条 `python3 tools/gen_font.py --profile stm32 --out-encoding gbk >
  User/Ui/font16.h`。`gen_font.py` 现在输出 CRLF，所以重新生成的这一份行尾也是 CRLF，但**字模内容**
  会按当前脚本与字体重新渲染，需要一台装了 Noto / DejaVu 字体的 Linux。

行尾与字模内容是两件事：前者只影响 `--check` 的换行判定，后者才决定屏上字形。`--check` 返回 1 不是
编码统一失败，只是这份旧产物的行尾没跟上。

字库缺字时，该字在屏上留空（`ui_port.c` 遇到字库中没有的字会推进一格但不画方块）。`UI_DYNAMIC_TEXT_STM32` 收全了 `BMS_ErrorStr()` 的全部 8 条文案（见上面第 2 步），"异常"一行不会缺字；字库覆盖的用字以 77 个汉字为限。

STM32 端的 `User/Ui/font16.h` 是 GBK 编码、77 个汉字；Linux 端的 `linux_can_monitor/font16.h` 是另一份独立生成的字库，UTF-8 编码、498 个汉字（Linux 端没有 32 KB 额度压力，不裁剪）。两者由同一个脚本、同一种字体渲染，字形一致，但字表与编码不是同一份文件，改一边不影响另一边。

### 10.3.1 PC 端语法检查

`User/` 下的全部源文件可以在 PC 上用 gcc 过一遍语法与符号检查，不依赖 Keil：

```bash
# Windows 上打包（CMSIS + 标准外设库头文件 + User 源码）
python tools/pack_syntax.py
# 虚拟机或 WSL 里执行
mkdir -p /tmp/sc && cd /tmp/sc && tar xzf ~/stm32_syntax.tar.gz
make -f tools/syntax_check.mk
```

语法检查用的是真实的标准外设库头文件（`stm32f10x.h` / `core_cm3.h` / `stm32f10x_can.h` 等），不需要桩文件。12 个源文件的结果：

```
gcc -std=gnu90 -Wall -Wextra -fsyntax-only   × 12 个源文件
RC=0，12 个文件全部通过
```

`syntax_check.mk` 在 `-Wall -Wextra` 之外还加了 `-Wno-unused-parameter -Wno-unused-but-set-variable`，"0 warning" 只在这一组开关与对应 gcc 版本下成立。

这 12 个文件是 `main.c`、`stm32f10x_it.c`、`can.c`、`bms_protocol.c`、`bsp_led.c`、`bsp_usart.c`、`ui_port.c`、`ui_app.c`、`lcd.c`、`ili93xx.c`、`ctiic.c`、`touch.c`——最后两个触摸文件没有任何调用点，仍然一起过一遍编译面，以免换触摸芯片时才发现它们编不过。

这条检查是**语法与类型检查**，不产出可烧录镜像，也不是 Keil 构建的替代品：两套编译器的告警集不同。armcc 的 "set but never used"（`syntax_check.mk` 用 `-Wno-unused-but-set-variable` 主动关掉）与过程内可达性分析（不可达语句）都不在这条检查的覆盖范围内。体积与告警数一律以 Keil 的 `Rebuild` 输出为准。

改完源码后用 `make -f tools/syntax_check.mk` 跑一遍这条检查，再进 Keil 构建。

### 10.4 界面相关

**屏幕每 0.5 秒闪一下。** 整屏重绘会造成这种闪动：每 500 ms 先把 480×320 清底、再整屏画一遍，清底与画字之间有一段空白期。当前固件按分层重绘（静态层只在首次绘制，动态层逐格比对后才重画，见第五节），不出现这种闪动；若屏幕仍在闪，板子上跑的不是当前固件。

**屏幕黑、串口正常。** 先看启动日志里有没有 `[UI ] UI ready: 480x320`。没有说明卡在 `LCD_Init()`。有 `UI ready` 但画面全黑，看 `ili93xx.c` 的读 ID 是否为 `0x9488`；读到 `0x0000` 或 `0xFFFF` 说明 FSMC 读时序或 RS 地址线不对，检查 `lcd.h` 的 `LCD_RS_BIT`（默认 10，即 FSMC_A10 接 LCD_RS）。

**颜色发暗、发灰、像负片。** ILI9488 与 ILI9341 部分寄存器编号相同但取值完全不同（例如 0xC0 Power Control 1，ILI9341 是 0x19、ILI9488 是 0x13）。把 ILI9341 的初始化表套到 ILI9488 上就是这个现象。初始化表在 `User/Lcd/ili93xx.c`。

**画面旋转 90 度或左右反了。** 改 `User/Lcd/ili93xx.c` 的 `LCD_MADCTL`（默认 0x28 = 480×320 横屏不镜像）：0x68 左右镜像，0xA8 上下镜像，0xE8 上下左右都镜像。

**触摸点不动。** 固件里没有触摸。`uip_touch_read()` 恒返回 0，`uip_touch_ok()` 恒返回 0，标题栏没有按钮，屏幕是只读的。历史曲线在 I.MX 端看。

工程里仍编译 `User/Lcd/touch.c` 与 `ctiic.c`，但没有任何调用点，链接器会把它们全部剥掉。真要恢复触摸，需要先把 `ui_port.c` 里 `#include "touch.h"` 加回来、让 `uip_touch_read()` 真正调用 `tp_dev.scan(0)`，再从 `ui_app.c` 里重建命中判据与切页逻辑。

### 10.5 Keil 编译/链接常见报错

**`error: #119: cast to type "CAN_Frame_t" is not allowed`**

出现在 `can.c` 的 `CAN_GetRxFrame()`。`s_rx_queue` 声明为 `volatile`，ARM Compiler 5 不允许在结构体类型转换中丢弃 volatile 限定（GCC 容忍这种写法）。当前代码逐字段拷贝，不触发该报错。

**`Symbol xxx multiply defined (by lcd_1.o and lcd.o)`**

看到 `_1.o` 后缀就说明同一个 `.c` 在工程里被加了两次。Keil 对重名文件自动改名（`lcd.c` 编出 `lcd.o`，第二个同名文件编出 `lcd_1.o`），两个目标文件里是同一份符号。在 Keil 左侧文件树里右键删掉重复的那一条即可。判断方法：同一个文件名在树里出现两次。当前工程的链接输入里引用的是 `lcd_1.o`（Keil 重命名的结果），工程文件里只有一条 `lcd.c`，属正常。

**`L6218E: Undefined symbol TP_Init / LCD_Init / ...`**

说明 `UI_ENABLE` 是 1 但 `User/Lcd/` 下的文件没进工程或没在 Include Paths 里。检查工程是否包含 `LCD` 组，以及 `C/C++` → `Include Paths` 里有没有 `..\User\Lcd`。临时绕过办法是把 `UI_ENABLE` 置 0。

**`bsp_usart.c` 报 static 声明与非 static 声明冲突**

`NVIC_Configuration` 在 `bsp_usart.h` 里是公开声明，`bsp_usart.c` 里不能加 `static`。Keil 对此较宽容，PC 上的 gcc 语法检查会抓出来。当前代码不含 `static`。

### 10.6 改完代码后现象不变

先确认板子上跑的确实是新固件，再怀疑代码。三条判据：

1. 串口启动横幅里的 `[UI ] 界面版本: 2025-UI-READONLY`——看不到这行说明不是当前固件。
2. 改了头文件（`font16.h`、`bms_protocol.h`）之后必须 `Rebuild`，只按 `Build` 可能不重编依赖它的源文件。
3. 看 `Project/Objects/yehuoF103.axf` 或 `.hex` 的时间戳有没有更新。

### 10.7 待机时总线被刷屏 / 收发帧数一开始就几千

当前实现里，待机时的 BSM 与心跳都走 `SendOneQuiet()`：帧照发，但不进统计、不上日志。所以：

- 待机时串口每秒只有状态摘要那三行，不应该有 `-> 0x181356F4 ...` 这样的帧级日志；
- 屏幕上"接收 / 发送"两个计数在真正开始充电前应保持不涨。

如果待机时仍然每秒刷一行帧日志、或者收发计数一路涨，说明固件是旧的。

空闲态分支只重置 BHM / BRO / BCL / BCS 四个计时器，BSM 的计时器不能在状态机里重置：`StateMachine()` 在主循环里每秒被调用约 1400 次，一旦每拍把 `s_next_bsm_ms` 重置成"现在"，`if (s_tick_ms >= s_next_bsm_ms)` 恒成立，BSM 就会以约 1400 帧/秒灌满总线，对端的 CST 排不进发送队列，表现为按了停止没有反应。

### 10.8 充电结束后 BST 连续重发

BST 按 **10 ms** 周期发 3 次（`GB_T_BST_PERIOD` = `10u`，标准表 5），BSD 只发 1 次，由 `s_bst_sent` / `s_bsd_sent` 计数器控制。判据必须用计数器而不是时间窗：`StateMachine()` 在主循环里高频调用，按"进入结束态后 50 ms 内发 BST"这类时间窗判据会在窗口内连续发出上千帧。若串口里 BST 一直刷屏，说明固件是旧的。

### 10.9 停止后仍显示"充电中"、SOC 继续上涨

协议状态与充电阶段必须同步。`SetState()` 在进入 `BMS_ST_STOPPING` 或 `BMS_ST_IDLE` 时会把阶段从"充电中"改成"充满"（SOC 已满）或"放电中"（未满）。若停止后阶段没跟着走，检查：

- 收到 CST 时 `s_state` 是否确实是 `BMS_ST_CHARGING` 或 `BMS_ST_CHARGING_READY`——只有这两个状态收到 CST 才会进结束态；
- 结束态驻留 3 秒后是否真的回到了空闲态（日志里应有 `[BMS] 回到空闲态, 等待下一次 CHM 握手`）。

`Sim_Step()` 里判断"充电已停止"只认 `IDLE` / `STOPPING` / `FAULT` 三个状态，不能写成 `s_state != BMS_ST_CHARGING`：握手、辨识、参数配置、充电准备这几个中间状态本来就不等于 `CHARGING`，写成后者的话握手第一拍阶段就被打回"放电中"，表现是"放电中点充电，电量却一直往下掉"。

---

## 十一、性能基准实测

`tools/bench_board.sh` 是在 I.MX6ULL 开发板上采集性能数据的脚本，采的是**充电机侧监控终端**的 CPU、内存、SQLite 与总线帧率；这些数据同时反映本模拟器的工作点，因为总线上的帧全部来自 STM32。

```sh
# 在 I.MX 板上（不是 STM32 上）
cd ~/CAN && make
sudo sh tools/bench_board.sh can0 60          # 默认 charging：发起充电，测充电中
sudo sh tools/bench_board.sh can0 60 idle     # idle：不发起充电，两端停在待机，测待机
```

脚本自己把接口配成 250 kbps 并 up，有 `charging`（充电中）与 `idle`（待机）两种采样模式，区别只有一个参数 —— 是否给 `can_monitor` 加 `--auto-start`。`charging` 模式下它主动发起充电（不需要人去点屏上的"充电"按钮），采的是充电阶段的工作点；`idle` 模式不加 `--auto-start`，本程序停在空闲态、STM32 也停在待机，采到的是心跳 1 Hz + BSM 4 Hz 的待机流量。两种模式都会输出可直接复制的表格：环境信息、CAN 接口状态与位定时、总线帧率与各 ID 分布、进程 CPU 与内存 RSS、缓冲丢弃、程序自身的退出统计、SQLite 记录数与全表扫描速率、系统负载与 TF 卡顺序写。

本模拟器的报文周期与 GB/T 27930-2015 一致（BCL 50 ms、BCS / BSM 250 ms、BST 10 ms）。60 秒充电窗口内各周期报文的实测周期（周期 = `60000 ÷ 帧数`）：

| 报文 | 发送端 | 标准周期 | 实测（60 秒窗口） |
|------|--------|----------|-------------------|
| CCS | i.MX | 50 ms | 1199 帧 / 50.05 ms |
| CRO | i.MX | 250 ms | 240 帧 / 250.0 ms |
| BCL | STM32 | 50 ms | 1197 帧 / 50.13 ms |
| BCS | STM32 | 250 ms | 239 帧 / 251.0 ms |
| BSM | STM32 | 250 ms | 239 帧 / 251.0 ms |

窗口内各 ID 的计数 1199 + 1197 + 240 + 239 + 239 = 3114，正好等于总帧数，五类周期报文全部命中标称值。BCS 与 BSM 的 239 帧是采样窗口边界所致：计数器窗口略短于 60.000 秒，按 `60000 ÷ 239` 得 251.0 ms，标称 240 帧的报文在略短的窗口里采到 239 属正常（窗口内 BCS / BSM 记录到的是 240 帧，即 4.0 Hz）。CRO 与 CHM / CRM / CML 的周期同为 250 ms（`T_CRO_PERIOD_MS` = `250u`）。CTS 是参数配置阶段的时间同步报文，不进充电阶段，60 秒窗口内一帧都没有。

两端的周期任务用绝对调度 `next += period`：写成相对调度 `next = now + period` 会在每次发送时从"当前时刻"重新起算，量化误差逐次累积。

其余实测指标（2026-10-09，i.MX6ULL Cortex-A7 792 MHz 单核，`tools/bench_board.sh`）：

| 指标 | 值 |
|------|-----|
| 充电时总线帧率 | 51 帧/秒（60 秒 3114 帧；脚本用整数除法显示为 51，3114 ÷ 60 = 51.90） |
| 充电时总线负载 | 3.11 % |
| 充电时进程侧开销 | 进程 CPU **4.9 %**、内存 RSS **2244 KB**、缓冲丢弃 0、错误帧 0 |
| 串口输出 | 28 行/秒（115200 下约 11 % 占用，推算值） |
| 数据库增长 | **8.3 MB/小时**（充电阶段，86.5 字节/行） |
| 数据库体积 / TF 卡顺序写 | 932 KiB（954,368 字节：10,647 条原始帧 + 391 条解析记录）/ **30.4 ~ 30.6 MB/s**（`dd bs=1M count=32 conv=fdatasync`） |
| SQLite 全表聚合查询 | **3875 条/秒** |
| 待机时总线 | **5 帧/秒**（60 秒 300 帧）、总线负载 **0.30 %**：BSM 240 帧（4.0 Hz）+ 心跳 60 帧（1.0 Hz） |
| BSM 自身待机帧率 | **4.0 Hz**（60 秒 240 帧） |
| 待机时进程侧开销 | 进程 CPU **1.9 %**、内存 RSS **2056 KB**、缓冲丢弃 0；数据库原始帧 **0**（待机遥测不入库） |

数据库增长一项由充电阶段入库报文的帧率与单行字节数推出：充电阶段入库的是 BCL 19.95 + BCS 3.98 + BSM 3.98 = **27.91 帧/秒**（握手 / 辨识 / 参数配置 / 结束态的报文不入库），单行 86.5 字节，合 27.9 帧/秒 × 3600 秒 × 86.5 字节/行 ≈ 8.3 MB/小时（按 1024 换算，与固件体积 `27408 字节 = 26.77 KB` 同一口径）。86.5 字节/行取自三次全新空库入库实测，合计 954,368 字节 / 11,038 行：

| 实测 | 窗口 | 数据库字节 | `can_raw` | `charge_data` |
|------|------|------------|-----------|---------------|
| ① | 300 秒 | 471,040 | 5473 | 203 |
| ② | 100 秒 | 249,856 | 2693 | 99 |
| ③ | 90 秒 | 233,472 | 2481 | 89 |
| **合计** | — | **954,368** | **10,647** | **391** |

总线吞吐上限按推算给出：250 kbps 总线按扩展帧 8 字节数据、含位填充开销约 150 位估算，理论最大帧率约 1600 ~ 1700 帧/秒（推算值，非实测）。

脚本在开发板上运行：CPU 占用率、SQLite 写入速率、内存占用与硬件强相关（Cortex-A7 792 MHz + TF 卡与 x86 + SSD 差一个数量级），PC 或虚拟机上测出的数字对本项目没有参考价值。

---

## 附：关键参数速查

| 项 | 值 | 位置 |
|----|-----|------|
| 额定容量 | 100.0 Ah | `SIM_RATED_CAP_X10` |
| 额定总电压 | 512.0 V | `SIM_RATED_V_X10` |
| 单体串数 | 160 | `SIM_CELL_COUNT` |
| 单体最高允许充电电压 | 3.65 V | `SIM_MAX_CELL_V_X100` |
| 单体标称电压 | 3.20 V（512.0 V ÷ 160） | `SIM_RATED_V_X10` / `SIM_CELL_COUNT` |
| 总压钳位下限 / 单体下限 | 320.0 V / 2.00 V | `SIM_FLOOR_V_X10` |
| 最高允许充电总电压 | 584.0 V | `SIM_MAX_TOTAL_V_X10` |
| 最高允许充电电流 | 100.0 A | `SIM_MAX_CURRENT_X10` |
| 最高允许温度 | 55 ℃ | `SIM_MAX_TEMP_C` |
| 标称总能量 | 51.2 kWh | `SIM_ENERGY_X10` |
| 出厂 SOC / 总压 | 45.0% / 480.0 V | `SIM_START_SOC_X10` / `SIM_START_V_X10` |
| 仿真步长 | 1000 ms，SOC ±0.5% | `GB_T_SIM_PERIOD` / `SIM_SOC_STEP_X10` |
| 充满保持 | 5 拍 | `SIM_FULL_HOLD_TICKS` |
| Flash 记录地址 | 0x0807F800（最后一页 2 KB） | `BMS_FLASH_ADDR` |
| 电池厂商 / 型号 | `CATL  ` / `LFP-512V100A` | `BMS_MANUFACTURER` / `BMS_MODEL` |
| 生产日期 / 充电次数 | 2024-06-18 / 128 | `BMS_PROD_*` / `BMS_CHARGE_COUNT` |
| 空闲态心跳 | 每 1000 ms，ID `0x18FFF4F4` | `BMS_HEARTBEAT_ENABLE` / `BMS_HEARTBEAT_PERIOD` |
