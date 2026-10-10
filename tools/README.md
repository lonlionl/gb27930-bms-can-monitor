# tools/ —— 开发期工具

`tools/` 存放开发期使用的脚本、构建片段与标准对照文档，共 17 个文件，用于源码编码统一、静态
检查、字库生成、界面渲染与板上基准，不参与固件与终端程序的构建，也不编进交付产物。

## 1. 工具清单

| 文件 | 作用 |
|------|------|
| `README.md` | 本文件：组成、命令、依赖与判据 |
| `GB_T_27930_报文格式定义.md` | 从标准扫描件逐字核读整理的逐报文字段定义 |
| `GB_T_27930_标准核对基准.md` | 标准原文抄录与工程实现的核对基准 |
| `to_gbk.py` | STM32 侧源码的编码与换行统一，报告 BOM 与纯 LF 行 |
| `lint_c.py` | 纯 Python 的轻量 C 检查，不需要编译器 |
| `pack_syntax.py` | 打包 STM32 语法检查包 `stm32_syntax.tar.gz` |
| `syntax_check.mk` | 用本机 gcc 对 12 个 STM32 源文件做 `-fsyntax-only` 检查 |
| `gb27930_layout_check.js` | 两端报文布局交叉校验，166 项 |
| `gen_font.py` | 生成 LCD 点阵字库头文件 `font16.h` |
| `pc_ui/Makefile` | PC 侧界面预览的构建脚本 |
| `pc_ui/pc_ui.c` | PC 侧 `ui_port` 实现：480×320 虚拟屏、桩协议数据、渲染与字库覆盖检查 |
| `pc_ui/bms_protocol.h` | `BMS_UiSnapshot_t` 等公开接口的桩，供 `ui_app.c` 在 PC 上编译 |
| `pc_ui/stm32f10x.h` | 寄存器类型别名桩（`u8`/`u16`/`u32`/`s8`/`s16`/`s32`） |
| `ppm2png.py` | 把 PPM 帧转成 PNG，支持批量与整数倍放大 |
| `virtual_touch.py` | 用 uinput 创建虚拟触摸屏，经命名管道注入触摸事件 |
| `deploy_linux.sh` | 把 Linux 端源码平铺到部署目录并编译、自检 |
| `bench_board.sh` | I.MX6ULL 板上的性能基准，充电与待机两种工况 |

## 2. 目录约定

- STM32 侧源码（`User/**`）**含中文的文件都是 GBK 编码**；Linux 侧源码（`linux_can_monitor/**`）与 `tools/` 下的脚本为 UTF-8。
- 换行**不统一**：`to_gbk.py` 以 **GBK + CRLF** 作为它覆盖的那 12 个文件的目标状态。以 `User/` 下 24 个 `.c/.h` 计，
  纯 CRLF 18 个、纯 LF 6 个（`bms_protocol.h`、`can.c`、`can.h`、`main.c`、`stm32f10x_it.c`、`User/Ui/font16.h`）。
  编码与换行是两个独立维度：21 个文件含中文（GBK，其中 15 个是 CRLF、6 个仍是 LF；`to_gbk.py` 覆盖的
  那 12 个里 11 个已收进 CRLF，只剩 `User/Ui/font16.h` 是 LF，见 3 节），
  3 个是纯 ASCII + CRLF（`stm32f10x_conf.h`、`stm32f10x_it.h`、`bsp_led.h`）。
- 点阵字库分两份：`User/Ui/font16.h`（GBK，Keil 工程）与 `linux_can_monitor/font16.h`（UTF-8），由 `gen_font.py` 的不同档位分别生成，两者不可互换。
- 不进仓库的内容：`stm32_syntax.tar.gz`、`tools/_gbt_pages/`、`__pycache__/`、`_tmp_*`、`_tmp/`、`*.ppm`、`*.db`、`*.log`、`linux_can_monitor/build/` 等编译与运行产物。

## 3. 编码统一：`to_gbk.py`

处理 `User/Ui/` 下的 `ui_app.c/.h`、`ui_port.c/.h`、`font16.h` 与 `User/Lcd/` 下的 `lcd.c/.h`、
`ili93xx.c`、`touch.c/.h`、`ctiic.c/.h`，共 12 个文件。

```bash
python tools/to_gbk.py            # UTF-8 -> GBK，并统一成 CRLF
python tools/to_gbk.py --to-utf8  # GBK -> UTF-8，便于用普通编辑器改中文
python tools/to_gbk.py --check    # 只报告当前编码与换行，不改动文件
```

`--check` 逐文件打印编码名、CRLF 行数与纯 LF 行数，凡带 UTF-8 BOM、含纯 LF 行、编码不是 GBK 三者
之一成立即返回 1。默认方向与 `--to-utf8` 把内容统一成 CRLF 后按目标编码写回并打印字节数变化，
编码无法表示某字符时逐条列出并返回 1。

**当前实际读数：这 12 个文件里 11 个是 GBK + CRLF、报 `CRLF=n LF=0`，唯一的例外是
`User/Ui/font16.h` —— 它是纯 LF（552 行），所以 `python tools/to_gbk.py --check` 现在返回 1：**

```
User/Ui/font16.h         GBK    CRLF=0    LF=552  <-- 含 552 个纯 LF
```

这不是有意保留的例外。`User/Ui/font16.h` 由 `gen_font.py` 的输出重定向生成，而仓库里现存的这一份
是 `gen_font.py` 改成 CRLF 输出**之前**产出的旧产物，一直没有重新生成。要让 `--check` 返回 0，
下面两种方式都可以，但改的东西不同：

**方式 A —— 只改行尾（不动字模内容）**

```bash
python tools/to_gbk.py
```

`User/Ui/font16.h` 在这份工具的 12 个文件名单里，默认方向就是「UTF-8 -> GBK + CRLF」；它已经是
GBK，所以只有那 552 行 LF 被换成 CRLF，字符数据一字不改，之后 `python tools/to_gbk.py --check`
直接返回 0。不需要 Linux，也不需要字体。

**方式 B —— 重新生成字库内容（行尾随之变成 CRLF）**

```bash
python3 tools/gen_font.py --profile stm32 --out-encoding gbk > User/Ui/font16.h
```

`gen_font.py` 现在输出 CRLF（见 5 节），所以重新生成的这一份也是 CRLF。这条路径重新生成**字模
内容**，需要一台装了 Noto / DejaVu 字体的 Linux。

两者的区别：A 只动行尾、不动字模，输出字节数只差那 552 个 `\r`；B 会按当前脚本文案重新生成字模
内容，字号、字符集合或字模压缩逻辑变化时字节数会明显不同。

在清零之前，`--check` 返回 1 是这份旧产物的已知现状，不是编码统一失败，其余 11 个文件的
编码与换行都已到位。

## 4. 静态检查

### 4.1 `lint_c.py`

`python tools/lint_c.py` 默认检查 `User` 与 `linux_can_monitor`，也可只给一个或多个目录，例如
`python tools/lint_c.py User`。四项检查：`()` `[]` `{}` 是否配平（注释与字符串字面量不参与计数）；
每个 `#include "xxx.h"` 能否在搜索路径（`User` 各子目录、`linux_can_monitor`、`tools/pc_ui`）里
找到；带项目前缀（`uip_`、`UI_`、`LCD_`、`BMS_`、`gb27930_`、`gui_` 等）的调用是否都有定义或
声明；定义了却没有任何调用点的 static 函数。函数式宏与标准外设库头文件里的声明一并计入，因此
`LCD_WR_REG(...)`、`CAN_Init(...)` 不会误报为未定义。输出分「括号 / 头文件」「项目内部函数」
「static 函数」三段，末尾给出问题数与提示数，问题数非 0 时返回 1；类型、参数个数与 C89 声明位置
不在检查范围内。

### 4.2 `pack_syntax.py` 与 `syntax_check.mk`

`python tools/pack_syntax.py` 生成 `<仓库根>/stm32_syntax.tar.gz`，内容为 `Libraries/CMSIS`、
`Libraries/STM32F10x_StdPeriph_Driver/inc`、`User/**` 与 `tools/syntax_check.mk`；`User/Ui/font16.h`
被排除，改用 `linux_can_monitor/font16.h` 放在同一路径（该文件是 UTF-8，GBK 双字节里的 `0x5C` 不会
被 gcc 当作转义）。解包后执行 `make -f tools/syntax_check.mk`，该规则对 `SRCS` 里的 12 个文件逐个
执行 `gcc -std=gnu90 -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable
-DUSE_STDPERIPH_DRIVER -DSTM32F10X_HD -fsyntax-only`：全部通过时打印 `== 全部源文件语法检查通过 ==`
并返回 0，任一文件报错则打印 `!! 存在语法/类型错误，见上方输出 !!` 并返回 1。依赖 gcc 与 make。

### 4.3 `gb27930_layout_check.js`

`node tools/gb27930_layout_check.js`，无命令行参数。校验对象是两份实现：`User/Bms/bms_protocol.c/.h`
（GBK，用 Node 的 `TextDecoder('gbk')` 读取）与 `linux_can_monitor/gb27930.c/.h`；标准原文的字段
偏移、长度、分辨率与枚举值取自 `GB_T_27930_报文格式定义.md`，`linux_can_monitor/fake_bms.py` 的
模拟器算法作为对照。六节共 166 项，实测 `PASS 166 / FAIL 0`，返回 0；有不一致时逐条打印 `[FAIL]`
行并返回 1。

项数构成为：两端偏移宏比对 87、按标准原文生成的参考载荷逐字节比对 13、换算参数与枚举值抽查 47、
旧字段反向检查 13、编译单元归属（宏重复定义、只定义在 `.c` 里却被其它 `.c` 引用）4、printf 格式串
非法 `%` 转换 2。不做语法与类型检查，不看 `-Wunused` 类告警与链接期符号，通过不代表能编译；依赖
Node.js（`TextDecoder('gbk')` 需要完整 ICU，Node 14 及以后的官方构建默认包含）。

## 5. 字库生成：`gen_font.py`

```bash
python3 tools/gen_font.py --profile stm32 --out-encoding gbk > User/Ui/font16.h
python3 tools/gen_font.py --profile linux > linux_can_monitor/font16.h
python3 tools/gen_font.py --art "电压0125."     # 终端里打印 ASCII 艺术字
python3 tools/gen_font.py --preview /tmp/p.png  # 另外输出一张放大预览图
```

`--profile stm32` 只扫 `User/Ui/ui_app.c` 与 `User/Ui/ui_app.h`，再补上 `UI_DYNAMIC_TEXT_STM32`
列出的界面动态文案；`--profile linux` 扫 `linux_can_monitor/` 下会画到屏上的 13 个文件（`.c` 与 `.h`）；
不带 `--profile` 等同 `all`，取两份扫描范围的并集；`--out-encoding gbk` 把标准输出切到 GBK，便于直接
重定向进 Keil 工程。

写往标准输出的头文件正文一律以 **CRLF** 结束（脚本内 `ENDL`）：**重新生成**出来的 `font16.h` 因此与
Keil 工程里其它源码的换行一致，`to_gbk.py --check` 也不会因纯 LF 行返回 1。注意这只对重新生成的文件
成立 —— 仓库里现存的 `User/Ui/font16.h` 是改 `ENDL` 之前生成的，仍是纯 LF、`--check` 仍返回 1
（读数见 3 节）。**行尾与字模内容是两个独立的问题**：只想把行尾改成 CRLF、不重新生成字模，走 3 节的
方式 A（`python tools/to_gbk.py`），不需要字体。`--art` 是终端自检、不进头文件，仍按平台默认行尾输出。

字模格式：16×16，每字 32 字节，每行 2 字节，最高位对应最左像素；ASCII 为 0x20~0x7E 共 95 个，只画
左侧 8 列、绘制步进 8；汉字用满 16 列、绘制步进 16。`--profile stm32` 下 ASCII 字模压缩为每字
16 字节（`FONT_ASC_ROWBYTES 1u`），并输出由 `#ifdef FONT_WITH_GBK_MAP` 包住的 `g_font_gbk_map[]`
（GBK 双字节码 → 字模下标，按 GBK 升序，可二分查找）。字符来源为源码扫描，只取字符串字面量里的
非 ASCII 字符，注释里的汉字不计入；扫描无结果时退回 `CJK_TEXT` 表并告警。

当前 `User/Ui/font16.h` 为 `FONT_CJK_COUNT 77u`、`FONT_ASC_BYTES 16u`、`FONT_CJK_BASE 1520u`、
`FONT_GBK_COUNT 77u`；`linux_can_monitor/font16.h` 为 `FONT_CJK_COUNT 498u`、`FONT_ASC_BYTES 32u`、
`FONT_CJK_BASE 0u`、`FONT_GBK_COUNT 497u`。

依赖 Python 3 与 Pillow（`sudo apt-get install -y python3-pil`）以及一套中文字体：脚本按序查找
`NotoSansCJK-Regular/Medium.ttc`（`fonts-noto-cjk`）、`DroidSansFallbackFull.ttf`
（`fonts-droid-fallback`）、`uming.ttc`、`wqy-zenhei.ttc`，ASCII 字模的字体查找 DejaVu Sans Mono、
Liberation Mono、Ubuntu Mono、FreeMono；中文字体缺失时打印告警并返回 1。

## 6. 界面渲染

### 6.1 `pc_ui/`

```bash
make -f tools/pc_ui/Makefile         # 编译到 build/pc_ui
make -f tools/pc_ui/Makefile run     # 编译并运行
make -f tools/pc_ui/Makefile clean   # 清理 build/pcui_*.o 与 build/pc_ui
```

编译选项为 `-std=gnu99 -Wall -Wextra -Wno-unused-parameter -O1 -g -Itools/pc_ui -IUser/Ui`，链接
`-lm`；`tools/pc_ui/pc_ui.c` 按 UTF-8 编译，`User/Ui/ui_app.c` 加 `-finput-charset=GBK
-fexec-charset=UTF-8`，即按 GBK 读入、按 UTF-8 生成字符串字面量。运行过程与产物：

1. 灌入 300 条 1 秒间隔的历史采样，渲染主界面，打印 ASCII 字符画并导出 `/tmp/stm32_main.ppm`；
2. 经 `pc_ui.c` 自身的 `uip_touch_read()` 桩注入合成触摸 (413,17) 并推进 100 ms，再渲染一次并导出
   `/tmp/stm32_curve.ppm`，随后注入 (200,200) 的空白触摸；
3. 对 5 组「状态 × 异常码」组合各推进 1 秒，结果写入 `/tmp/stm32_states.txt`；
4. 打印「字库覆盖检查」，绘制过程中查不到字模的字符逐个报为 `[缺字] U+XXXX`，出现缺字时退出码为 1。

`ui_app.c` 不调用 `uip_touch_read()`，`UI_IsCurveScreen()` 恒返回 0，因此第 2 步渲染的仍是主界面，
`/tmp/stm32_curve.ppm` 与 `/tmp/stm32_main.ppm` 内容相同。`tools/pc_ui/bms_protocol.h` 是
`User/Bms/bms_protocol.h` 公开部分的镜像，其中的 `BMS_UiSnapshot_t` 必须与后者逐字段一致。

### 6.2 `ppm2png.py`

`python3 tools/ppm2png.py <目录或文件> [更多...]` 转换全部输入，也可写成
`python3 tools/ppm2png.py /tmp/ui_dashboard.ppm` 或
`python3 tools/ppm2png.py /tmp/gui_frames --scale 2`。目录参数下的 `*.ppm` 与 `*.pnm` 全部转换，
输出与源文件同名的 `.png`；`--scale N` 用最近邻放大 N 倍（仅 N>1 时生效）。逐文件打印
「源名 -> 目标名 宽x高」，末尾打印转换总数；没有找到任何 PPM 文件时返回 1，不带路径参数时打印用法
并返回 2。依赖 Python 3 与 Pillow。

产出 PPM 的三个来源：I.MX 界面离线渲染 `cd ~/CAN && make uitest && ./uitest` 生成
`/tmp/ui_dashboard.ppm`、`/tmp/ui_history.ppm`、`/tmp/ui_grid.ppm`；I.MX 界面整链路
`cd ~/CAN && ./run_gui_test.sh` 生成 `/tmp/gui_frames/*.ppm`（帧名形如 `NN_main`、`NN_curve`、
`NN_grid`、`NN_detail`），转出的 PNG 复制到 `/tmp/gui_frames_png/`；STM32 界面由
`make -f tools/pc_ui/Makefile run` 生成 `/tmp/stm32_main.ppm` 与 `/tmp/stm32_curve.ppm`。
`run_gui_test.sh` 在 `$WORK/`、`$WORK/tools/`、`$WORK/../tools/`、`$WORK/../../tools/` 四处依次查找
`ppm2png.py`，把 `tools/` 一并复制到部署目录即可命中第一处。

### 6.3 `virtual_touch.py`

```bash
sudo python3 tools/virtual_touch.py --create                   # 终端 A：常驻
sudo ./can_monitor -i can0 --gui --no-touch-grab --touch-debug # 终端 B：界面
sudo python3 tools/virtual_touch.py --tap 357 14               # 终端 C：点主界面「数据曲线」
```

`--create` 创建屏幕并常驻到 Ctrl+C；`--name` 指定设备名（默认 `gb27930 virtual touch`）；
`--raw-x-max` / `--raw-y-max` 指定原始量程上限（默认 34799 / 13064）；`--screen` 指定逻辑屏幕
（默认 `480x272`），用于屏幕坐标到原始坐标的换算。

`--tap X Y` 送**屏幕坐标**：先把屏幕坐标按上面那组量程换算成原始值，再把原始值写进管道；`--raw-tap
X Y` 送**原始坐标**，只写入、不换算 —— 用来直接灌设备原始值，复现「内核上报量程与实际值不符」
（野火板上那块 Goodix 内核声称 34799 x 13064、设备却按屏幕像素上报）。`--raw-mode` 配合 `--create`
用，把管道里的数值一律当原始值 —— 管道是唯一输入通道，常驻进程无法知道写入方用的是哪一组参数，
所以默认按屏幕坐标解释；确实要整条管道都灌原始值时用 `--raw-tap`，或在 `--create` 时加 `--raw-mode`。

`--create` 打开 `/dev/uinput`，注册 `EV_SYN`、`EV_KEY`、`EV_ABS` 与 `BTN_TOUCH`、`ABS_X/Y`、
`ABS_MT_SLOT`、`ABS_MT_POSITION_X/Y`、`ABS_MT_TRACKING_ID`，把 event 节点名写入
`/tmp/virtual_touch.node`，然后阻塞读命名管道 `/tmp/virtual_touch.fifo`，每读到一对数值就发一组
按下 + 抬起事件；结束时销毁设备并删除管道与节点文件。

坐标换算与 `can_monitor` 的 `map_axis()` 同一套线性映射：`round(sx × raw_max / (屏幕边长 − 1))`，
故屏幕像素 `sx` 对应的原始值为 `round(sx × 34799 / 479)`，`sy` 对应 `round(sy × 13064 / 271)`。
标题栏按钮中心的取值：主界面「数据曲线」屏幕 (357,14) → 原始 (25936,675)，「充电」(439,14) →
(31893,675)，数据曲线页「历史」(366,14) → (26590,675)、「返回」(439,14) → (31893,675)
（按钮矩形取自 `linux_can_monitor/gui.c` 的 `gui_button_rect()`）。运行要求 root 权限、存在
`/dev/uinput`（`sudo modprobe uinput`）与 Python 3。

## 7. 部署与板上基准

### 7.1 `deploy_linux.sh`

`bash tools/deploy_linux.sh` 部署到 `~/CAN`，`bash tools/deploy_linux.sh /root/CAN` 部署到指定目录。
脚本按自身路径定位仓库根目录，源码取 `linux_can_monitor/`，执行三步：把 `*.c`、`*.h`、`Makefile`、
`*.sh`、`*.py` 平铺复制到目标目录（只覆盖同名文件，不删除目标目录里的其它内容）；`make clean` 后
`make`；做三项部署自检并打印计数——`main.c` 含 `--no-gui`（应大于 0）、`main.c` 含 `V("[1/5]`
（应大于 0）、`gb27930.c` 含旧 ID 列表串（应为 0）。三项全部满足时打印 `部署目录是最新代码 [OK]`
并返回 0，否则返回 2；依赖目标机上的 bash、make 与 gcc。

### 7.2 `bench_board.sh`

`cd ~/CAN && make` 之后，`sudo sh tools/bench_board.sh can0 60` 测充电工况（默认），
`sudo sh tools/bench_board.sh can0 60 idle` 测待机工况。位置参数依次为 CAN 接口（默认 `can0`）、
采样秒数（默认 `60`）、工况（默认 `charging`）。`charging` 给 `can_monitor` 加 `--auto-start`，由
程序自行发起充电后在充电过程中采样；`idle` 不加该参数，两端停在空闲态。要求 root、当前目录下有已
编译的 `./can_monitor`、CAN 总线连通且 STM32 已上电。

输出按【0】~【7】分段：环境信息（内核、CPU、内存、`CLK_TCK`）、接口配置（250 kbit/s、
`restart-ms 100`）、进程启动与当前状态、连续 N 秒的帧数与平均帧率及总线负载（按每帧约 150 位估算，
另列各 ID 帧数前 12）、进程 CPU 与内存 RSS、优雅退出后的程序统计、SQLite 记录数与全表扫描耗时、
系统负载与 TF 卡顺序写。`candump` 或 `sqlite3` 缺失时对应段落打印 `[跳过]`，中间文件为
`/tmp/_bench.db` 与 `/tmp/_bench_run.log`。

## 8. 标准对照文档与检查读数

`GB_T_27930_报文格式定义.md` 是字段级定义的出处：22 类报文的总表（PGN、优先权、长度、周期、方向、
完整 29 位 ID、标准页码）、29 位标识符的拼装公式与逐报文字段定义，随附页图放在 `tools/_gbt_pages/`。
`GB_T_27930_标准核对基准.md` 是条款级的对照：标准原文的 29 位标识符构成、PGN、优先权、长度、周期、
字节序、填充规则与超时规定；当前实现的状态以 `User/Bms/bms_protocol.h`、`linux_can_monitor/gb27930.h`
里的 `GB_ID_*` 与布局宏，以及 `gb27930_layout_check.js` 的读数为准。

| 命令 | 读数 |
|------|------|
| `cd ~/CAN && make selftest && ./selftest` | `PASS 347 / FAIL 0` |
| `cd ~/CAN && ./run_e2e_test.sh` | `PASS 19 / FAIL 0` |
| `cd ~/CAN && ./run_gui_test.sh` | `PASS 15 / FAIL 0` |
| `node tools/gb27930_layout_check.js` | `PASS 166 / FAIL 0` |
| `python tools/lint_c.py` | `通过（0 个问题，0 个提示）`，50 个文件 |
| `make -f tools/syntax_check.mk` | `== 全部源文件语法检查通过 ==` |

前四条对应协议层离线自检、vcan 端到端联调、界面整链路验证与两端布局交叉校验，读数与
`linux_can_monitor/README.md` 的记录一致。仓库总览见 `README.md`，构建、部署与运维说明见
`项目总使用手册.md`，STM32 端模拟器的使用说明见 `STM32_BMS模拟器使用手册.md`，I.MX 端终端程序的
参数说明见 `linux_can_monitor/README.md`。
