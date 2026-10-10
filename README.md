# 基于 GB/T 27930-2015 的 BMS CAN 监控终端

STM32F103ZET6 作 BMS 报文模拟器，I.MX6ULL（EBF6ULL B1）作充电机侧监控终端，两端通过 250 kbps 扩展帧 CAN 总线按 GB/T 27930-2015 完成握手、辨识、参数配置、充电准备、充电、结束的完整流程。

- STM32 端：协议状态机、电池模型（充 / 放电）、3.5" ILI9488 只读仪表盘、Flash 电量保存
- I.MX 端：充电流程驱动、J1939 多帧重组、SQLite 落库、4.3" framebuffer 界面
- 两端各有本地屏幕界面，均可脱离对方独立运行：STM32 端可单独用 `candump` 观察，i.MX 端带离线自检与虚拟屏渲染验证

```
   ┌──────────────────────────┐                    ┌──────────────────────────┐
   │  STM32F103ZET6 精英板     │   250 kbps 扩展帧   │  I.MX6ULL (EBF6ULL B1)   │
   │                          │  GB/T 27930-2015   │                          │
   │  BMS 报文模拟器            │ ◄────────────────► │  充电机侧监控终端          │
   │  · 协议状态机              │                    │  · 充电流程驱动            │
   │  · 电池模型（充/放电）      │                    │  · J1939 多帧重组         │
   │  · 3.5" ILI9488 只读仪表盘 │                    │  · SQLite 落库            │
   │  · Flash 电量保存          │                    │  · 4.3" framebuffer 界面  │
   └──────────────────────────┘                    └──────────────────────────┘
```

## 协议实现

已实现 **19 类国标报文**与 J1939 传输协议帧：CHM、BHM、CRM、BRM、BCP、CTS、CML、BRO、CRO、BCL、BCS、CCS、BSM、BST、CST、BSD、CSD、BEM、CEM，外加 TP.CM / TP.DT。

| 层 | 标准依据 | 实现 |
|---|---|---|
| 29 位标识符 | 表 1 的 PDU 结构 + 表 3~表 7 的 PGN 与优先权 | 19 类报文按 `(P<<26)\|(PF<<16)\|(DA<<8)\|SA` 计算，两端一致 |
| 数据域字节序 | 4.4，数据信息传输采用低字节先发送的格式 | 两端多字节字段一律小端（低字节先发） |
| 未定义位 / 预留位 | 7.9，本标准未规定的位或预留位填充 1 | 一律填 1（`GB_FILL_RESERVED`） |
| 数据长度 | 表 3~表 7 的「数据长度 byte」列 | 按标准实际长度发送（BRO/CRO 1 字节、BHM 2、CHM 3、BCL 5、BSM 7 …） |
| 超时判定 | 第 8 章通用 5 s + 各报文正文的特殊规定 | BCL 1 s、CCS 1 s、BCS 5 s、BRO/CRO 60 s、其余 5 s |

### 发送周期

| 报文 | 周期 |
|---|---|
| BCL 电池充电需求 | 50 ms |
| CCS 充电机充电状态 | 50 ms |
| BCS 电池充电总状态 | 250 ms |
| BSM 动力蓄电池状态 | 250 ms |
| CRO 充电机输出准备就绪 | 250 ms |
| CHM / CRM / CML | 250 ms |
| CTS 时间同步 | 500 ms |
| BST 中止充电 | 10 ms |
| CST 中止充电 | 10 ms |
| BEM / CEM 错误报文 | 250 ms |

CHM / CRM / CML / CTS 属握手与参数配置阶段，BEM / CEM 优先权 2。

### 周期实测

| 报文 | 标准周期 | 实测 |
|---|---|---|
| BCL | 50 ms | 50.13 ms |
| CCS | 50 ms | 50.05 ms |
| BCS | 250 ms | 251.0 ms |
| BSM | 250 ms | 251.0 ms |
| CRO | 250 ms | 250.0 ms |

采样为 I.MX6ULL 开发板上 60 秒充电窗口的 `candump` 计数，周期按 `60000 ÷ 帧数` 换算。BCS 与 BSM 在 60 秒窗口内计得 239 帧，为窗口边界所致。

### 实测性能

| 指标 | 充电中 | 待机 |
|---|---|---|
| 总线帧率 | 51 帧/秒 | 5 帧/秒 |
| 总线负载 | 3.11 % | 0.30 % |
| 进程 CPU 占用 | 4.9 % | 1.9 % |
| 内存 RSS | 2244 KB | 2056 KB |
| 数据库增长 | 8.3 MB/小时 | 0 |
| SQLite 全表聚合查询 | 3875 条/秒 | — |

测试环境：I.MX6ULL Cortex-A7 @ 792 MHz 单核，Debian，Linux 4.19.35-imx6，TF 卡顺序写 30.4 ~ 30.6 MB/s。内存 RSS 在 2056 ~ 2280 KB 之间波动属正常。采集脚本 `tools/bench_board.sh` 随仓库提供，可复现。

## 固件体积

Armcc V5.06 update 7，`<Optim>2</Optim>` 即 -O2：

```
Program Size: Code=21264  RO-data=5908  RW-data=236  ZI-data=2572
".\Objects\yehuoF103.axf" - 0 Error(s), 0 Warning(s).
```

```
Total ROM = Code + RO-data + RW-data = 21264 + 5908 + 236 = 27408 字节 = 26.77 KB
Total RAM = RW-data + ZI-data         =          236 + 2572 =  2808 字节 =  2.74 KB
```

Total ROM 距 MDK-Lite 的 32 KB 上限 5360 字节 = 5.23 KB。`Total ROM Size` / `Total RW Size` 两行在链接器生成的 map 文件里（本工程 `Project/Listings/yehuoF103.map`，"Image component sizes" 段末尾）：

```
        Total RO  Size (Code + RO Data)                27172 (  26.54kB)
        Total RW  Size (RW Data + ZI Data)              2808 (   2.74kB)
        Total ROM Size (Code + RO Data + RW Data)      27408 (  26.77kB)
```

map 文件里的 `kB` 按 1024 计，`27408 ( 26.77kB)` 即 27408 ÷ 1024。

## 快速开始

### STM32 端（Keil MDK5）

用 Keil 打开 `Project/yehuoF103.uvprojx`，Rebuild 后烧录。工程用 MDK-Lite 时有 32 KB 镜像上限，当前 ROM 占用 26.77 KB。

```bash
# 重新生成字库（Linux 虚拟机，需要 fonts-noto-cjk）
python3 tools/gen_font.py --profile stm32 --out-encoding gbk > User/Ui/font16.h
```

### I.MX6ULL 端（Debian + SocketCAN）

```bash
make                            # 编译
make selftest && ./selftest     # 离线自检，PASS 347 / FAIL 0
sudo ./can_monitor -i can0      # 正式运行（默认带本地屏界面）
```

**硬件前提**：野火 EBF6ULL B1 Pro 底板的两个 CAN 收发器共用一条独立电源轨 `CAN_3V3`，必须用跳线帽短接 J8（2×4 排针）的 1、2 脚，出厂默认不接。未短接时收发器无电，总线上一帧都收发不了。详见 [I.MX 端 README](linux_can_monitor/README.md) 第三节第 0 小节。

## 复现步骤

前三步不需要 CAN 硬件。

**第零步：语法 / 编译检查。**
```bash
cd linux_can_monitor && make clean && make -j4 && make selftest
# selftest PASS 347 / FAIL 0

# STM32 端：12 个源文件的 gcc -std=gnu90 -fsyntax-only 检查
cd .. && python3 tools/pack_syntax.py
mkdir -p /tmp/st && tar xzf stm32_syntax.tar.gz -C /tmp/st && cd /tmp/st
make -f tools/syntax_check.mk                                     # RC=0，12 个文件全部通过
```

**第一步：Linux 端软件链路。**
```bash
cd linux_can_monitor
make && make selftest && ./selftest        # PASS 347 / FAIL 0
```

**第二步：端到端联调（vcan 虚拟总线）。**
```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan && sudo ip link set vcan0 up
sudo bash run_e2e_test.sh                  # PASS 19 / FAIL 0
```

**第三步：本地界面（虚拟屏）。**
```bash
sudo bash run_gui_test.sh                  # PASS 15 / FAIL 0
```

**第四步：上真硬件。**
1. 两块板子按 `CANH↔CANH`、`CANL↔CANL` 接线，两端各 120 Ω 终端电阻，J8 跳线短接
2. STM32 端用 Keil 打开 `Project/yehuoF103.uvprojx`，Rebuild 后烧录
3. I.MX 端 `make` 后运行 `sudo ./can_monitor -i can0`
4. 屏上点「充电」，观察两端状态同步；需要看细节时加 `-v`

**第五步：采性能数据（可选）。**
```bash
sudo sh tools/bench_board.sh can0 60          # 充电工况
sudo sh tools/bench_board.sh can0 60 idle     # 待机工况
```

## 目录结构

```
User/                   STM32 端源码
  Bms/                  协议实现、状态机、电池模型（bms_protocol.c/h 是核心）
  Can/                  CAN 驱动
  Lcd/                  LCD / 触摸 / I2C 驱动（触摸不在调用链上）
  Ui/                   本地界面（只读仪表盘 + 点阵字库）
  Usart/ Led/           串口、指示灯

linux_can_monitor/      I.MX 端源码
  gb27930.c/h           协议状态机与报文编解码
  isotp.c/h             J1939 多帧传输
  can_layer.c/h         SocketCAN 封装 + rtnetlink 在线配置
  ring_buffer.c/h       无锁环形缓冲
  storage.c/h           SQLite 持久化（WAL + 批量事务）
  session.c/h           充电会话历史
  ui.c/ fbdev.c/ gui*.c 终端仪表盘 + framebuffer 图形界面
  uitouch.c/ uitest.c   触摸抽象、界面离线渲染自检
  selftest.c            协议层离线自检
  run_e2e_test.sh       vcan 端到端联调测试
  run_gui_test.sh       界面整链路测试（虚拟屏 + 脚本化触摸）

Project/                Keil 工程文件
Libraries/              STM32 标准外设库 + CMSIS
tools/                  开发期工具（字库生成、语法检查、基准测试等）
Doc/ui_preview/         界面渲染效果图
```

## 文档

| 文档 | 内容 |
|---|---|
| [项目总手册](README_GB27930项目手册.md) | 报文对照表、硬件接线、两端部署、联调步骤、故障速查 |
| [STM32 端说明](README_STM32_BMS模拟器.md) | 协议实现要点、充放电模拟规则、Flash 保存、串口输出 |
| [I.MX 端说明](linux_can_monitor/README.md) | 模块说明、编译部署、命令行参数、性能实测、常见问题 |
| [开发问题记录](GB27930项目开发问题记录.docx) | 全程 **144 个问题**的现象、原因、解决与代码位置 |
| [工具说明](tools/README.md) | 字库生成、语法检查、基准测试等工具的用法 |

## 非国标扩展

1. **BSM 不分状态广播** —— 国标只在充电阶段发 BSM，这里在待机 / 充满 / 掉电阶段也按 250 ms 周期发送。待机时 4 帧/秒、负载约 0.24 %，待机帧不入库。
2. **心跳报文 `0x18FFF4F4`** —— 非国标 ID，1 Hz，只在空闲态发。不接任何仪器、只用 `candump` 即可判断物理链路是否连通。
3. **J1939 TP 超时统一取 1000 ms** —— J1939-21 规定 T1=750 / T2=1250 / T3=1250 / T4=1050 四个独立定时器，本工程为单一超时加 3 次重试。
4. **BMS 不主动上线** —— 由充电机侧发起 CHM，BMS 只被动响应。

## 与标准的已知差异

| 差异 | 说明 |
|---|---|
| BCS 按 8 字节发送（标准 9 字节） | 9 字节超过 CAN 单帧上限，按标准 6.2 注 7 应经 J1939 传输协议发送。按 8 字节单帧发送时缺 B9（剩余充电时间高字节），代码注释已标注。 |
| BRM 为 41 字节 | 标准表 3 声明 41 字节，表 11 逐行相加为 49 字节，本实现按表 3 的 41 字节，不发送 SPN2576（BMS 软件版本号 8 字节）。 |
| BMV / BMT / BSP 未实现 | 标准 9.3 规定这三类为可选报告，充电机不对其进行报文超时判定。 |
| BSD 的「单体最低电压」、CSD 的「输出能量」为估算值 | 项目只建模一路单体电压、无电能计量，分别按「最高压 − 40 mV」与「电压×电流积分」上报，代码注释已标明非实测。 |
| BSM 的绝缘 / 连接器状态填「不可信(10)」 | 项目没有相应检测能力。 |
| BCS 的「估算剩余充电时间」填 `0xFF` | 项目没有估算能力，按 7.9 填 1 表示未规定。 |

## 开发环境

- STM32：Keil MDK5（Armcc V5.06 update 7，-O2），正点原子 STM32F103ZET6 精英板
- I.MX：Ubuntu 18.04 虚拟机交叉编译，野火 EBF6ULL B1 Pro（Debian 10，内核 4.19.35）
- 主机侧验证：`tools/syntax_check.mk` 用 `gcc -std=gnu90 -fsyntax-only` 检查 12 个 STM32 源文件
- 三个编译环境的告警数不同：Keil 与 Ubuntu 18.04 + gcc 7.5.0 下均为 0 error / 0 warning，板子 Debian 10 的较新 gcc 会额外报 2 个 `-Wformat-truncation`（`ui.c:401`、`gui.c:313`）

## 硬件资料说明

两块开发板的接线与引脚定义参考厂商官方文档：

- 正点原子 STM32F103ZET6 精英板原理图与 IO 引脚分配表
- 野火 EBF6ULL B1 Pro 底板原理图与装配图

这几份资料由厂商发布，未随本仓库分发，到对应厂商的资料包或官网获取。

其中 EBF6ULL B1 Pro 底板上的两个 CAN 收发器共用一条独立电源轨 `CAN_3V3`，必须用跳线帽短接 J8（2×4 排针）的 1、2 脚才有电，出厂默认不接。详见 [I.MX 端 README](linux_can_monitor/README.md) 第三节第 0 小节。
