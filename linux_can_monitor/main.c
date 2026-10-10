/**
 * @file    main.c
 * @brief   GB/T 27930-2015 BMS CAN 监控主终端 —— 主程序与多线程调度
 *
 * 运行平台：野火 I.MX6ULL Pro / Debian / Linux 4.19 (SocketCAN)
 * 编译方式：make（见同目录 Makefile）
 *
 * ============================ 线程模型 ============================
 *
 *   ┌────────────────────┐   can_layer_recv()   ┌──────────────────┐
 *   │  线程 1            │ ───────────────────► │ 无锁环形缓冲区    │
 *   │  CAN 接收线程      │   storage_push_raw() │ (1024 帧)        │
 *   │  (实时性最高)      │ ──────────┐          └────────┬─────────┘
 *   └────────────────────┘           │                   │ rb_pop_batch()
 *                                    │                   ▼
 *                                    │          ┌──────────────────────┐
 *                                    │          │  线程 2              │
 *                                    │          │  协议解析线程        │
 *                                    │          │  gb27930_process_frame│
 *                                    │          │  gb27930_tick        │
 *                                    │          └──────────┬───────────┘
 *                                    │                     │ storage_push_charge()
 *                                    ▼                     ▼
 *                          ┌───────────────────────────────────────┐
 *                          │  存储写队列 (无锁 SPSC × 2)            │
 *                          └───────────────────┬───────────────────┘
 *                                              │ storage_tick() 每 100 ms
 *                                              ▼
 *                                   ┌──────────────────────┐
 *                                   │  线程 3              │
 *                                   │  存储线程            │
 *                                   │  SQLite WAL 批量写入 │
 *                                   └──────────────────────┘
 *
 *   主线程：每秒刷新终端仪表盘；收到 SIGINT/SIGTERM 时优雅停机。
 *
 * 为什么这样分层？
 *   - CAN 接收线程只做「取帧 + 入队」，绝不解析、绝不写盘，
 *     保证 250 kbps 总线满负载（约 2000 帧/s）下不丢帧；
 *   - 协议解析线程独占状态机，天然串行，不需要给状态机加锁；
 *   - 存储线程承担最慢的 IO，通过无锁队列与解析线程解耦，
 *     磁盘抖动不会反压到实时链路。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <getopt.h>
#include <ctype.h>

#include "can_layer.h"
#include "ring_buffer.h"
#include "isotp.h"
#include "gb27930.h"
#include "storage.h"
#include "ui.h"
#include "gui_app.h"

/*----------------------------------------------------------------------------
 * 版本与构建戳
 *
 *   为什么要打「构建时间」：改完代码以后最常见的乌龙就是**板子上跑的
 *   还是旧程序**（忘了重新拷贝 / 忘了重新 make），于是新加的功能一个都
 *   看不到，却去怀疑逻辑写错了。启动时把构建时间打出来，
 *   一眼就能确认「我现在跑的到底是不是刚编的那份」。
 *
 *   BUILD_STAMP 用编译器的 __DATE__ / __TIME__，每次重新编译都会变。
 *--------------------------------------------------------------------------*/
#define FW_VERSION   "1.0.1"
#define FW_BUILD     __DATE__ " " __TIME__

/* 这个版本新增/修改的能力清单（用 --version 打印，便于自助排查） */
#define FW_FEATURES  "CAN 错误帧中文翻译 / --gui 本地界面 / --fb virtual / " \
                     "--touch demo / --touch-grab(EVIOCGRAB) / 桌面冲突检测 / " \
                     "--gui-rec 渲染留证"

/*==============================================================================
 *                          运行配置
 *============================================================================*/

typedef struct
{
    char     ifname[16];      /**< CAN 接口名 */
    uint32_t bitrate;         /**< 波特率 */
    char     db_path[256];    /**< 数据库路径 */
    int      loopback;        /**< 打开回环模式 */
    int      do_config;       /**< 是否通过 netlink 配置接口 */
    int      accept_all;      /**< 关闭硬件滤波 */
    int      dump_mode;       /**< 只抓包，不驱动充电状态机 */
    int      no_ui;           /**< 关闭仪表盘，只输出日志 */
    int      verbose;         /**< 1 = 打印详细启动过程（默认只打一行摘要） */
    int      run_seconds;     /**< 自动退出秒数，0 = 一直运行 */
    int      print_status;    /**< 打印状态后退出 */
    int      gui;             /**< 1 = 在本地屏上启用图形界面 */
    int      touch_grab;      /**< 1 = 独占抓取触摸设备（桌面在跑时必须开） */
    int      no_touch_grab;   /**< 1 = 即使检测到桌面也不自动抓取 */
    int      touch_debug;     /**< 1 = 打印触摸事件（只走日志，不在屏幕上画任何东西） */
    int      touch_swap;      /**< 1 = 交换触摸 X/Y */
    int      touch_mirror_x;  /**< 1 = 触摸 X 轴镜像 */
    int      touch_mirror_y;  /**< 1 = 触摸 Y 轴镜像 */
    int      cal_xmin, cal_xmax, cal_ymin, cal_ymax;  /**< 手动指定原始量程 */
    int      cal_valid;       /**< 1 = 上面这组量程有效 */
    char     fb_dev[64];      /**< framebuffer 设备路径（"virtual" = 内存虚拟屏） */
    int      auto_start;      /**< 1 = 上电即开始充电（自动化测试 / 单板自测用） */
    char     touch_dev[64];   /**< 触摸设备路径（空 = 自动查找，"demo" = 脚本化触摸） */
    char     gui_rec[256];    /**< 渲染留证目录（空 = 不导出帧） */
} app_config_t;

static app_config_t   g_cfg;
static volatile sig_atomic_t g_running = 1;

static can_layer_t    g_can;
static ring_buffer_t  g_rb;
static gb_context_t   g_ctx;
static gb_callbacks_t g_cbs;

/* 速率统计（主线程 1 Hz 采样） */
static uint64_t g_last_rx_total = 0;
static uint64_t g_last_tx_total = 0;
static uint64_t g_rx_rate = 0;
static uint64_t g_tx_rate = 0;

/*==============================================================================
 *                          信号处理
 *============================================================================*/

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;   /* 只置标志位；真正的清理在主线程完成 */
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);   /* CAN 写失败不应导致进程被杀 */
}

/*==============================================================================
 *                          协议层回调
 *============================================================================*/

static void cb_state_change(void *arg, gb_state_t old_state, gb_state_t new_state)
{
    (void)arg;
    ui_log("STATE", "状态切换: %s -> %s",
           gb27930_state_str(old_state), gb27930_state_str(new_state));
}

static void cb_error(void *arg, gb_error_t err, const char *detail)
{
    (void)arg;
    ui_log("ERROR", "%s%s%s", gb27930_error_str(err),
           (detail != NULL) ? " | " : "", (detail != NULL) ? detail : "");
}

static void cb_message(void *arg, uint32_t can_id, const uint8_t *data, uint8_t len)
{
    static uint64_t s_last_quiet_ms = 0;
    struct timespec ts;
    uint64_t        ms;
    char hex[64];
    int  i, n = 0;
    (void)arg;

    /* ---- 待机时的周期遥测不逐条打印 ----
     *
     * BMS 每秒广播一次电池状态（BSM）来喂实时电量。这些帧对充电流程没有意义，
     * 逐条打出来会把串口刷得看不清真正有用的日志，所以待机时**每 30 秒只放行
     * 一条**，用来确认"BMS 还在线、电量还在更新"就够了。
     * 一旦进入充电会话（state != IDLE），所有报文照常逐条打印。 */
    if (g_ctx.state == GB_ST_IDLE)
    {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);

        if (s_last_quiet_ms != 0 && (ms - s_last_quiet_ms) < 30000ULL)
        {
            return;
        }
        s_last_quiet_ms = ms;

        /* 放行的这一条也标清楚：它是"待机遥测"，不是协议交互 */
        ui_log("RX", "0x%08X [%u] (待机遥测，30 秒汇总一条)", can_id, len);
        return;
    }

    s_last_quiet_ms = 0;

    for (i = 0; i < len && n < (int)sizeof(hex) - 3; i++)
    {
        n += snprintf(hex + n, sizeof(hex) - (size_t)n, "%02X ", data[i]);
    }
    hex[(n > 0) ? (size_t)(n - 1) : 0] = '\0';

    ui_log("RX", "0x%08X [%u] %s", can_id, len, hex);
}

/*==============================================================================
 *                          线程 1：CAN 接收
 *============================================================================*/

typedef struct
{
    uint64_t frames;
    uint64_t errors;
    uint64_t bus_off_events;    /**< Bus-Off / 控制器重启次数（用于诊断提示） */
} rx_thread_stat_t;

static rx_thread_stat_t g_rx_stat;

static void *can_rx_thread(void *arg)
{
    can_layer_t *cl = (can_layer_t *)arg;
    can_item_t   item;
    can_item_t   rb_item;
    storage_raw_t raw;
    int           rc;

    ui_log("INFO", "CAN 接收线程已启动 (tid=%lu)", (unsigned long)pthread_self());

    while (g_running)
    {
        /* 超时 100 ms 便于及时响应退出信号 */
        rc = can_layer_recv(cl, &item, 100);

        if (rc == 0)
        {
            continue;   /* 超时，无数据 */
        }

        if (rc < 0 && rc != -1)
        {
            /* 真错误（socket 异常），短暂休眠避免忙等刷屏 */
            ui_log("ERROR", "CAN 接收失败: %s", strerror(-rc));
            usleep(200000);
            continue;
        }

        if (rc == -1)
        {
            /* -1 表示错误帧，已在 HAL 层分类统计。
             * 这里把内核那套「按位编码」的错误标志翻译成中文再打印 ——
             * 直接打 0x00000100，现场根本看不出是总线关闭还是 ACK 错误。 */
            char why[192];

            g_rx_stat.errors++;
            can_err_frame_str(item.can_id, item.data, why, sizeof(why));

            if (can_err_is_bus_off(item.can_id))
            {
                g_rx_stat.bus_off_events++;

                if (g_rx_stat.bus_off_events == 1u)
                {
                    /* 这一条最值得看：把原因和排查方向一次说清楚 */
                    ui_log("WARN", "CAN 错误帧: %s", why);
                    ui_log("WARN", "总线诊断: 控制器反复进入 Bus-Off，说明本机发出去的帧"
                                   "在总线上拿不到应答。若此刻「接收帧数」一直为 0，"
                                   "按顺序检查：①对端(STM32)是否上电且固件正在运行；"
                                   "②CANH/CANL 是否接反（两端同名相连）；"
                                   "③两端 120Ω 终端电阻是否到位（断电测 CANH-CANL 应为 60Ω）；"
                                   "④两端波特率是否都是 250 kbps。");
                }
                else if ((g_rx_stat.bus_off_events % 100u) == 0u)
                {
                    ui_log("WARN", "Bus-Off 已累计 %llu 次，收帧 %llu；总线仍然没有对端应答",
                           (unsigned long long)g_rx_stat.bus_off_events,
                           (unsigned long long)g_rx_stat.frames);
                }
            }
            else if (g_rx_stat.errors == 1u || (g_rx_stat.errors % 50u) == 0u)
            {
                /* 普通错误帧：第 1 条和每 50 条打印一次，避免刷屏 */
                ui_log("WARN", "CAN 错误帧: %s（累计 %llu）", why,
                       (unsigned long long)g_rx_stat.errors);
            }
            continue;
        }

        /* ---- 正常数据帧 ---- */
        g_rx_stat.frames++;

        /* 1. 写入无锁环形缓冲区，交给协议解析线程（低延迟路径） */
        rb_item = item;
        if (rb_push(&g_rb, &rb_item) != 0)
        {
            if (rb_drop_count(&g_rb) % 1000u == 1u)
            {
                ui_log("WARN", "环形缓冲区已满，累计丢弃 %llu 帧",
                       (unsigned long long)rb_drop_count(&g_rb));
            }
        }

        /* 2. 原始报文落库（异步，不阻塞接收）
         *
         * 【只存充电会话里的帧】待机时 BMS 每秒广播一次 BSM、外加心跳，
         * 这些周期遥测对事后分析没有任何价值，却会把库撑大：
         * 实测跑 7 分钟就有 887 帧原始记录，而真正有用的充电报文只有 215 条，
         * 其余全是待机遥测 —— 跑一天就是十几万条垃圾。
         * 所以跟"帧计数"用同一个开关：只有会话进行中才落库。
         * （错误帧不在此列，任何时候都要留证据。） */
        if (g_can_stat_enable || item.is_error)
        {
            raw.ts_us    = (int64_t)item.ts.tv_sec * 1000000LL + item.ts.tv_nsec / 1000;
            raw.can_id   = item.can_id;
            raw.dlc      = item.dlc;
            raw.is_error = item.is_error;
            memcpy(raw.data, item.data, 8);
            (void)storage_push_raw(&raw);
        }
    }

    ui_log("INFO", "CAN 接收线程退出（累计收帧 %llu）",
           (unsigned long long)g_rx_stat.frames);
    return NULL;
}

/*==============================================================================
 *                          线程 2：协议解析
 *============================================================================*/

static void *protocol_thread(void *arg)
{
    can_layer_t *cl = (can_layer_t *)arg;
    can_item_t   batch[32];
    size_t       n;
    int          i;
    int          rc;
    struct timespec now;
    uint64_t     now_us;

    uint64_t     last_charge_persist_ms = 0;
    storage_charge_t rec;

    ui_log("INFO", "协议解析线程已启动 (tid=%lu)", (unsigned long)pthread_self());

    while (g_running)
    {
        /* 1. 批量取出待解析帧，减少循环开销 */
        n = rb_pop_batch(&g_rb, batch, 32);
        for (i = 0; i < (int)n; i++)
        {
            rc = gb27930_process_frame(&g_ctx, cl, &batch[i]);
            if (rc < 0)
            {
                ui_log("WARN", "报文解析失败: ID=0x%08X 原因=%s",
                       batch[i].can_id, isotp_status_str((isotp_status_t)rc));
            }
        }

        /* 2. 周期性驱动状态机（发送报文、超时判定、组包超时检查） */
        clock_gettime(CLOCK_MONOTONIC, &now);
        now_us = (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)(now.tv_nsec / 1000);
        gb27930_tick(&g_ctx, cl, now_us);

        /* 2.5 帧计数器只在充电会话里统计。
         *
         * 待机时 BMS 每秒广播一次电池状态（外加心跳），这些是周期遥测，
         * 不是充电协议交互。如果照样计数，界面上的"收/发帧数"开机没多久
         * 就是几千，完全看不出**这一次充电**到底跑了多少报文。
         * 帧本身照收照处理，只是不进统计。 */
        /* 抓包模式（--dump）不驱动状态机，状态永远是 IDLE ——
         * 那种模式下必须照常统计，否则 candump 式的用法会显示"收 0 帧"。 */
        g_can_stat_enable = (g_cfg.dump_mode || g_ctx.state != GB_ST_IDLE) ? 1 : 0;

        /* 3. 每秒把当前充电上下文持久化一条（充电阶段的曲线数据） */
        if (g_ctx.state >= GB_ST_PARAM_CONFIG && g_ctx.state != GB_ST_FAULT)
        {
            uint64_t ms = now_us / 1000ULL;
            if (ms - last_charge_persist_ms >= 1000ULL)
            {
                last_charge_persist_ms = ms;
                gb27930_fill_storage_record(&g_ctx, &rec);
                (void)storage_push_charge(&rec);
            }
        }

        /* 4. 5 ms 调度间隔：对 50 ms 周期的 BCL 而言余量充足 */
        usleep(5000);
    }

    ui_log("INFO", "协议解析线程退出");
    return NULL;
}

/*==============================================================================
 *                          线程 3：存储
 *============================================================================*/

static void *storage_thread(void *arg)
{
    (void)arg;
    int written;

    ui_log("INFO", "存储写库线程已启动 (tid=%lu)", (unsigned long)pthread_self());

    while (g_running)
    {
        written = storage_tick();
        if (written > 0 && written >= STORAGE_BATCH_FRAMES)
        {
            /* 达到批量阈值时打印一次，便于观察写入节奏 */
            ui_log("DB", "批量落盘 %d 条（累计原始 %llu / 解析 %llu）",
                   written,
                   (unsigned long long)storage_raw_written(),
                   (unsigned long long)storage_charge_written());
        }
        usleep(100000);   /* 100 ms 轮询一次，兼顾实时性与 CPU 占用 */
    }

    ui_log("INFO", "存储写库线程退出");
    return NULL;
}

/*==============================================================================
 *                          参数解析
 *============================================================================*/

/* 详细启动日志开关。默认关：现场反馈 5 行 [n/5] 加 10 行 ID 列表
 * 把启动信息刷得看不清重点。想看细节加 -v。 */
int g_verbose = 0;

#define V(...)   do { if (g_verbose) { printf(__VA_ARGS__); } } while (0)

static void config_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.ifname, sizeof(g_cfg.ifname), "%s", CAN_DEFAULT_IFNAME);
    g_cfg.bitrate   = CAN_BITRATE_GB27930;      /* 250 kbps */
    snprintf(g_cfg.db_path, sizeof(g_cfg.db_path), "%s", STORAGE_DEFAULT_DB);
    g_cfg.do_config = 1;
    snprintf(g_cfg.fb_dev, sizeof(g_cfg.fb_dev), "%s", "/dev/fb0");

    /* ---- 开箱即用：默认就把本屏 GUI 和触摸打开 ----
     * 现场这块 480x272 屏（GT1151）的量程必须写成 0,474,0,272
     * （X 用 474 而不是 480，面板右边最后几列压不到），
     * 直接写进默认值，这样 `./can_monitor` 不带任何参数就能用。
     * 想关掉界面用 --no-gui。 */
    g_cfg.gui       = 1;
    g_cfg.cal_valid = 1;
    g_cfg.cal_xmin  = 0;
    g_cfg.cal_xmax  = 474;
    g_cfg.cal_ymin  = 0;
    g_cfg.cal_ymax  = 272;
}

static int parse_args(int argc, char **argv)
{
    /* 只做长选项的参数（界面相关），用 >255 的编号避免与字符选项冲突 */
    enum { OPT_GUI = 1000, OPT_AUTO_START, OPT_FB, OPT_TOUCH, OPT_GUI_REC, OPT_TOUCH_GRAB,
           OPT_NO_TOUCH_GRAB, OPT_TOUCH_DEBUG, OPT_TOUCH_SWAP,
           OPT_TOUCH_MIRROR_X, OPT_TOUCH_MIRROR_Y, OPT_TOUCH_CALIB,
           OPT_NO_GUI, OPT_VERSION };

    static const struct option long_opts[] = {
        { "interface",  required_argument, NULL, 'i' },
        { "bitrate",    required_argument, NULL, 'b' },
        { "database",   required_argument, NULL, 'd' },
        { "loopback",   no_argument,       NULL, 'l' },
        { "no-gui",     no_argument,       NULL, OPT_NO_GUI },
        { "verbose",    no_argument,       NULL, 'v' },
        { "auto-start", no_argument,       NULL, OPT_AUTO_START },
        { "no-config",  no_argument,       NULL, 'n' },
        { "accept-all", no_argument,       NULL, 'a' },
        { "dump",       no_argument,       NULL, 'D' },
        { "no-ui",      no_argument,       NULL, 'u' },
        { "record",     required_argument, NULL, 'r' },
        { "status",     no_argument,       NULL, 's' },
        { "gui",        no_argument,       NULL, OPT_GUI },
        { "fb",         required_argument, NULL, OPT_FB },
        { "touch",      required_argument, NULL, OPT_TOUCH },
        { "gui-rec",    required_argument, NULL, OPT_GUI_REC },
        { "touch-grab", no_argument,       NULL, OPT_TOUCH_GRAB },
        { "no-touch-grab", no_argument,    NULL, OPT_NO_TOUCH_GRAB },
        { "touch-debug", no_argument,      NULL, OPT_TOUCH_DEBUG },
        { "touch-swap",  no_argument,      NULL, OPT_TOUCH_SWAP },
        { "touch-mirror-x", no_argument,   NULL, OPT_TOUCH_MIRROR_X },
        { "touch-mirror-y", no_argument,   NULL, OPT_TOUCH_MIRROR_Y },
        { "touch-calib", required_argument, NULL, OPT_TOUCH_CALIB },
        { "version",    no_argument,       NULL, OPT_VERSION },
        { "help",       no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    int opt;

    while ((opt = getopt_long(argc, argv, "i:b:d:lnaDur:shv", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
            case 'i': snprintf(g_cfg.ifname, sizeof(g_cfg.ifname), "%s", optarg); break;
            case 'b': g_cfg.bitrate = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'd': snprintf(g_cfg.db_path, sizeof(g_cfg.db_path), "%s", optarg); break;
            case 'l': g_cfg.loopback = 1; break;
            case 'n': g_cfg.do_config = 0; break;
            case 'a': g_cfg.accept_all = 1; break;
            case 'D': g_cfg.dump_mode = 1; break;
            case 'u': g_cfg.no_ui = 1; break;
            case 'v': g_cfg.verbose = 1; g_verbose = 1; break;
            case OPT_NO_GUI: g_cfg.gui = 0; break;
            case 'r': g_cfg.run_seconds = atoi(optarg); break;
            case 's': g_cfg.print_status = 1; break;
            case OPT_GUI:   g_cfg.gui = 1; break;
            case OPT_AUTO_START: g_cfg.auto_start = 1; break;
            case OPT_FB:    snprintf(g_cfg.fb_dev, sizeof(g_cfg.fb_dev), "%s", optarg); break;
            case OPT_TOUCH: snprintf(g_cfg.touch_dev, sizeof(g_cfg.touch_dev), "%s", optarg); break;
            case OPT_GUI_REC:
                snprintf(g_cfg.gui_rec, sizeof(g_cfg.gui_rec), "%s", optarg);
                g_cfg.gui = 1;      /* 留证隐含启用界面 */
                break;
            case OPT_TOUCH_GRAB:
                g_cfg.touch_grab = 1;
                g_cfg.gui = 1;      /* 抓取触摸隐含启用界面 */
                break;
            case OPT_NO_TOUCH_GRAB:
                g_cfg.no_touch_grab = 1;
                break;
            case OPT_TOUCH_DEBUG:
                g_cfg.touch_debug = 1;
                g_cfg.gui = 1;
                break;
            case OPT_TOUCH_SWAP:
                g_cfg.touch_swap = 1;
                g_cfg.gui = 1;
                break;
            case OPT_TOUCH_MIRROR_X:
                g_cfg.touch_mirror_x = 1;
                g_cfg.gui = 1;
                break;
            case OPT_TOUCH_MIRROR_Y:
                g_cfg.touch_mirror_y = 1;
                g_cfg.gui = 1;
                break;
            case OPT_TOUCH_CALIB:
            {
                /* 格式: xmin,xmax,ymin,ymax （例如 0,480,0,272） */
                int a = 0, b = 0, c = 0, d = 0;

                if (sscanf(optarg, "%d,%d,%d,%d", &a, &b, &c, &d) == 4 &&
                    b > a && d > c)
                {
                    g_cfg.cal_xmin = a; g_cfg.cal_xmax = b;
                    g_cfg.cal_ymin = c; g_cfg.cal_ymax = d;
                    g_cfg.cal_valid = 1;
                    g_cfg.gui = 1;
                }
                else
                {
                    fprintf(stderr,
                            "错误: --touch-calib 的格式应为 xmin,xmax,ymin,ymax\n"
                            "      例如 --touch-calib 0,480,0,272\n");
                    return -1;
                }
                break;
            }
            case OPT_VERSION:
                printf("GB/T 27930-2015 BMS CAN 监控终端\n");
                printf("  版本    : v%s\n", FW_VERSION);
                printf("  构建时间: %s\n", FW_BUILD);
                printf("  已编入的能力: %s\n", FW_FEATURES);
                return -1;
            case 'h': ui_usage(argv[0]); return -1;
            default:  ui_usage(argv[0]); return -1;
        }
    }
    return 0;
}

/*==============================================================================
 *                          主函数
 *============================================================================*/

int main(int argc, char **argv)
{
    pthread_t th_rx, th_proto, th_store;
    int       rc;
    struct timespec t_start, t_now;
    double    uptime;
    ui_snapshot_t snap;
    uint64_t  last_ui_sec = 0;

    config_defaults();
    if (parse_args(argc, argv) != 0)
    {
        return 1;
    }

    install_signal_handlers();

    printf(UI_C_BOLD "GB/T 27930-2015 BMS CAN 监控终端 v%s" UI_C_RESET "\n",
           FW_VERSION);
    /* 构建戳：确认板子上跑的就是刚编出来的这一份（排查「改了没生效」的第一步） */
    printf("构建时间: %s    (源码: " __FILE__ ")\n", FW_BUILD);
    printf("正在初始化……\n");

    /*------------------------------------------------------------------
     * 步骤 1：配置并打开 CAN 接口
     *----------------------------------------------------------------*/
    if (g_cfg.do_config)
    {
        V("[1/5] 通过 rtnetlink 配置 %s: bitrate=%u ...\n",
               g_cfg.ifname, g_cfg.bitrate);
        if (can_set_bitrate(g_cfg.ifname, g_cfg.bitrate) != 0)
        {
            printf("      " UI_C_YELLOW "警告" UI_C_RESET
                   ": 在线配置失败，继续使用接口现有配置\n");
        }
        if (g_cfg.loopback)
        {
            (void)can_set_loopback(g_cfg.ifname, 1);
        }
        if (can_set_link_up(g_cfg.ifname, 1) != 0)
        {
            printf("      " UI_C_YELLOW "警告" UI_C_RESET
                   ": 接口 UP 失败，请手工执行 sudo ip link set %s up\n", g_cfg.ifname);
        }
    }
    else
    {
        V("[1/5] 跳过接口配置（--no-config）\n");
    }

    /*------------------------------------------------------------------
     * 步骤 2：创建 SocketCAN 套接字
     *----------------------------------------------------------------*/
    rc = can_layer_init(&g_can, g_cfg.ifname);
    if (rc != 0)
    {
        fprintf(stderr,
                UI_C_RED "错误" UI_C_RESET ": CAN 接口 %s 初始化失败 (rc=%d)\n"
                "  请确认:\n"
                "    1) 接口存在 : ip link show %s\n"
                "    2) 已启动    : sudo ip link set %s up\n"
                "    3) 波特率    : sudo ip link set %s type can bitrate %u\n"
                "    4) 内核模块  : lsmod | grep -E 'can|flexcan|mcp251x'\n"
                "    5) 接线      : CANH/CANL 是否接反，120R 终端电阻是否到位\n",
                g_cfg.ifname, rc, g_cfg.ifname, g_cfg.ifname,
                g_cfg.ifname, g_cfg.bitrate);
        return 2;
    }

    /* 硬件滤波：只放行 GB/T 27930 中 BMS→充电机的报文 ID */
    if (g_cfg.accept_all)
    {
        (void)can_apply_filter_all(&g_can);
    }
    else
    {
        (void)gb27930_apply_rx_filter(&g_can);
    }

    /* 错误帧订阅 */
    (void)can_enable_error_frames(&g_can);

    /*------------------------------------------------------------------
     * 步骤 3：初始化无锁环形缓冲区（默认 1024 帧）
     *----------------------------------------------------------------*/
    if (rb_init(&g_rb, RB_DEFAULT_CAPACITY) != 0)
    {
        fprintf(stderr, "错误: 环形缓冲区初始化失败\n");
        can_layer_deinit(&g_can);
        return 3;
    }
    V("[2/5] 无锁环形缓冲区已创建: 容量 %zu 帧\n", rb_capacity(&g_rb));

    /*------------------------------------------------------------------
     * 步骤 4：打开 SQLite 数据库
     *----------------------------------------------------------------*/
    if (storage_open(g_cfg.db_path) != 0)
    {
        fprintf(stderr, "错误: 数据库打开失败: %s\n", g_cfg.db_path);
        fprintf(stderr, "      请确认已安装 libsqlite3-dev 并有写权限\n");
        rb_deinit(&g_rb);
        can_layer_deinit(&g_can);
        return 4;
    }
    V("[3/5] SQLite 数据库已打开: %s\n", g_cfg.db_path);

    /*------------------------------------------------------------------
     * 步骤 5：初始化协议层状态机
     *----------------------------------------------------------------*/
    g_cbs.on_state_change = cb_state_change;
    g_cbs.on_error        = cb_error;
    g_cbs.on_message      = g_cfg.dump_mode ? NULL : cb_message;
    g_cbs.arg            = NULL;

    gb27930_init(&g_ctx, &g_cbs);

    /* --auto-start：上电就当作已经按下充电按钮。
     * 正常情况下监听端是待机的，要等人在界面上点「充电」才发 CHM；
     * 自动化脚本没法点屏幕，所以给一个显式开关。 */
    if (g_cfg.auto_start)
    {
        gb27930_request_start(&g_ctx);
        printf("[CAN] 已启用 --auto-start：上电即开始充电\n");
    }
    g_ctx.dump_mode = g_cfg.dump_mode;   /* 抓包模式：只解析不主动发报文 */

    V("[4/5] GB/T 27930 协议栈已就绪，初始状态: %s\n",
           gb27930_state_str(g_ctx.state));

    /*------------------------------------------------------------------
     * 步骤 6：启动三个工作线程
     *----------------------------------------------------------------*/
    if (pthread_create(&th_rx, NULL, can_rx_thread, &g_can) != 0)
    {
        fprintf(stderr, "错误: CAN 接收线程创建失败\n");
        storage_close();
        rb_deinit(&g_rb);
        can_layer_deinit(&g_can);
        return 5;
    }
    if (pthread_create(&th_proto, NULL, protocol_thread, &g_can) != 0)
    {
        fprintf(stderr, "错误: 协议解析线程创建失败\n");
        g_running = 0;
        pthread_join(th_rx, NULL);
        storage_close();
        rb_deinit(&g_rb);
        can_layer_deinit(&g_can);
        return 5;
    }
    if (pthread_create(&th_store, NULL, storage_thread, NULL) != 0)
    {
        fprintf(stderr, "错误: 存储线程创建失败\n");
        g_running = 0;
        pthread_join(th_rx, NULL);
        pthread_join(th_proto, NULL);
        storage_close();
        rb_deinit(&g_rb);
        can_layer_deinit(&g_can);
        return 5;
    }

    V("[5/5] 三线程已启动: CAN接收 / 协议解析 / 数据存储\n");

    if (!g_verbose)
    {
        printf("[CAN] %s @ %u bps 已就绪 | 环形缓冲 %zu 帧 | 日志库 %s\n",
               g_cfg.ifname, g_cfg.bitrate, rb_capacity(&g_rb), g_cfg.db_path);
    }
    sleep(1);

    if (g_cfg.print_status)
    {
        printf("\n当前接口状态: ");
        {
            char info[128];
            if (can_query_link(g_cfg.ifname, info, sizeof(info)) == 0)
            {
                printf("%s\n", info);
            }
        }
        g_running = 0;
    }

    /*------------------------------------------------------------------
     * 主循环：每秒刷新一次仪表盘
     *----------------------------------------------------------------*/
    if (!g_cfg.no_ui && !g_cfg.print_status)
    {
        if (ui_init())
        {
            ui_banner(FW_VERSION, g_cfg.ifname, g_cfg.bitrate, g_cfg.db_path,
                      !g_cfg.accept_all, g_cfg.loopback);
            sleep(2);
        }
        else
        {
            /* 终端太矮，ui_init() 已自动降级为滚动日志模式，
             * 这里同步切换主循环与 ui_log 的输出方式。 */
            g_cfg.no_ui = 1;
            ui_banner(FW_VERSION, g_cfg.ifname, g_cfg.bitrate, g_cfg.db_path,
                      !g_cfg.accept_all, g_cfg.loopback);
            printf("======== 滚动日志模式，Ctrl+C 退出 ========\n");
        }
    }
    else
    {
        printf("======== 进入日志模式（无仪表盘）Ctrl+C 退出 ========\n");
    }

    /*------------------------------------------------------------------
     * 本地图形界面（可选）：framebuffer + 触摸屏
     *   与终端仪表盘互相独立，可以同时开：SSH 里看仪表盘，
     *   开发板本屏上看曲线。打不开 framebuffer 时只告警不退出。
     *----------------------------------------------------------------*/
    if (g_cfg.gui && !g_cfg.print_status)
    {
        gui_app_cfg_t gcfg;

        memset(&gcfg, 0, sizeof(gcfg));
        gcfg.can       = &g_can;
        gcfg.ctx       = &g_ctx;
        gcfg.ifname    = g_cfg.ifname;
        gcfg.bitrate   = g_cfg.bitrate;
        gcfg.fb_dev    = g_cfg.fb_dev;
        gcfg.touch_dev = (g_cfg.touch_dev[0] != '\0') ? g_cfg.touch_dev : NULL;
        gcfg.rec_dir   = (g_cfg.gui_rec[0] != '\0') ? g_cfg.gui_rec : NULL;
        gcfg.db_path   = g_cfg.db_path;      /* 历史会话和主库同一个文件 */
        gcfg.touch_grab = g_cfg.touch_grab;
        gcfg.no_touch_grab = g_cfg.no_touch_grab;
        gcfg.touch_debug = g_cfg.touch_debug;
        gcfg.touch_swap = g_cfg.touch_swap;
        gcfg.touch_mirror_x = g_cfg.touch_mirror_x;
        gcfg.touch_mirror_y = g_cfg.touch_mirror_y;
        gcfg.cal_valid = g_cfg.cal_valid;
        gcfg.cal_xmin = g_cfg.cal_xmin;
        gcfg.cal_xmax = g_cfg.cal_xmax;
        gcfg.cal_ymin = g_cfg.cal_ymin;
        gcfg.cal_ymax = g_cfg.cal_ymax;

        if (gui_app_start(&gcfg) == 0)
        {
            ui_log("GUI", "本地图形界面已启动（%s），点「数据曲线」看本次充电曲线",
                   g_cfg.fb_dev);
        }
        else
        {
            ui_log("WARN", "本地图形界面启动失败，继续以终端模式运行");
            g_cfg.gui = 0;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_start);

    while (g_running)
    {
        uint64_t total_rx, total_tx;

        sleep(1);

        clock_gettime(CLOCK_MONOTONIC, &t_now);
        uptime = (double)(t_now.tv_sec - t_start.tv_sec)
               + (double)(t_now.tv_nsec - t_start.tv_nsec) / 1e9;

        /* --- 计算速率 --- */
        can_layer_refresh_sysfs_stats(&g_can);
        total_rx = g_can.stats.rx_frames;
        total_tx = g_can.stats.tx_frames;
        g_rx_rate = total_rx - g_last_rx_total;
        g_tx_rate = total_tx - g_last_tx_total;
        g_last_rx_total = total_rx;
        g_last_tx_total = total_tx;

        if (g_cfg.no_ui || g_cfg.print_status)
        {
            /* 日志模式下每秒打印一行摘要，便于 SSH 记录 */
            if (((uint64_t)uptime) % 5 == 0)
            {
                ui_log("STAT", "状态=%s 电压=%.1fV 电流=%.1fA SOC=%.1f%% "
                               "收=%llu/s 发=%llu/s 错误=%llu",
                       gb27930_state_str(g_ctx.state),
                       g_ctx.has_bcs ? g_ctx.bcs.measure_voltage : 0.0,
                       g_ctx.has_bcs ? g_ctx.bcs.measure_current : 0.0,
                       g_ctx.has_bcs ? g_ctx.bcs.current_soc : 0.0,
                       (unsigned long long)g_rx_rate,
                       (unsigned long long)g_tx_rate,
                       (unsigned long long)g_can.stats.err_frames);
            }
        }
        else if (((uint64_t)uptime) != last_ui_sec)
        {
            last_ui_sec = (uint64_t)uptime;

            memset(&snap, 0, sizeof(snap));
            snap.ifname            = g_cfg.ifname;
            snap.bitrate           = g_cfg.bitrate;
            snap.db_path           = g_cfg.db_path;
            snap.can               = g_can.stats;
            snap.rx_rate           = g_rx_rate;
            snap.tx_rate           = g_tx_rate;
            snap.total_rx          = total_rx;
            snap.total_tx          = total_tx;
            snap.rb_size           = rb_size(&g_rb);
            snap.rb_capacity       = rb_capacity(&g_rb);
            snap.rb_pushed         = rb_push_count(&g_rb);
            snap.rb_dropped        = rb_drop_count(&g_rb);
            snap.ctx               = &g_ctx;
            snap.db_raw_written    = storage_raw_written();
            snap.db_charge_written = storage_charge_written();
            snap.db_raw_dropped    = storage_raw_dropped();
            snap.db_commits        = storage_commit_count();
            snap.db_size           = storage_db_size_bytes();
            snap.uptime_sec        = uptime;

            ui_render(&snap);
        }

        /* --- 自动退出（自动化测试用） --- */
        if (g_cfg.run_seconds > 0 && uptime >= (double)g_cfg.run_seconds)
        {
            ui_log("INFO", "已达到预设运行时长 %d 秒，准备退出", g_cfg.run_seconds);
            g_running = 0;
        }
    }

    /*------------------------------------------------------------------
     * 优雅退出：停线程 -> 落盘 -> 关数据库 -> 关 socket
     *----------------------------------------------------------------*/
    printf("\n");
    ui_log("INFO", "收到退出信号，开始优雅停机……");

    /* 先停界面线程：它会读 SQLite 加载历史曲线，
     * 必须在本线程 storage_close() 之前退出，否则会用到已关闭的句柄。 */
    if (gui_app_is_active())
    {
        ui_log("INFO", "正在停止本地图形界面……");
        gui_app_stop();
    }

    pthread_join(th_rx, NULL);
    pthread_join(th_proto, NULL);
    pthread_join(th_store, NULL);

    {
        int flushed = storage_flush();

        /* 这里的数字是"关机收尾这一次又补交了多少条"。
         * 数据是边跑边落盘的（见 storage 的批量提交），所以正常情况
         * 缓冲区早就空了，这里就是 0 —— 那表示"没有残留"，不是出错。
         * 原来的文案容易被误读成"一条都没存进去"，所以分开说。 */
        if (flushed > 0)
        {
            ui_log("INFO", "关机收尾：缓冲区还有 %d 条，已补交落盘", flushed);
        }
        else
        {
            ui_log("INFO", "关机收尾：缓冲区已空，无需补交（数据此前已边跑边落盘）");
        }
    }
    storage_close();
    rb_deinit(&g_rb);
    can_layer_deinit(&g_can);

    ui_shutdown();

    printf("========================================================================\n");
    printf(" 运行统计\n");
    printf("   运行时长    : %.0f 秒\n", uptime);
    printf("   接收帧数    : %llu\n", (unsigned long long)g_can.stats.rx_frames);
    printf("   发送帧数    : %llu\n", (unsigned long long)g_can.stats.tx_frames);
    printf("   错误帧数    : %llu (位错误 %llu / 填充 %llu / 格式 %llu / ACK %llu / CRC %llu)\n",
           (unsigned long long)g_can.stats.err_frames,
           (unsigned long long)g_can.stats.err_bit,
           (unsigned long long)g_can.stats.err_stuff,
           (unsigned long long)g_can.stats.err_form,
           (unsigned long long)g_can.stats.err_ack,
           (unsigned long long)g_can.stats.err_crc);
    printf("   解析报文数  : %llu\n", (unsigned long long)g_ctx.rx_msg_count);
    printf("   数据库原始  : %llu 帧\n", (unsigned long long)storage_raw_written());
    printf("   数据库解析  : %llu 条\n", (unsigned long long)storage_charge_written());
    printf("   数据库文件  : %s\n", g_cfg.db_path);
    printf("========================================================================\n");
    printf("程序已退出。\n");

    return 0;
}

/******************* (C) COPYRIGHT 2025 CAN Monitor *****END OF FILE****/
