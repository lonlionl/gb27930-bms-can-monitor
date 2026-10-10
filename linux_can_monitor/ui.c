/**
 * @file    ui.c
 * @brief   终端监控界面实现（ANSI 彩色仪表盘）
 *
 * 刷新策略
 * --------
 *   每帧先用 "\033[H" 把光标移回左上角，再整屏重绘，最后用 "\033[J"
 *   清除残留内容。这样既没有滚屏，也不会有闪烁。
 *   所有输出先攒进一个缓冲区再一次 write()，避免多线程环境下
 *   printf 交错导致画面撕裂。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ui.h"

#include <string.h>

extern int g_verbose;   /* 由 main.c 定义：1 = 输出逐报文调试日志 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>

/*==============================================================================
 *                              常量
 *============================================================================*/

#define UI_LINE_CHAR   '-'
#define UI_BAR_WIDTH   20       /* 进度条字符数 */

/*==============================================================================
 *                              内部状态
 *============================================================================*/

static int s_ui_inited = 0;
static int s_term_width = 80;
static int s_term_rows  = 0;      /* 0 = 未知 */
static int s_dashboard_on = 1;    /* 0 = 已降级为滚动日志模式 */
static int s_compact = 0;         /* 1 = 终端较矮，省略分隔线 */

/*------------------------------------------------------------------------------
 * 事件环形缓冲
 *
 * 仪表盘模式下**不能**直接把日志 printf 到终端 —— 仪表盘每秒会用
 * "\033[H" 把光标拉回左上角整屏重绘，两者交替输出就会互相踩踏，
 * 在 Mobaxterm / 串口终端上表现为画面撕裂、内容重叠。
 * 因此这里把日志收进环形缓冲，由 ui_render() 统一画在仪表盘底部；
 * 只有在滚动日志模式（--no-ui）下才直接打印。
 *----------------------------------------------------------------------------*/
#define UI_EVENT_LINES   6
#define UI_EVENT_COMPACT 3    /* 紧凑布局下只显示最近 3 条，省出 3 行高度 */
static char s_events[UI_EVENT_LINES][220];
static int  s_event_total = 0;

/*==============================================================================
 *                              工具函数
 *============================================================================*/

/** 查询终端宽度与高度 */
static void detect_term_width(void)
{
    struct winsize ws;

    memset(&ws, 0, sizeof(ws));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
    {
        if (ws.ws_col > 0)
        {
            s_term_width = ws.ws_col;
        }
        if (ws.ws_row > 0)
        {
            s_term_rows = ws.ws_row;
        }
    }
    if (s_term_width < 70)
    {
        s_term_width = 70;
    }
    if (s_term_width > 140)
    {
        s_term_width = 140;
    }

    /* 终端只有 24 行这种典型串口终端放不下完整仪表盘，
     * 少于 UI_COMPACT_ROWS 行时连分隔线也省掉。 */
    if (s_term_rows > 0 && s_term_rows < UI_FULL_ROWS)
    {
        s_compact = 1;
    }
}

/** 输出一条水平分隔线（紧凑模式下直接跳过） */
static void put_line(char *buf, size_t cap, size_t *pos)
{
    int i;
    int n = s_term_width;

    if (s_compact)
    {
        return;
    }

    for (i = 0; i < n && *pos + 1 < cap; i++)
    {
        buf[(*pos)++] = UI_LINE_CHAR;
    }
    if (*pos + 1 < cap)
    {
        buf[(*pos)++] = '\n';
    }
}

/** 追加格式化文本到输出缓冲 */
static void put_fmt(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*pos >= cap)
    {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        *pos += (size_t)n;
        if (*pos > cap)
        {
            *pos = cap;
        }
    }
}

/**
 * @brief  追加一个带颜色阈值的进度条
 * @param  value/max 当前值与上限
 * @param  warn_ratio 超过该比例显示黄色
 * @param  crit_ratio 超过该比例显示红色
 */
static void put_bar(char *buf, size_t cap, size_t *pos,
                    double value, double max,
                    double warn_ratio, double crit_ratio)
{
    double ratio = (max > 0.0) ? (value / max) : 0.0;
    int    filled;
    const char *color;
    int    i;

    if (ratio < 0.0)
    {
        ratio = 0.0;
    }
    if (ratio > 1.0)
    {
        ratio = 1.0;
    }
    filled = (int)(ratio * UI_BAR_WIDTH + 0.5);

    if (ratio >= crit_ratio)
    {
        color = UI_C_RED;
    }
    else if (ratio >= warn_ratio)
    {
        color = UI_C_YELLOW;
    }
    else
    {
        color = UI_C_GREEN;
    }

    put_fmt(buf, cap, pos, "%s[", color);
    for (i = 0; i < UI_BAR_WIDTH; i++)
    {
        put_fmt(buf, cap, pos, "%c", (i < filled) ? '#' : '.');
    }
    put_fmt(buf, cap, pos, "]%s %5.1f%%", UI_C_RESET, ratio * 100.0);
}

/** 把秒数格式化成 HH:MM:SS */
static void fmt_hms(double seconds, char *out, size_t cap)
{
    long s = (long)seconds;
    if (s < 0)
    {
        s = 0;
    }
    snprintf(out, cap, "%02ld:%02ld:%02ld", s / 3600, (s % 3600) / 60, s % 60);
}

/** 把字节数格式化成人类可读 */
static void fmt_size(uint64_t bytes, char *out, size_t cap)
{
    if (bytes >= 1024ull * 1024ull * 1024ull)
    {
        snprintf(out, cap, "%.2f GB", (double)bytes / 1073741824.0);
    }
    else if (bytes >= 1024ull * 1024ull)
    {
        snprintf(out, cap, "%.2f MB", (double)bytes / 1048576.0);
    }
    else if (bytes >= 1024ull)
    {
        snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
    }
    else
    {
        snprintf(out, cap, "%llu B", (unsigned long long)bytes);
    }
}

/*==============================================================================
 *                              生命周期
 *============================================================================*/

int ui_init(void)
{
    /* ① 标准输出不是终端（被重定向到文件 / 管道 / tee）时绝不画仪表盘。
     *    否则那些 "\033[H" 光标定位序列会被原样写进日志文件，
     *    用 cat / less 看就是一堆乱码。 */
    if (!isatty(STDOUT_FILENO))
    {
        s_dashboard_on = 0;
        s_ui_inited    = 0;
        printf("[提示] 标准输出不是终端（已重定向到文件或管道），"
               "已切换为滚动日志模式。\n\n");
        return 0;
    }

    detect_term_width();

    /* ② 终端太矮就自动降级为滚动日志模式。
     *    硬画一个比终端还高的仪表盘，配合每秒一次的 "\033[H" 重绘，
     *    在串口/Mobaxterm 上只会得到滚动错乱的画面。 */
    if (s_term_rows > 0 && s_term_rows < UI_MIN_ROWS)
    {
        /* 安静降级：终端太矮就直接用滚动日志模式，不再打印提示。
         * 提示本身也是噪音 —— 要用仪表盘的人本来就会把窗口拉高。 */
        s_dashboard_on = 0;
        s_ui_inited    = 0;
        return 0;
    }

    s_dashboard_on = 1;

    /* 清屏 + 隐藏光标 */
    fputs("\033[2J\033[H\033[?25l", stdout);
    fflush(stdout);
    s_ui_inited = 1;
    return 1;
}

/** 当前是否处于仪表盘模式（供 main 判断是否需要额外提示） */
int ui_is_dashboard(void)
{
    return s_dashboard_on;
}

void ui_shutdown(void)
{
    if (!s_ui_inited)
    {
        return;
    }
    fputs(UI_C_RESET "\033[?25h\n", stdout);   /* 显示光标 + 复位颜色 */
    fflush(stdout);
    s_ui_inited = 0;
}

void ui_banner(const char *version, const char *ifname, uint32_t bitrate,
               const char *db_path, int filter_on, int loopback)
{
    printf(UI_C_BOLD UI_C_CYAN);
    printf("========================================================================\n");
    printf("  GB/T 27930-2015  电动汽车非车载充电机与 BMS 通信协议\n");
    printf("  BMS CAN 监控主终端 (充电机侧)     版本 %s\n", version ? version : "1.0.0");
    printf("  Target: I.MX6ULL Pro / Debian / SocketCAN\n");
    printf("========================================================================\n");
    printf(UI_C_RESET);
    printf("  CAN 接口    : %s\n", ifname);
    printf("  总线波特率  : %u bps  (%s)\n", bitrate,
           (bitrate == 250000u) ? "GB/T 27930 标准值" : "非标准值，请确认");
    printf("  硬件滤波    : %s\n", filter_on ? UI_C_GREEN "已启用（只放行 BMS 侧报文）" UI_C_RESET
                                           : UI_C_YELLOW "已关闭（接收全部报文）" UI_C_RESET);
    printf("  回环模式    : %s\n", loopback ? UI_C_YELLOW "开启（自发自收，单板自测）" UI_C_RESET
                                           : "关闭（正常总线模式）");
    printf("  数据库      : %s\n", db_path);
    printf("  启动时间    : %s", ctime(&(time_t){ time(NULL) }));
    printf("========================================================================\n");
    fflush(stdout);
}

void ui_usage(const char *prog)
{
    printf("\n用法: %s [选项]\n\n", prog);
    printf("  -i, --interface <name>   CAN 接口名，默认 can0\n");
    printf("  -b, --bitrate <bps>      总线波特率，默认 250000 (GB/T 27930)\n");
    printf("  -d, --database <path>    SQLite 数据库路径，默认 ./gb27930_data.db\n");
    printf("  -l, --loopback           打开 CAN 回环模式（单板自测用）\n");
    printf("  -n, --no-config          不通过 netlink 修改接口配置（只用现有配置）\n");
    printf("  -a, --accept-all         关闭硬件滤波，接收全部报文（调试用）\n");
    printf("  -D, --dump               抓包模式：只打印报文，不驱动充电状态机\n");
    printf("  -u, --no-ui              关闭仪表盘，只输出滚动日志（便于 SSH 记录）\n");
    printf("  -r, --record <sec>       只运行指定秒数后自动退出（自动化测试用）\n");
    printf("  -s, --status             打印总线状态与统计后退出\n");
    printf("  -v, --verbose            输出详细过程（逐条报文的收发与解析、\n");
    printf("                           触摸命中、启动各阶段），排查问题时用\n");
    printf("      --gui                在本地屏（framebuffer）上显示图形界面\n");
    printf("                           （默认就开；下面 --no-gui 可关掉）\n");
    printf("      --no-gui             关闭本地屏界面，只留终端仪表盘\n");
    printf("      --auto-start         启动后立即开始充电流程，不必手点屏上的\n");
    printf("                           「充电」按钮（自动化测试与无人值守时用）\n");
    printf("      --fb <dev>           framebuffer 设备，默认 /dev/fb0；\n");
    printf("                           传 virtual 使用内存虚拟屏（无屏幕时验证界面用）\n");
    printf("      --touch <dev>        触摸设备，默认自动查找 /dev/input/event*；\n");
    printf("                           传 demo 使用脚本化触摸事件（无触摸硬件时自检用）\n");
    printf("      --gui-rec <dir>      把渲染的每一帧导出到目录（PPM，便于目视检查）\n");
    printf("      --touch-grab         独占抓取触摸设备（EVIOCGRAB）\n");
    printf("                           检测到桌面（X11）时会自动启用，无需手动加\n");
    printf("      --no-touch-grab      即使检测到桌面也不自动抓取（不想让本程序抢占触摸时用）\n");
    printf("      --touch-debug        打印触摸原始事件与映射结果（只走日志，不动屏幕画面）\n");
    printf("                           「点了没反应」时第一个该用的开关\n");
    printf("      --touch-swap         交换触摸 X/Y（横向拖动光标却竖着走时用）\n");
    printf("      --touch-mirror-x     X 轴镜像（左右反了）\n");
    printf("      --touch-mirror-y     Y 轴镜像（上下反了）\n");
    printf("      --touch-calib <xmin,xmax,ymin,ymax>\n");
    printf("                           手动指定触摸原始量程（内核上报不准时用）\n");
    printf("                           例：--touch-calib 0,480,0,272\n");
    printf("      --version            打印版本、构建时间与已编入的能力\n");
    printf("  -h, --help               显示本帮助\n\n");
    printf("示例:\n");
    printf("  %s -i can0 -b 250000 -d /root/CAN/gb27930.db\n", prog);
    printf("  %s --dump -i can0                # 只监听打印，不主动发报文\n", prog);
    printf("  %s -l -i can0                    # 回环自测\n", prog);
    printf("  %s --gui -i can0                 # 本地屏显示实时监控 + 历史曲线\n", prog);
    printf("  %s --gui --touch-grab -i can0    # 桌面还在跑时，抢回触摸\n", prog);
    printf("  %s --gui --fb virtual --touch demo --gui-rec /tmp/frames -i can0 -r 12\n"
           "                                   # 无屏幕/无触摸时验证整条界面链路\n\n", prog);
}

/*==============================================================================
 *                              事件日志
 *============================================================================*/

/**
 * 逐报文级别的日志标签。
 *
 * 这几类日志在握手/充电阶段每秒会冒出好几条（BHM 每 250 ms 一条、
 * BCL/BCS/BSM 各一条、外加每帧的原始数据），串口完全是刷屏状态，
 * 真正有用的状态迁移和异常反而被冲没了。
 *
 * 产品形态下默认只保留 STATE / ERROR / WARN / INFO 这几类，
 * 需要排查协议交互时加 -v 即可全部打开。
 */
static int ui_log_tag_is_chatty(const char *level)
{
    return (strcmp(level, "RX") == 0 ||      /* 每一帧原始报文 */
            strcmp(level, "GB") == 0 ||      /* 每一条国标报文的语义解析 */
            strcmp(level, "TP") == 0 ||      /* J1939 多帧分包 */
            strcmp(level, "DB") == 0);       /* 批量落盘 */
}

void ui_log(const char *level, const char *fmt, ...)
{
    char    line[256];
    char    stamp[32];
    time_t  t = time(NULL);
    struct tm tmv;
    va_list ap;
    int     slot;

    /* 逐报文日志默认不输出（见 ui_log_tag_is_chatty 的说明） */
    if (!g_verbose && ui_log_tag_is_chatty((level != NULL) ? level : "INFO"))
    {
        return;
    }

    localtime_r(&t, &tmv);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tmv);

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (s_dashboard_on && s_ui_inited)
    {
        /* 仪表盘模式：只缓存，不打印。直接 printf 会和
         * "\033[H" 整屏重绘互相踩踏，导致画面撕裂。 */
        slot = s_event_total % UI_EVENT_LINES;
        snprintf(s_events[slot], sizeof(s_events[slot]),
                 "[%s] %-6s %s", stamp, (level != NULL) ? level : "INFO", line);
        s_event_total++;
        return;
    }

    /* 滚动日志模式：直接输出 */
    printf("%s[%s] %-7s%s %s\n", UI_C_GRAY, stamp,
           (level != NULL) ? level : "INFO", UI_C_RESET, line);
    fflush(stdout);
}

/*==============================================================================
 *                              仪表盘渲染
 *============================================================================*/

void ui_render(const ui_snapshot_t *snap)
{
    static char buf[16384];
    size_t pos = 0;
    char   uptime[32];
    char   dbsize[32];
    const gb_context_t *ctx;
    char   state_elapsed[32];

    if (snap == NULL)
    {
        return;
    }
    ctx = snap->ctx;

    fmt_hms(snap->uptime_sec, uptime, sizeof(uptime));
    fmt_size(snap->db_size, dbsize, sizeof(dbsize));

    /* 本状态驻留时长 = 当前毫秒时间 - 进入本状态的时间 */
    if (ctx != NULL && ctx->now_ms >= ctx->state_enter_ms)
    {
        fmt_hms((double)(ctx->now_ms - ctx->state_enter_ms) / 1000.0,
                state_elapsed, sizeof(state_elapsed));
    }
    else
    {
        snprintf(state_elapsed, sizeof(state_elapsed), "00:00:00");
    }

    /* 光标归位 */
    put_fmt(buf, sizeof(buf), &pos, "\033[H");

    /*------------------------ 标题栏 ------------------------*/
    put_fmt(buf, sizeof(buf), &pos, UI_C_BOLD UI_C_CYAN);
    put_line(buf, sizeof(buf), &pos);
    put_fmt(buf, sizeof(buf), &pos,
            " GB/T 27930-2015 充电监控终端   |   运行时长 %s   |   %s\n",
            uptime, snap->ifname);
    put_line(buf, sizeof(buf), &pos);
    put_fmt(buf, sizeof(buf), &pos, UI_C_RESET);

    /*------------------------ 总线状态 ------------------------*/
    {
        const char *st_color = UI_C_GREEN;
        if (snap->can.state == CANBUS_STATE_BUS_OFF)          { st_color = UI_C_RED; }
        else if (snap->can.state == CANBUS_STATE_ERROR_PASSIVE){ st_color = UI_C_RED; }
        else if (snap->can.state == CANBUS_STATE_ERROR_WARNING){ st_color = UI_C_YELLOW; }

        put_fmt(buf, sizeof(buf), &pos,
                " [总线] %s / %u bps   状态: %s%s%s   TEC=%u  REC=%u   控制器重启 %llu 次\n",
                snap->ifname, snap->bitrate,
                st_color, can_state_str(snap->can.state), UI_C_RESET,
                snap->can.txerr, snap->can.rxerr,
                (unsigned long long)snap->can.ctrl_restarts);

        put_fmt(buf, sizeof(buf), &pos,
                "        接收 %llu 帧 (%llu/s)   发送 %llu 帧 (%llu/s)   "
                "错误帧 %llu   BusOff %llu\n",
                (unsigned long long)snap->total_rx, (unsigned long long)snap->rx_rate,
                (unsigned long long)snap->total_tx, (unsigned long long)snap->tx_rate,
                (unsigned long long)snap->can.err_frames,
                (unsigned long long)snap->can.bus_off);

        /* 错误分类：全部为 0 时用绿色，否则黄色/红色 */
        {
            int any_err = (int)(snap->can.err_bit + snap->can.err_stuff +
                                snap->can.err_form + snap->can.err_ack +
                                snap->can.err_crc + snap->can.err_other);
            put_fmt(buf, sizeof(buf), &pos,
                    "        位错误 %llu | 填充错误 %llu | 格式错误 %llu | "
                    "ACK 错误 %llu | CRC 错误 %llu | 其他 %llu  %s\n",
                    (unsigned long long)snap->can.err_bit,
                    (unsigned long long)snap->can.err_stuff,
                    (unsigned long long)snap->can.err_form,
                    (unsigned long long)snap->can.err_ack,
                    (unsigned long long)snap->can.err_crc,
                    (unsigned long long)snap->can.err_other,
                    any_err ? (UI_C_YELLOW "[注意]" UI_C_RESET) : (UI_C_GREEN "[正常]" UI_C_RESET));
        }
    }

    /*------------------------ 环形缓冲 ------------------------*/
    put_line(buf, sizeof(buf), &pos);
    put_fmt(buf, sizeof(buf), &pos,
            " [缓冲] 环形队列 %zu/%zu   入队 %llu 帧   丢弃 %llu 帧   %s\n",
            snap->rb_size, snap->rb_capacity,
            (unsigned long long)snap->rb_pushed,
            (unsigned long long)snap->rb_dropped,
            (snap->rb_dropped == 0) ? (UI_C_GREEN "无丢帧" UI_C_RESET)
                                    : (UI_C_RED "存在丢帧，请增大缓冲区" UI_C_RESET));

    /*------------------------ 状态机 ------------------------*/
    put_line(buf, sizeof(buf), &pos);
    if (ctx != NULL)
    {
        const char *sc = UI_C_CYAN;
        if (ctx->state == GB_ST_FAULT)                          { sc = UI_C_RED; }
        else if (ctx->state == GB_ST_CHARGING)                  { sc = UI_C_GREEN; }
        else if (ctx->state == GB_ST_STOPPING)                  { sc = UI_C_YELLOW; }

        put_fmt(buf, sizeof(buf), &pos,
                " [状态机] %s%s%s   会话 #%u   本状态驻留 %s   解析报文 %llu 条   解析错误 %llu 次\n",
                sc, gb27930_state_str(ctx->state), UI_C_RESET,
                ctx->session_id, state_elapsed,
                (unsigned long long)ctx->rx_msg_count,
                (unsigned long long)ctx->parse_err_count);

        if (ctx->error != GB_ERR_NONE)
        {
            put_fmt(buf, sizeof(buf), &pos,
                    "          异常: " UI_C_RED "%s" UI_C_RESET "\n",
                    gb27930_error_str(ctx->error));
        }
        else
        {
            put_fmt(buf, sizeof(buf), &pos, "          异常: " UI_C_GREEN "无" UI_C_RESET "\n");
        }
    }

    /*------------------------ 实时数据 ------------------------*/
    put_line(buf, sizeof(buf), &pos);
    put_fmt(buf, sizeof(buf), &pos, " [实时数据]\n");

    if (ctx != NULL && (ctx->has_bcs || ctx->has_bcl || ctx->has_bcp))
    {
        double v_meas = ctx->has_bcs ? ctx->bcs.measure_voltage
                                     : (ctx->has_bcp ? ctx->bcp.current_voltage : 0.0);
        double i_meas = ctx->has_bcs ? ctx->bcs.measure_current : 0.0;
        double v_limit = ctx->has_bcp ? ctx->bcp.max_total_voltage : 0.0;
        double i_limit = ctx->has_bcp ? ctx->bcp.max_current : 0.0;
        double soc     = ctx->has_bcs ? ctx->bcs.current_soc : 0.0;

        /* 充电电压 */
        put_fmt(buf, sizeof(buf), &pos, "   总电压  %8.1f V  ", v_meas);
        put_bar(buf, sizeof(buf), &pos, v_meas, (v_limit > 0) ? v_limit : 750.0, 0.85, 0.95);
        put_fmt(buf, sizeof(buf), &pos, "   上限 %.1f V\n", v_limit);

        /* 充电电流 */
        put_fmt(buf, sizeof(buf), &pos, "   总电流  %8.1f A  ", i_meas);
        put_bar(buf, sizeof(buf), &pos, i_meas, (i_limit > 0) ? i_limit : 400.0, 0.85, 0.95);
        put_fmt(buf, sizeof(buf), &pos, "   上限 %.1f A\n", i_limit);

        /* SOC */
        put_fmt(buf, sizeof(buf), &pos, "   SOC     %8.1f %%  ", soc);
        put_bar(buf, sizeof(buf), &pos, soc, 100.0, 0.90, 1.01);
        put_fmt(buf, sizeof(buf), &pos, "\n");

        /* 单体电压
         * 【标准依据】BSM（标准表 20）里**没有电压字段**，只有"最高单体电压
         * 所在编号"；最高单体电压只在 BCS 的 B5-B6（1-12 位）里。
         * 所以这里的电压值只可能来自 BCS，编号来自 BCS 的组号字段。 */
        if (ctx->has_bcs)
        {
            put_fmt(buf, sizeof(buf), &pos,
                    "   最高单体电压 %6.3f V (所在组号 %2u%s)\n",
                    ctx->bcs.max_single_voltage, ctx->bcs.max_single_group,
                    ctx->bcs.group_number_valid ? "" : " 未知");
        }

        /* 温度 */
        if (ctx->has_bsm)
        {
            const char *tc = UI_C_GREEN;
            if (ctx->bsm.max_temp >= 55.0)      { tc = UI_C_RED; }
            else if (ctx->bsm.max_temp >= 45.0) { tc = UI_C_YELLOW; }

            put_fmt(buf, sizeof(buf), &pos,
                    "   最高温度 %s%6.1f C%s (检测点 %2u)   最低温度 %6.1f C (检测点 %2u)\n",
                    tc, ctx->bsm.max_temp, UI_C_RESET, ctx->bsm.max_temp_no,
                    ctx->bsm.min_temp, ctx->bsm.min_temp_no);

            /* BSM 的 6 个 2 位状态字段（标准表 20）。0=正常 1=异常 2=不可信。
             * 只要不是"全部正常 + 允许充电"，就把原因列出来。 */
            {
                static const char *st_name[3] = { "正常", "异常", "不可信" };
                uint8_t v = ctx->bsm.cell_voltage_state;
                uint8_t s = ctx->bsm.soc_state;
                uint8_t c = ctx->bsm.over_current_state;
                uint8_t t = ctx->bsm.over_temp_state;
                uint8_t n = ctx->bsm.insulation_state;
                uint8_t k = ctx->bsm.connector_state;

                put_fmt(buf, sizeof(buf), &pos,
                        "   BSM 状态 B6=0x%02X B7=0x%02X  单体电压[%s] SOC[%s] 过流[%s] "
                        "过温[%s] 绝缘[%s] 连接器[%s]  充电允许[%s]\n",
                        ctx->bsm.status_b6, ctx->bsm.status_b7,
                        st_name[v & 3u], st_name[s & 3u], st_name[c & 3u],
                        st_name[t & 3u], st_name[n & 3u], st_name[k & 3u],
                        (ctx->bsm.charge_permit & 1u) ? "允许" : "禁止");
            }
        }

        /* 需求 */
        if (ctx->has_bcl)
        {
            put_fmt(buf, sizeof(buf), &pos,
                    "   需求电压 %6.1f V   需求电流 %6.1f A   充电模式 %s\n",
                    ctx->bcl.voltage_demand, ctx->bcl.current_demand,
                    (ctx->bcl.charge_mode == 1) ? "恒压" :
                    (ctx->bcl.charge_mode == 2) ? "恒流" : "未知");
            /* ★ 标准 BCL 只有 5 字节 3 个字段，没有"允许充电电压/电流"。
             *   界面上原来那一行显示的允许值，现在统一从 BCP 取。 */
            if (ctx->has_bcp)
            {
                put_fmt(buf, sizeof(buf), &pos,
                        "   允许上限（来自 BCP） %.1f V / %.1f A\n",
                        ctx->bcp.max_total_voltage, ctx->bcp.max_current);
            }
        }

        /* 电池身份 */
        if (ctx->has_brm)
        {
            put_fmt(buf, sizeof(buf), &pos,
                    "   电池 厂商[%s] VIN[%s] 额定 %.1fV/%.1fAh 类型 %u 生产 %04u-%02u-%02u 循环 %u 次\n",
                    ctx->brm.manufacturer, ctx->brm.vin,
                    ctx->brm.rated_voltage, ctx->brm.rated_capacity,
                    ctx->brm.battery_type,
                    ctx->brm.produce_year, ctx->brm.produce_month, ctx->brm.produce_day,
                    ctx->brm.charge_count);
        }
    }
    else
    {
        put_fmt(buf, sizeof(buf), &pos,
                UI_C_GRAY "   等待 BMS 上报数据……（BCP/BCL/BCS/BSM 尚未收到）" UI_C_RESET "\n");
    }

    /*------------------------ 报文统计 ------------------------*/
    put_line(buf, sizeof(buf), &pos);
    if (ctx != NULL)
    {
        put_fmt(buf, sizeof(buf), &pos,
                " [报文] BHM %-5s BCP %-5s BRM %-5s BRO %-5s BCL %-5s BCS %-5s BSM %-5s BST %-5s\n",
                ctx->has_bhm ? "OK" : "-",
                ctx->has_bcp ? "OK" : "-",
                ctx->has_brm ? "OK" : "-",
                ctx->has_bro ? "OK" : "-",
                ctx->has_bcl ? "OK" : "-",
                ctx->has_bcs ? "OK" : "-",
                ctx->has_bsm ? "OK" : "-",
                ctx->has_bst ? "OK" : "-");
        put_fmt(buf, sizeof(buf), &pos,
                "        未知 ID 帧 %llu   多帧组包 完成 %u / 超时 %u / 序号错 %u\n",
                (unsigned long long)ctx->unknown_id_count,
                ctx->brm_rx.stat_sessions, ctx->brm_rx.stat_timeouts,
                ctx->brm_rx.stat_seq_err);
    }

    /*------------------------ 存储 ------------------------*/
    put_line(buf, sizeof(buf), &pos);
    put_fmt(buf, sizeof(buf), &pos,
            " [存储] SQLite(WAL) 原始 %llu 帧   解析 %llu 条   COMMIT %llu 次   "
            "DB %s   丢弃 %llu\n",
            (unsigned long long)snap->db_raw_written,
            (unsigned long long)snap->db_charge_written,
            (unsigned long long)snap->db_commits,
            dbsize,
            (unsigned long long)snap->db_raw_dropped);
    if (!s_compact)
    {
        put_fmt(buf, sizeof(buf), &pos,
                "        文件: %s\n", snap->db_path ? snap->db_path : "-");
    }

    /*------------------------ 底部事件区 ------------------------*/
    put_line(buf, sizeof(buf), &pos);
    put_fmt(buf, sizeof(buf), &pos, " [" UI_C_BOLD "最近事件" UI_C_RESET "]\n");
    if (s_event_total == 0)
    {
        put_fmt(buf, sizeof(buf), &pos, "   " UI_C_GRAY "（暂无）" UI_C_RESET "\n");
    }
    else
    {
        int maxshow = s_compact ? UI_EVENT_COMPACT : UI_EVENT_LINES;
        int shown = (s_event_total < maxshow) ? s_event_total : maxshow;
        int first = s_event_total - shown;
        int k;
        for (k = 0; k < shown; k++)
        {
            put_fmt(buf, sizeof(buf), &pos, "   %s%s%s\n",
                    UI_C_GRAY, s_events[(first + k) % UI_EVENT_LINES], UI_C_RESET);
        }
    }
    put_fmt(buf, sizeof(buf), &pos,
            " " UI_C_GRAY "Ctrl+C 优雅退出并落盘  |  -u 滚动日志模式  |  --help 查看参数"
            UI_C_RESET "\n");

    /* 清除本帧之后可能残留的旧内容 */
    put_fmt(buf, sizeof(buf), &pos, "\033[J");

    /* 一次性写出，避免画面撕裂 */
    if (pos > 0)
    {
        ssize_t ignored = write(STDOUT_FILENO, buf, pos);
        (void)ignored;
    }
}

/******************* (C) COPYRIGHT 2025 CAN Monitor *****END OF FILE****/
