# tools/ —— 开发期工具（不编进固件，交付时可整个删掉）

这里放的是「为了让两端界面可验证」而写的一次性工具。它们不参与运行，
但建议保留：改界面文案或布局时能立刻复查，比在板子上反复烧录快得多。

---

## 1. `gen_font.py` —— 点阵字库生成器

把界面源码里用到的所有字符渲染成 16×16 点阵，输出 `font16.h`。

```bash
# 在 Ubuntu 虚拟机上（需要 Pillow 和一套中文字体）
sudo apt-get install -y python3-pil fonts-noto-cjk fonts-dejavu
python3 tools/gen_font.py > font16.h          # 输出到当前目录
python3 tools/gen_font.py --art "电压0125."    # 终端里打印 ASCII 艺术字，自检字形
python3 tools/gen_font.py --preview /tmp/p.png # 输出放大预览图
```

**它会自动扫描源码**里**字符串字面量**中出现的所有非 ASCII 字符并全部收进字库，
因此不会出现「改了文案忘补字、屏上显示空白」的问题。注释里的汉字会被排除 ——
本工程注释是全中文的，光是界面源码的注释里就有 900 多个不同汉字，
全收进来就是几十 KB Flash。

`--profile stm32` 的扫描面只有 `User/Ui/ui_app.c` 与 `User/Ui/ui_app.h` 两个文件
（后者没有字符串字面量，实际贡献 0 字）。协议层的 `User/Bms/bms_protocol.c`
**不扫**：那个文件里 150 多个汉字只走串口日志，扫进来实测会让字库从 77 字涨到
205 字，按每字 36 字节算是 5 KB 以上的 Flash，而 Keil MDK-Lite 只给 32 KB 总额度。

所以协议层真正会画到屏上的那几条文案靠 `UI_DYNAMIC_TEXT_STM32` 元组**显式列出**
（当前只有 `BMS_ErrorStr()` 的 8 条），不靠扫描。`BMS_StateStr()` 的状态名不上屏，
故意不收。以后改这里请记住：**先量再删**，把元组减到只剩实际会出现的那几条，
重新生成后到 Keil 里看 ROM 占用。

生成物要点：

| 项 | 值 |
|----|----|
| 字模尺寸 | 16×16，每行 2 字节，共 32 字节/字 |
| ASCII | 0x20~0x7E 共 95 个，只画左侧 8 列，绘制步进 8 |
| 汉字（STM32） | 77 个，用满 16 列，绘制步进 16 |
| 汉字（Linux） | 498 个，用满 16 列，绘制步进 16 |
| 总大小（STM32） | `g_font_data[]` 3984 字节（ASCII 95×16 = 1520 + 汉字 77×32 = 2464）<br>+ `g_font_gbk_map[]` 308 字节（77×4）= **4292 字节（约 4.19 KB）** |
| 查找表 | `g_font_cjk_map[]`（Unicode 码点 → 下标）<br>`g_font_gbk_map[]`（GBK 双字节码 → 下标，STM32 端用，需 `#define FONT_WITH_GBK_MAP`） |

> **为什么 STM32 端用 GBK 表而不是 Unicode 表**：Keil 工程的源码是 GBK 编码，
> 字符串字面量里躺的就是 GBK 双字节码。与其在单片机上塞一张巨大的
> 「GBK → Unicode」转换表，不如由 PC 在生成字库时就把每个字的 GBK 码算出来，
> 单片机只做「读两个字节 → 二分查表 → 拿到字模下标」，零额外开销。

> **编码自动识别**：脚本对每份源码同时尝试 UTF-8 与 GBK 解码，然后按
> 「ASCII + 常用汉字占比」打分择优。这样同一份脚本既能读 Linux 端的 UTF-8
> 源码，也能读 STM32 端的 GBK 源码。

> **两端是两份独立的字库，不要互相覆盖**：STM32 端落在 `User/Ui/font16.h`
> （GBK 编码，`FONT_CJK_COUNT 77u`），Linux 端落在 `linux_can_monitor/font16.h`
> （UTF-8 编码，`FONT_CJK_COUNT 498u`）。两者由不同的 `--profile` 生成、
> 收字范围不同，因此条数必然不同、也不能互换：STM32 那边只有 Keil MDK-Lite
> 的 32 KB 总额度，把 Linux 端的 498 个字塞进去直接顶爆。用第 9 节的命令
> 分别生成、分别落盘即可，不要用一次 `--profile all` 的输出同时喂给两边。
> 两端的字模仍由同一套字体渲染，字形完全一致，便于对照。
> （用当前源码重新跑一次 `--profile linux` 得到的是 495 字 —— 仓库里那份
> Linux 字库是在文案再改过几处之前落盘的，比源码少 3 个字。这不影响屏上显示，
> 因为少的那几个字当前没有画到屏上；重跑一遍即可对齐。）

---

## 2. `to_gbk.py` —— 统一 STM32 源码编码

Keil 工程里原有的 `.c/.h` 一直是 GBK 编码；新加的界面文件最初按 UTF-8 写，
必须转换，否则在 Keil 里中文注释是乱码，严重时还会因为多字节序列被误解析。

```bash
python tools/to_gbk.py            # 转换（UTF-8 → GBK，并统一成 CRLF）
python tools/to_gbk.py --check    # 只检查当前编码，不改动
```

顺带会检查并报告 UTF-8 BOM —— **Keil 遇到带 BOM 的工程/源文件会直接报错**
（本项目就踩过一次：PowerShell 的 `[System.Text.Encoding]::UTF8` 写文件会带 BOM）。

---

## 3. `patch_stm32_ui.py` / `patch_keil.py` —— 接线脚本

| 脚本 | 作用 |
|------|------|
| `patch_stm32_ui.py` | 把 `UI_Init()` / `UI_Tick()` / `UI_Tick_1ms()` 接进 `User/main.c` 与 `User/stm32f10x_it.c`，并同步更新文件头注释 |
| `patch_keil.py`     | 把 `User/Ui/*.c` 挂进 `yehuoF103.uvprojx`（`Ui` 组 + IncludePath）与 `yehuoF103.uvoptx`（文件树显示） |

两个脚本都是**幂等**的（已打过补丁就跳过），且对 GBK 文件按 GBK 读写，
不会破坏原有编码。片段放在 `_splice/` 目录（UTF-8）。

---

## 4. `pack_syntax.py` + `syntax_check.mk` —— STM32 语法检查

STM32 代码在 PC 上没法真正编译，但可以用 x86 的 gcc 做一次 `-fsyntax-only`：
类型不匹配、函数原型不符、变量未声明、C89 里「声明必须在块首」这类问题
都能提前抓出来，比在 Keil 里反复 Build → 烧录 → 看现象快得多。

```bash
# Windows 上打包（CMSIS + 标准外设库头文件 + User 源码 + tools/stub 里的驱动桩）
python tools/pack_syntax.py

# 虚拟机上执行
mkdir -p /tmp/sc && cd /tmp/sc && tar xzf ~/stm32_syntax.tar.gz
make -f tools/syntax_check.mk
```

实测：**12 个源文件全部通过** `gcc -std=gnu90 -Wall -Wextra -fsyntax-only`，
`RC=0`（口径说明：`syntax_check.mk` 在 `-Wall -Wextra` 之外还加了
`-Wno-unused-parameter -Wno-unused-but-set-variable`，所以"零告警"只在这一组开关下成立；
换编译器版本或换开关结论可能不同 —— i.MX 端就出现过 Ubuntu 18.04 + gcc 7.5.0 是
0 error / 0 warning、板上 Debian 10 的较新 gcc 额外报 2 个 `-Wformat-truncation` 的情形）。
**这三处（Keil / Linux 虚拟机 / 板子）之外还有第四个口径**：Keil 端（Armcc V5.06 update 7，
`<Optim>2</Optim>`）最近一次 Rebuild（2026-10-09 18:39，修完两条警告之后那一轮）是
**`0 Error(s), 0 Warning(s)`**，输出 `Program Size: Code=21264 RO-data=5908 RW-data=236
ZI-data=2572`，即 **Total ROM = 27408 字节 = 26.77 KB**。修警告前那一轮（2026-10-09 18:22）
曾报 **`0 Error(s), 2 Warning(s)`** ——
`bms_protocol.c(551) #550-D: s_ccs_permit set but never used`（**已修**，补了状态变化日志，
修后该变量既有写也有读、全文件 6 处，代价是 Total ROM +64 字节）
与 `User/Lcd/touch.c(237) #111-D: statement is unreachable`（**已修**：根因是
`return TP_CHIP_NONE;` 被留在块注释结束符 `*/` 同一行，现已整行删除，不会引入 `#940-D`；
`touch.c` / `ctiic.c` 不在调用链上、全部段都被链接器剥掉，所以这处对体积的影响是 0 字节）。
**两条都已修掉，并在 18:39 那次 Rebuild 实测消除 —— 现在 Keil 端也是 `0 Error(s), 0 Warning(s)`。**
注意 `syntax_check.mk` 的 `-Wno-unused-but-set-variable` **恰好会屏蔽掉第 1 条 armcc 警告**，
所以"Linux 语法检查 0 warning"与"Keil 曾报 2 个 warning"**同时成立、并不矛盾**。
12 个文件就是 `syntax_check.mk` 里 `SRCS` 列出的那 12 个：
`User/main.c`、`User/stm32f10x_it.c`、`User/Can/can.c`、`User/Bms/bms_protocol.c`、
`User/Led/bsp_led.c`、`User/Usart/bsp_usart.c`、`User/Ui/ui_port.c`、`User/Ui/ui_app.c`、
`User/Lcd/lcd.c`、`User/Lcd/ili93xx.c`、`User/Lcd/ctiic.c`、`User/Lcd/touch.c`
（`syntax_check.mk` 自身在 `-Wall -Wextra` 之外还加了
`-Wno-unused-parameter -Wno-unused-but-set-variable`）。
这套检查实际抓到过 3 个真实问题：

1. `can.h` 的位定时宏与 `CAN_InitTypeDef` 成员重名（宏展开把结构体成员替换掉了）；
2. 自定义 `CAN_GetLastErrorCode()` 与标准库同名函数冲突；
3. `NVIC_Configuration` 在头文件里是公开声明、在 `.c` 里却加了 `static`。

`tools/stub/lcd.h`、`tools/stub/touch.h` 是**驱动桩文件**，签名与正点原子例程一致，
只声明 `ui_port.c` 用到的那几个符号。它们只服务于这次检查，不属于交付内容。

> 这一套检查覆盖 `User/Bms/bms_protocol.h`（`syntax_check.mk` 的 `SRCS` 会连带它的
> 头文件）。该头文件里的周期宏已按 GB/T 27930-2015 对齐：`GB_T_BCL_PERIOD` = 50u、
> `GB_T_BCS_PERIOD` / `GB_T_BSM_PERIOD` = 250u、`GB_T_BST_PERIOD` = 10u
> （充电机侧的 `CTS` 周期 500u 在 i.MX 端的 `gb27930.c` 里）。
> 周期是纯配置值，不影响本工具的任何检查逻辑，也不需要改 `pack_syntax.py`
> 或 `syntax_check.mk` —— 重新打包跑一遍即可。
>
> **这一层检查的价值在本轮得到了验证**：在「按标准原文全面修正报文 ID / 字节序 /
> 字段布局」的改动中，正是它抓出了两个只有编译器才能发现的错误 ——
> `fill_reserved` 的声明顺序错误（`Build_BEM()` 出现得比函数定义早 80 行）、
> 以及 `bcd2bin` 只写了调用没写定义。此前 STM32 端只能靠 Windows 上的 Keil
> 人工 Rebuild，这类问题在自动流程里是盲区。
>
> **反过来的教训（问题 143 / 144）**：有了编译器这一层，也**不能反过来用静态自检替代它**。
> 周期调度修复那一轮里，子任务改完 `bms_protocol.c` 后静态自检是全绿的（大括号平衡
> 173/173、宏定义 1 处、`GB_SCHED_NEXT` 出现 9 次、`grep` 无相对调度残留），
> 但真编译**报了 29 个错**：插入 `GB_SCHED_NEXT` 宏时把紧随其后的
> `§3 模块静态状态` 注释块起始行 `/*==========…` 一起删掉了，注释符没了、
> ` * §3 …` 变成代码，`§`（GBK 字节 `A1 EC`）被 gcc 报成
> `bms_protocol.c:529:4: error: stray '\241' in program`，注释块断裂后
> `s_tick_ms` 的声明不再被解析，级联出 29 个错误。补回注释起始符后 →
> **`RC=0`，12 个文件全部通过**。
>
> **本轮又栽了第三次（已与上面合并成问题 144）**：修 `touch.c` 那行不可达语句时，
> **在注释里写了字面的 `*/`**，把注释块**提前终止**，真编译报 **215 个错误**
> （`stray '\`'`、`stray '\265'` …）。三次的共同点是**注释块被破坏**，且**前两次静态自检全绿**。
>
> **教训：对源码做替换式编辑后，"大括号平衡 + 宏计数 + grep"不足以判定可编译 ——
> 注释块的完整性、以及被替换边界吞掉的相邻行，只有真编译器能发现。**
> **排查顺序也因此固定下来：先跑 `make -f tools/syntax_check.mk`，再看静态自检的结论。**
>
> **两端工具的覆盖范围不同，必须都跑**：`gcc -fsyntax-only` 能抓**语法级**的注释块错误，
> **但抓不到 `-Wunused-but-set-variable` 这类代码生成阶段的告警** —— 这正是
> `s_ccs_permit` 被 gcc 放过、被 Keil（armcc）抓到的原因。两端的 "0 warning" 不能互相替代。

---

## 5. `pc_ui/` —— 在 PC 上把 STM32 界面渲染出来

STM32 的界面没法在 PC 上真跑，但可以把它**原样编译**出来，把绘制指令落在
一块内存里的 480×320 虚拟屏上，再导出 ASCII 字符画和 PPM 图片，用来检查
元素重叠、文字出屏、进度条比例等布局问题。

```bash
make -f tools/pc_ui/Makefile run
```

关键编译选项：

```
-finput-charset=GBK     源码是 Keil 用的 GBK
-fexec-charset=UTF-8    编译时把字符串字面量转成 UTF-8，
                        这样才能直接查 font16.h 里的 Unicode 字模表
```

这个组合顺带验证了 **GBK 源码本身是合法的**（非法序列会让 gcc 直接报错）。

工具会做这些事并打印结果：

1. 渲染主界面 → ASCII 字符画 + `/tmp/stm32_main.ppm`
2. 注入一次合成触摸（点 (413,17)）并推进 100 ms
3. 再渲染一次 → ASCII 字符画 + `/tmp/stm32_curve.ppm`
4. 遍历各组「状态机状态 × 异常码」组合做渲染冒烟测试

> **工具本身停留在「触摸切页 + 历史曲线页」那一版**：`pc_ui.c` 仍在往
> `uip_touch_read()` 里灌 (413,17)、(200,200) 两次合成触摸，找的也是
> `UI_IsCurveScreen()` 里的历史曲线页。而 STM32 端界面现在是**只读仪表盘**：
> 没有触摸、没有曲线页，`UI_IsCurveScreen()` 恒返回 0，`ui_app.c` 也不再调用
> `uip_touch_read()` —— 那两次触摸是死代码。所以第 2、3 步实际上又渲染了一遍
> 主界面，`/tmp/stm32_curve.ppm` 的内容与 `stm32_main.ppm` 相同，**不是曲线图**。
> 下面那张表里的曲线范围、触摸切页两行同理，都是改造前的测量值，留作记录。

实测（对导出的 PPM 逐像素核对）：

| 检查项 | 结果 |
|--------|------|
| 主界面内容外接矩形 | `x 0..479, y 0..316`（未出屏、未越过最后一个面板） |
| 三个读数文字最右边界 | 176 / 177 / 161，进度条从 300 开始 → 无重叠 |
| 电压 / 电流 / SOC 进度条 | 99 % / 28 % / 98 %，与 `4800/5840`、`292/1000`、`988/1000` 一致 |
| SOC 曲线范围 | `x 46..470, y 50..134`（绘图区 `x 46..470, y 44..208`）—— 改造前，曲线页已删除 |
| 电压曲线范围 | `x 46..470, y 44..204` —— 改造前，曲线页已删除 |
| 触摸切页 | 主界面→曲线 ✓，曲线→主界面 ✓，点空白不触发 ✓ —— 改造前，现已无触摸 |

> `tools/pc_ui/bms_protocol.h` 是协议层公开接口的**桩**（因为 PC 上不需要
> 整个 StdPeriph 库）。**如果以后给 `BMS_UiSnapshot_t` 加了字段，
> 记得同步改这个桩**，否则 PC 预览会和板子上的实际显示不一致。

---

## 6. `ppm2png.py` —— PPM 转 PNG，便于目视检查

C 侧导出 PPM 不需要任何压缩库（PNG 依赖 zlib），代码只有几十行；
PPM 是无损位图，转换交给 PC 上的 Pillow 一行搞定。这样开发板上既不用装
图形库，也不用引入压缩依赖。

```bash
python3 tools/ppm2png.py /tmp/gui_frames            # 批量转换整个目录
python3 tools/ppm2png.py /tmp/stm32_main.ppm        # 单个文件
python3 tools/ppm2png.py /tmp/gui_frames --scale 2  # 放大 2 倍，看细字用
```

两端都会用到它：

| 产出方 | 命令 | 输出 |
|--------|------|------|
| I.MX 界面离线自检 | `cd ~/CAN && make uitest && ./uitest` | `/tmp/ui_dashboard.ppm`、`/tmp/ui_history.ppm`、`/tmp/ui_grid.ppm` |
| I.MX 界面**整链路**验证 | `cd ~/CAN && ./run_gui_test.sh` | `/tmp/gui_frames/*.ppm`（每帧以 `NN_main` / `NN_curve` / `NN_grid` / `NN_detail` 命名） |
| STM32 界面渲染 | `make -f tools/pc_ui/Makefile run` | `/tmp/stm32_main.ppm`、`/tmp/stm32_curve.ppm`（后者见第 5 节：现在与前者同内容，不是曲线图） |

> 虚拟机上的布局是「扁平」的：`linux_can_monitor/*` 被拷到 `~/CAN/`，
> 所以 `run_gui_test.sh` 会在 `$WORK/`、`$WORK/tools/`、`$WORK/../tools/`
> 等几个位置依次找 `ppm2png.py`。把 `tools/` 一起拷过去最省事。

---

## 7. `lint_c.py` —— 轻量 C 自检（不需要编译器）

正式的编译验证靠虚拟机上的 gcc。但如果虚拟机不在手边（或者只想改完先自查
一遍），改完代码就完全没有保护。这个脚本用纯 Python 做几件最容易被漏掉、
又最容易被编译器抓到的事：

```bash
python tools/lint_c.py User                 # STM32 侧
python tools/lint_c.py linux_can_monitor    # Linux 侧
python tools/lint_c.py User linux_can_monitor
```

| 检查项 | 说明 |
|--------|------|
| 括号配对 | `()` `[]` `{}` 是否配平 —— 手误多写一个花括号是最常见的低级错误 |
| 头文件可达 | 每个 `#include "xxx.h"` 能否在工程的头文件搜索路径里找到 |
| 内部函数完整性 | 所有 `uip_` / `LCD_` / `TP_` / `gui_` … 前缀的调用，是否都有定义或声明（「改名忘改调用点」就靠这条抓） |
| 残留 static | 定义了却没有任何调用点的 static 函数（通常是改名后留下的死代码） |

它会自动把**函数式宏**（`#define LCD_WR_REG(reg) ...`）和标准外设库头文件里的
声明一起收进来，因此不会把 `LCD_WR_REG(...)`、`CAN_Init(...)` 之类误报成
「未定义」。

> **它不是编译器**：类型不匹配、参数个数不对、C89 的「声明必须放在块首」
> 这些只有真正的编译器才看得准。它的定位是「改完先跑一遍」，
> 正式验证仍然是 `pack_syntax.py` + 虚拟机上的 `make -f tools/syntax_check.mk`。

---

## 8. `virtual_touch.py` —— 用 uinput 造一个「虚拟触摸屏」

触摸类问题（点了没反应、坐标偏了、左右反了）只有在真机上才暴露，改一次代码
烧一次板子太慢。Linux 的 `uinput` 允许用户态**创建一个看起来完全像真触摸屏的
输入设备**，再往里灌事件 —— 对 `can_monitor` 来说它和真的 GT1151 没有区别。

这个工具只用得上 I.MX 那头：STM32 端界面已经改成只读仪表盘，没有触摸可点。

```bash
# 终端 A：常驻，创建虚拟触摸屏
sudo python3 tools/virtual_touch.py --create
#   -> 虚拟触摸屏已就绪  event 节点: /dev/input/event7

# 终端 B：拿它跑界面
sudo ./can_monitor -i can0 --gui --no-touch-grab --touch-debug

# 终端 C：点主界面「数据曲线」按钮（参数是逻辑屏幕坐标）
sudo python3 tools/virtual_touch.py --tap 357 14
```

标题栏上并排两个按钮：「数据曲线」占 `x 314..401`，「充电」占 `x 408..471`
（y 都是 `4..23`）。`357 14` 落在「数据曲线」正中。**别再照抄旧文档里的
`428 14`** —— 那个 x 现在落在「充电」里，会点成开始充电（旧版那里是「历史曲线」，
右边还没有「充电」按钮，所以旧文档当时是对的）。

实测效果（真实 evdev 路径，不是模拟分支）：

```
[TOUCH] 设备 /dev/input/event7  名称 "gb27930 virtual touch"
[TOUCH] 原始量程 X[0..34799] Y[0..13064] -> 屏幕 480x272  (多点协议)
[TOUCH-DBG] 事件 #1: type=3 code=57 value=1        ABS_MT_TRACKING_ID
[TOUCH-DBG] 事件 #2: type=3 code=53 value=25875    ABS_MT_POSITION_X
[TOUCH-DBG] 事件 #3: type=3 code=54 value=675      ABS_MT_POSITION_Y
[TOUCH-DBG] 原始(25875,675) → 屏幕(357,14)  down=1 pressed=1
[GUI] 触摸 (357,14) -> 数据曲线
```

**默认原始量程就是照着野火板上那块 Goodix 抄的（34799 × 13064）**，
所以现场遇到的坐标问题可以在 PC 上原样复现、反复验证，不用碰板子。

> 实现上的两个坑，记在这里免得再踩：
> 1. `struct uinput_user_dev` **只有一个 `ff_effects_max` 字段**。写成
>    `80s + HHHH + II` 会多出 4 字节，内核 `write()` 直接返回 `EINVAL`
>    （Ubuntu 18.04 实测 `sizeof` 是 1116）。
> 2. uinput 事件必须写在**创建设备时那个 fd** 上，而 `open("/dev/uinput")`
>    一次就是一台新设备 —— 所以「A 进程创建、B 进程点击」行不通，
>    这里用一个命名管道（`/tmp/virtual_touch.fifo`）把点击指令传给常驻进程。

---

## 9. 改动界面文案 / 布局后的标准流程

```bash
# ① 改 User/Ui/ui_app.c（Keil 侧）或 linux_can_monitor/gui.c（Linux 侧）
#    Keil 侧的文件是 GBK，先用工具转成 UTF-8 才能正常编辑：
python tools/to_gbk.py --to-utf8

# ② 重新生成字库（按 profile 扫描源码里的新文案）
#    注意：两端字库是分开生成的，**必须带 --profile** ——
#    不带就是 "all"（收全部 profile 的并集，**不少于 498 个汉字**，约 19 KB 以上），
#    STM32 那边只有 32 KB 总额度，
#    塞进去会顶爆。STM32 只收 77 字、约 4.19 KB：
python3 tools/gen_font.py --profile stm32 --out-encoding gbk > User/Ui/font16.h
python3 tools/gen_font.py --profile linux > linux_can_monitor/font16.h

# ③ STM32 端：转回 GBK → 自检 → 语法检查
python tools/to_gbk.py
python tools/lint_c.py User                  # 没有编译器时的兜底自检
python tools/pack_syntax.py                  # 打包后到虚拟机上 make -f tools/syntax_check.mk

# ④ 两端界面都渲染出来看一眼
make -f tools/pc_ui/Makefile run             # STM32 界面（PC 渲染；见第 5 节的已知滞后）
cd ~/CAN && make uitest && ./uitest          # I.MX 界面（离线渲染）
cd ~/CAN && ./run_gui_test.sh                # I.MX 界面整链路 + 导出 PNG

# ⑤ PPM 转 PNG 目视检查（PC 渲染产出的是 /tmp/stm32_main.ppm，
#    /tmp/stm32_curve.ppm 与它同内容，见第 5 节）
python3 tools/ppm2png.py /tmp/stm32_main.ppm

# ⑥ 都没问题再烧录 / 拷贝到板子（STM32 端改过头文件必须 Rebuild；I.MX 端源码变了必须重编）

# ⑦ 报文周期已在 2026-10-05 对齐 GB/T 27930-2015，CCS/BEM/CEM 三类报文已实现，
#    并于 2026-10-09 在开发板上重新采过基线（charging 与 idle 两种模式各一轮）；
#    同日又做了「周期任务相对调度 -> 绝对调度」的修复，随后 STM32 也重刷，两端都生效之后
#    补采了最终一轮。bench_board.sh 不预置任何帧率假设，帧率与总线负载都是现场数出来的。
#    实测工作点（充电中，2026-10-09，STM32 已重刷、两端周期调度修复都生效）：
#    51 帧/秒（60 秒 3114 帧）、总线负载 3.11 %、进程 CPU 4.9 %、内存 RSS 2244 KB、
#    数据库增长 8.3 MB/小时（按充电阶段计；三次全新空库实测合计 954,368 字节 / 11,038 行，
#    合 86.5 字节/行，再乘充电阶段入库帧率 27.9 帧/秒：27.9 × 3600 × 86.5 = 8,688,060 字节/小时，
#    8,688,060 ÷ 1024 ÷ 1024 ≈ 8.3 MB/小时，本项目统一按 1024 换算）。
#    ⚠️ 27.9 帧/秒 取自最终实测的 BCL 19.95 + BCS 3.98 + BSM 3.98 = 27.91 帧/秒
#       （60 秒窗口采到 BCL 1197 帧、BCS 与 BSM 各 239 帧），这一项是实测值。
#    ⚠️ 数据库增长不能按"体积 ÷ 时长"折算：握手 / 辨识 / 参数配置 / 结束态都不入库，建表的
#       固定开销又被平摊进时长，同一份代码只换窗口就能算出 5.4 / 8.6 / 8.9 MB/小时三个值
#       （分别出自 300 秒、100 秒、90 秒那三轮）。一律用"字节/行 × 帧率"。
#    报文构成（60 秒内各 ID 帧数）：CCS 1199 帧（50.05 ms）、BCL 1197 帧（50.13 ms）、
#    CRO 240 帧（250.0 ms）、BCS 239 帧（251.0 ms）、BSM 239 帧（251.0 ms），合计 3114 帧。
#    周期一律按 60000 / 帧数 换算，保留两位小数。
#    ⚠️ 采样口径：这一轮是 STM32 重刷、两端都跑绝对调度之后采的，**五类周期报文全部回到标称值**
#       （CCS 52.9 -> 50.05 ms、BCL 50.9 -> 50.13 ms、CRO 253.2 -> 250.0 ms、BCS / BSM 保持 251.0 ms）。
#       **BCS 与 BSM 的 239 帧不是漂移，而是采样窗口边界** —— 计数器窗口略短于 60.000 秒，
#       标称 240 帧的报文在略短的窗口里采到 239 属正常，窗口里两者记录到的就是 240 帧（4.0 Hz）。
#    待机工作点也已上板实测（idle 模式，2026-10-09 最终一轮）：5 帧/秒（60 秒 300 帧）、
#    总线负载 0.30 %、进程 CPU 1.9 %、内存 RSS 2056 KB、缓冲丢弃 0，
#    构成为 BSM 240 帧（4.0 Hz）+ 心跳 60 帧（1.0 Hz）；数据库原始帧 0，
#    「待机遥测不入库」这条策略已在真实硬件上验证。
#    需要新的工作点时重跑：
#    sudo sh tools/bench_board.sh can0 60          # 测充电中
    sudo sh tools/bench_board.sh can0 60 idle     # 测待机
```

---

## 10. `gb27930_layout_check.js` —— 两端报文布局交叉校验

这是本项目**按 GB/T 27930-2015 原文全面修正报文格式时新增的工具**，用来防止两端的
报文定义再次漂移。

```bash
node tools/gb27930_layout_check.js
```

实测输出：

```
  ======================================================================
   布局交叉校验结果:  PASS 166 / FAIL 0
  ======================================================================
```

它分 **6 节**检查，共 **166 项**：

| 节 | 检查内容 | 项数 | 为什么需要它 |
|---|---|---|---|
| `[1/3]` | **两端偏移宏逐条比对** —— 把 STM32 的 `bms_protocol.c/.h`（GBK）与 i.MX 的 `gb27930.c/.h`（UTF-8）里同名报文的偏移、长度、换算常量逐一比对 | 87 | 两端是两份独立实现，最容易出现"改了一边忘了另一边" |
| `[2/3]` | **参考载荷逐字节比对** —— 对每个报文用固定输入算出期望的数据域字节序列，与实现比对 | 13 | 光比宏不够，还要验证"算出来的字节" |
| `[3/3]` | **换算参数与枚举值抽查** —— 优先级、ID、状态位取值等 | 47 | |
| `[反向检查]` | **应当已被删除的旧字段** —— 逐条确认 BCL 的 B6-B8、BSM 的电压字段与旧故障标志字节、BCS 的 `max_single_no`、BRM 的 `model` 等改造前的旧字段真的清干净了 | 13 | 布局改完之后，最怕旧字段在某个角落还留着写或读，那种残留不会被"值比对"发现 |
| `[4/5]` | **编译单元归属检查** —— ① 同名宏是否在多个文件里各定义一次；② 宏是否只定义在 `.c` 里却被别的 `.c` 引用 | 4 | **这一节的由来**：本轮出现过 `GB_READY_INVALID`、`GB_CRM_ID_UNKNOWN` 定义在 `gb27930.c` 内、却被 `selftest.c` 引用导致编译失败；把这个检查固化后，**第一次运行就抓出了同类型的第三个 bug**（新增用例用了 `BCL_LEN`） |
| `[5/5]` | **printf 格式串非法 `%` 转换** —— 用完整的 C99 转换规范匹配，只检查真的走 printf 的调用 | 2 | 同一个文件里出现过 `"…（BCS 是 1 %/位）"` 这种漏写 `%%` 的格式串 |

> **节号与项数是怎么对上的**：脚本的分节标题本身分成两套编号 ——
> 前四节的标题是 `[1/3]` `[2/3]` `[3/3]` `[反向检查]`，后两节是 `[4/5]` `[5/5]`；
> 两套合起来是 6 节、共 166 项。另外 `[1/3]` 的表头声明"94 个宏"，但其中 7 个 `CTS_OFF_*`
> （`CTS_OFF_SECOND` / `MINUTE` / `HOUR` / `DAY` / `MONTH` / `YEAR_LO` / `YEAR_HI`）
> **只有 STM32 侧有** —— i.MX 端用 `gb27930_cts_pack()` 按 `struct tm` 直接拼包，
> 不定义这组偏移宏。这 7 个不参与比对（脚本会打印一行
> `[note] 仅 STM32 有的宏: …`），所以 `[1/3]` 实际计入 PASS 的是 **87** 项。
> `[4/5]` 与 `[5/5]` 在两端各跑一遍，所以是 2 × 2 = 4 项与 2 × 1 = 2 项。
> 六节相加：**87 + 13 + 47 + 13 + 4 + 2 = 166**。

**它检查不了什么**（这是边界，必须说清）：**语法错误、类型错误、`-Wunused` 告警、链接期符号**
—— 这些只有真正编译才能发现。**它 PASS 不代表能编过。**

STM32 端的 GBK 文件用 Node 的 `TextDecoder('gbk')` 读取，不需要先转编码。