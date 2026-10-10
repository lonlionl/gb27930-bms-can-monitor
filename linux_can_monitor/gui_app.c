/**
 * @file    gui_app.c
 * @brief   GUI 应用层实现 —— 数据快照组装 + 界面刷新 + 触摸交互
 *
 * 刷新策略
 * --------
 *   - 主界面：每 200 ms 重绘一次（5 Hz，人眼足够流畅，CPU 占用极低）
 *   - 历史界面：数据变化慢，每 1000 ms 重绘一次，省 CPU
 *   - 触摸轮询：每 30 ms 读一次 evdev（板子上触摸事件本身也是这个量级）
 *   - 重绘前先判断「画面是否有变化」，无变化直接跳过，避免无谓的
 *     memcpy 到 framebuffer（480x272x4 = 510 KB/次，5 Hz 就是 2.5 MB/s）
 *
 * 电量积分
 * --------
 *   会话电量由本模块自行积分：E += U * I * dt / 3600（kWh）
 *   只有在 CHARGING 状态且电流为负（约定充电为负）时才累加，
 *   避免把握手阶段的噪声也算进去。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gui_app.h"

extern int g_verbose;   /* 由 main.c 定义 */

/* 只在 -v 时输出的日志宏（和 main.c 里的同名宏一致）。
 * 参数解析、触摸命中、渲染留证这些都属于"排查用"的信息，
 * 产品形态下不该往串口里灌。 */
#ifndef V
#define V(...)   do { if (g_verbose) { printf(__VA_ARGS__); } } while (0)
#endif
#include "fbdev.h"
#include "gui.h"
#include "uitouch.h"
#include "storage.h"

/*==============================================================================
 *                              内部状态
 *============================================================================*/

#define GUI_REFRESH_DASH_MS     200u    /**< 主界面刷新周期 */
#define GUI_REFRESH_HIST_MS     1000u   /**< 历史界面刷新周期 */
#define GUI_CURVE_BG_MS         1000u   /**< 曲线缓存后台刷新周期 */
#define GUI_TOUCH_POLL_MS       30u     /**< 触摸轮询周期 */
#define GUI_CURVE_POINTS        300     /**< 历史曲线取最近 300 个采样点 */

/** fb_dev 传这个字符串时使用纯内存虚拟屏（不接触 /dev/fb*） */
#define GUI_FB_VIRTUAL          "virtual"
/** 默认的渲染留证帧数上限 */
#define GUI_REC_DEFAULT_LIMIT   24

static struct
{
    pthread_t       tid;
    int             running;          /**< 1 = 线程已启动 */
    volatile int    quit;             /**< 请求退出标志 */

    gui_app_cfg_t   cfg;              /**< 配置副本 */
    char            fb_dev[64];       /**< framebuffer 设备路径 */
    char            touch_dev[64];    /**< 触摸设备路径 */
    char            rec_dir[256];     /**< 渲染留证目录 */

    fbdev_t         fb;               /**< framebuffer 句柄 */
    uitouch_t       touch;            /**< 触摸句柄 */
    int             has_touch;        /**< 触摸是否可用 */

    gui_screen_t    screen;           /**< 当前界面 */
    gui_metric_t    metric;           /**< 数据曲线页 / 历史详情页当前画哪条曲线 */
    uint64_t        charge_lock_ms;   /**< 充电按钮互锁到期时刻（毫秒） */

    /* 电量积分 */
    uint64_t        last_tick_ms;
    double          energy_kwh;       /**< 本次会话累计电量 */
    uint32_t        charge_sec;       /**< 本次会话累计充电时长 */
    uint32_t        last_session_id;  /**< 用于检测会话切换 */

    /* 历史曲线缓存 */
    gui_curve_t     curve;            /**< 当前这一次的曲线（内存实时采集） */
    int64_t         now_us;           /**< 本轮采样时间戳（真实时钟） */
    int             have_soc;         /**< 1 = 已经收到过 SOC */
    double          soc_now;          /**< 当前 SOC */
    double          soc_prev;         /**< 上一次采样时的 SOC（判断在涨还是在掉） */
    double          soc_max;          /**< 本次会话见过的最大 SOC（判断是否真充满） */
    int             cur_saved;        /**< 1 = 本次已经存进历史 */
    int             cur_full;         /**< 1 = 本次是**充满**结束的（0 = 中途结束） */
    int             user_stopped;     /**< 1 = 这一次是用户按「停止」中止的 */

    /* ---- 历史会话（最近 9 次，新的在前） ---- */
    session_info_t  hist[SESSION_MAX];
    int             hist_n;
    int             detail_idx;       /**< 正在看第几条历史 */
    gui_curve_t     detail_curve;     /**< 历史详情页的曲线 */
    uint64_t        curve_load_ms;    /**< 上次加载曲线的时间 */

    /* 渲染留证 */
    int             rec_count;        /**< 已导出的帧数 */

    /* 日志抑制 */
    int             warn_no_storage;
} g_gui;

/*==============================================================================
 *                              工具函数
 *============================================================================*/

/** 真实（墙上）微秒时钟：会话起止时间要用它，不能用单调时钟 */
static int64_t gui_wall_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}

/** 单调毫秒时钟 */
static uint64_t gui_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

/*==============================================================================
 *                        桌面环境冲突检测
 *
 *   野火 Debian 出厂是带桌面（X11）自启动的。这时会同时出现两个问题：
 *     · 桌面在往 /dev/fb0 上画 → 我们的界面被反复盖掉，屏幕闪回桌面；
 *     · 桌面已经打开了触摸的 event 设备并消费事件 → 我们一个触摸事件都收不到。
 *   这两件事的现象刚好就是「界面显示正常，但点不动、还时不时闪一下」。
 *
 *   这里只做检测与提示（不擅自去杀别人的进程）：
 *     · 有 X 的 socket → 说明 X 在跑
 *     · 屏幕闪回桌面 → 提示停桌面
 *   真正的解决办法在文档里给了具体命令。
 *============================================================================*/

/** 桌面（X11）是否在运行 */
static int gui_desktop_running(void)
{
    /* X 服务器会在 /tmp/.X11-unix/ 下建 X0 / X1 … 的 unix socket */
    if (access("/tmp/.X11-unix/X0", F_OK) == 0)
    {
        return 1;
    }
    if (access("/tmp/.X11-unix", F_OK) == 0)
    {
        return 1;   /* 目录存在基本就能认定有 X 环境 */
    }
    return 0;
}

/** 检测到桌面就打印一次明确的处置建议 */
static void gui_check_desktop_conflict(void)
{
    if (!gui_desktop_running())
    {
        return;
    }

    /* 桌面冲突的说明已删掉：现场那台板子不跑桌面，这段只会占满串口。
     * 检测逻辑本身保留（启动流程靠它决定要不要自动独占抓取触摸），
     * 只是不再打印。 */
    (void)0;
}

/*==============================================================================
 *                          渲染留证（导出帧）
 *============================================================================*//**
 * @brief  把当前后台缓冲导出成 PPM 文件
 *
 * 为什么留这个功能：屏幕上的东西只有在板子上才看得见，调试时很难取证。
 * 打开 --gui-rec 后，每次重绘都会把整帧写到文件，之后用
 *   python3 tools/ppm2png.py <目录>
 * 转成 PNG 就能直接在电脑上目视检查（也能拿去做项目报告/答辩截图）。
 *
 * 用 PPM 而不是直接写 PNG：C 侧不需要引入任何压缩库（PNG 要 zlib），
 * 而 PPM 是最简单的无损位图格式，转换交给 PC 上的 Python 一行搞定。
 */
static void gui_dump_frame(const char *tag)
{
    char path[512];

    if (g_gui.rec_dir[0] == '\0')
    {
        return;
    }
    if (g_gui.rec_count >= g_gui.cfg.rec_limit)
    {
        return;
    }

    snprintf(path, sizeof(path), "%s/%02d_%s.ppm", g_gui.rec_dir,
             g_gui.rec_count, tag);

    if (fb_dump_ppm(&g_gui.fb, path) == 0)
    {
        g_gui.rec_count++;
        V("[GUI] 已导出渲染帧 %s\n", path);
    }
}

/*==============================================================================
 *                        本次充电的曲线（内存实时采集）
 *
 * 用户要求：
 *   · 数据曲线页只画**当前这一次**充电，不要把历次混在一起
 *   · 曲线只向右生长，画过的部分固定不动（横轴是 SOC，见 gui.c）
 *   · 充电结束后把这一次存进历史，最多保留 9 次
 *
 * 所以本次曲线是**实时采集在内存里**的，不再去数据库查。
 * 数据库只负责保存「已经结束的历史会话」。
 *============================================================================*/

/** 开始一次新会话：清空本次曲线 */
static void curve_begin(void)
{
    memset(&g_gui.curve, 0, sizeof(g_gui.curve));
    g_gui.cur_saved    = 0;
    g_gui.cur_full     = 0;
    g_gui.user_stopped = 0;
    g_gui.soc_max      = 0.0;
}

/** 追加一个采样点 */
static void curve_push(double v, double i, double soc, double temp)
{
    gui_curve_t *c = &g_gui.curve;
    int          k;

    if (c->n >= GUI_MAX_POINTS)
    {
        /* 内部存储满了：丢掉最旧的一个点。
         * 这只是存储用的滑动窗口，画面上横轴仍然是 SOC，
         * 不会出现「心电图式左移」。 */
        for (k = 1; k < GUI_MAX_POINTS; k++)
        {
            c->v[k - 1]    = c->v[k];
            c->i[k - 1]    = c->i[k];
            c->soc[k - 1]  = c->soc[k];
            c->temp[k - 1] = c->temp[k];
            c->ts[k - 1]   = c->ts[k];
        }
        c->n = GUI_MAX_POINTS - 1;
    }

    c->v[c->n]    = v;
    c->i[c->n]    = i;
    c->soc[c->n]  = soc;
    c->temp[c->n] = temp;
    c->ts[c->n]   = g_gui.now_us;    /* 横轴是时间，每个点都要带自己的时刻 */

    if (c->n == 0)
    {
        c->ts_first = g_gui.now_us;
    }
    c->ts_last = g_gui.now_us;
    c->n++;

    /* 记下最高电量。
     *
     * 为什么到 99.x 就要当成 100：
     *   STM32 的 SOC 步长是 0.5%，它在 SOC 刚到 100.0% 的那一拍就切到
     *   "结束态"、停止发 BCS 了 —— 100.0% 这一帧往往**根本发不出来**，
     *   充电机这边最多只看到 99.5%。所以 [99.0, 100] 统一归到 100，
     *   否则屏上永远显示 99。 */
    if (soc > c->soc_max)
    {
        c->soc_max = (soc >= 99.0) ? 100.0 : soc;
    }
}

/** 刷新历史列表（最近 9 次，新的在前） */
static void hist_reload(void)
{
    if (!session_ready())
    {
        g_gui.hist_n = 0;
        return;
    }
    g_gui.hist_n = session_list(g_gui.hist, SESSION_MAX);
}

/**
  * @brief  把本次曲线存进历史
  * @note   只在「这一次已经结束」时调用；cur_saved 做去重，保证只存一次。
  */
static void curve_finish(void)
{
    session_info_t info;
    int soc_x10[SESSION_POINTS];
    int v_x10[SESSION_POINTS];
    int i_x10[SESSION_POINTS];
    int t_c[SESSION_POINTS];
    int k, n;

    if (g_gui.cur_saved || g_gui.curve.n < 2 || !session_ready())
    {
        return;
    }

    n = g_gui.curve.n;
    if (n > SESSION_POINTS) { n = SESSION_POINTS; }

    for (k = 0; k < n; k++)
    {
        soc_x10[k] = (int)(g_gui.curve.soc[k]  * 10.0 + 0.5);
        v_x10[k]   = (int)(g_gui.curve.v[k]    * 10.0 + 0.5);
        i_x10[k]   = (int)(g_gui.curve.i[k]    * 10.0 + 0.5);
        t_c[k]     = (int)(g_gui.curve.temp[k] + 0.5);
    }

    memset(&info, 0, sizeof(info));
    info.start_us      = g_gui.curve.ts_first;
    info.end_us        = g_gui.curve.ts_last;
    info.soc_start_x10 = soc_x10[0];
    /* 结束电量取本次见过的最高值（见 curve_push 里的说明），
     * 这样充满的那次才会记成 100.0% 而不是 99.x%。 */
    info.soc_end_x10   = (int)(g_gui.curve.soc_max * 10.0 + 0.5);
    if (info.soc_end_x10 < soc_x10[n - 1]) { info.soc_end_x10 = soc_x10[n - 1]; }
    if (info.soc_end_x10 < info.soc_start_x10) { info.soc_end_x10 = info.soc_start_x10; }
    info.energy_wh     = (int)(g_gui.energy_kwh * 1000.0 + 0.5);
    info.charge_sec    = (int)g_gui.charge_sec;
    info.full          = g_gui.cur_full;

    if (session_save(&info, soc_x10, v_x10, i_x10, t_c, n) == 0)
    {
        g_gui.cur_saved = 1;
        hist_reload();
    }
}

/** 载入第 idx 条历史的完整曲线（详情页用） */
static void detail_load(int idx)
{
    static int soc[SESSION_POINTS];
    static int v[SESSION_POINTS];
    static int ic[SESSION_POINTS];
    static int tc[SESSION_POINTS];
    int n, k;

    memset(&g_gui.detail_curve, 0, sizeof(g_gui.detail_curve));

    if (idx < 0 || idx >= g_gui.hist_n || !session_ready()) { return; }

    n = session_load_curve(g_gui.hist[idx].id, soc, v, ic, tc,
                           SESSION_POINTS, NULL);
    if (n <= 0) { return; }

    /* 历史曲线在库里是按"8 字节一个点"存的，没有逐点时间戳，
     * 但采样间隔基本固定 1 秒，所以在开始/结束时间之间线性摊开即可，
     * 横轴刻度精度到分钟，这样完全够用。 */
    {
        int64_t t0  = g_gui.hist[idx].start_us;
        int64_t t1  = g_gui.hist[idx].end_us;
        int64_t span = t1 - t0;

        for (k = 0; k < n && k < GUI_MAX_POINTS; k++)
        {
            g_gui.detail_curve.soc[k]  = (double)soc[k] / 10.0;
            g_gui.detail_curve.v[k]    = (double)v[k]   / 10.0;
            g_gui.detail_curve.i[k]    = (double)ic[k]  / 10.0;
            g_gui.detail_curve.temp[k] = (double)tc[k];
            g_gui.detail_curve.ts[k]   =
                (n > 1) ? (t0 + span * (int64_t)k / (int64_t)(n - 1)) : t0;
        }
    }
    g_gui.detail_curve.n        = k;
    g_gui.detail_curve.ts_first = g_gui.hist[idx].start_us;
    g_gui.detail_curve.ts_last  = g_gui.hist[idx].end_us;
    g_gui.detail_curve.soc_max  = (double)g_gui.hist[idx].soc_end_x10 / 10.0;
}

/**
  * @brief  推断充电阶段（界面显示用）
  *
  * STM32 是「电池」，i.MX 是「监控」。阶段由协议状态机 + SOC 趋势推出来：
  *   握手 ~ 充电中    -> 充电中
  *   STOPPING        -> 充满（BMS 发了 BST 收尾）
  *   其它 + SOC 在涨  -> 充电中
  *   其它 + SOC 在掉  -> 放电中（停止充电后按同速率掉电）
  *   其它 + 没数据    -> 待机
  */
static gui_phase_t infer_phase(void)
{
    gb_context_t *ctx = g_gui.cfg.ctx;

    if (ctx != NULL)
    {
        switch (ctx->state)
        {
            case GB_ST_HANDSHAKE:
            case GB_ST_IDENTIFY:
            case GB_ST_PARAM_CONFIG:
            case GB_ST_CHARGING_READY:
            case GB_ST_CHARGING:
                return GUI_PHASE_CHARGING;

            case GB_ST_STOPPING:
                return GUI_PHASE_FULL;

            default:
                break;
        }
    }

    if (!g_gui.have_soc) { return GUI_PHASE_STANDBY; }

    if (g_gui.soc_now > g_gui.soc_prev + 0.05) { return GUI_PHASE_CHARGING; }
    if (g_gui.soc_now < g_gui.soc_prev - 0.05) { return GUI_PHASE_DISCHARGE; }

    /* SOC 没变：充过电的就是停止状态，否则还是待机 */
    return (g_gui.curve.n > 0) ? GUI_PHASE_DISCHARGE : GUI_PHASE_STANDBY;
}

/** 本次充电是否已经结束（决定何时存档） */
static int charge_is_over(void)
{
    gb_context_t *ctx = g_gui.cfg.ctx;

    if (ctx == NULL) { return 0; }

    switch (ctx->state)
    {
        case GB_ST_HANDSHAKE:
        case GB_ST_IDENTIFY:
        case GB_ST_PARAM_CONFIG:
        case GB_ST_CHARGING_READY:
        case GB_ST_CHARGING:
            return 0;       /* 还在充电流程里 */
        default:
            break;
    }

    return (g_gui.curve.n >= 2) ? 1 : 0;
}

/*==============================================================================
 *                          数据快照组装
 *============================================================================*/

/** 计算 x 轴时间标签文本 */
static void snapshot_build(gui_data_t *d)
{
    gui_app_cfg_t *cfg = &g_gui.cfg;
    gb_context_t  *ctx = cfg->ctx;
    can_layer_t   *cl  = cfg->can;
    uint64_t now = gui_now_ms();
    double dt_h;

    memset(d, 0, sizeof(*d));

    /* ---------------- 状态机 ---------------- */
    if (ctx != NULL)
    {
        d->state_name = gb27930_state_str(ctx->state);
        d->error_name = gb27930_error_str(ctx->error);
        d->session_id = ctx->session_id;
        d->charging   = (ctx->state == GB_ST_CHARGING) ? 1 : 0;

        /* ---------------- 实时物理量 ----------------
         * 判定「有数据」的标准：收到过 BCS（充电电压/电流/SOC 都来自它），
         * 否则界面显示 "--" 占位符，避免把 0 误当成真实测量值。
         * 【标准依据】最高单体电压只在 BCS 的 B5-B6（1-12 位）里 ——
         * 标准 BSM（表 20）**没有电压字段**，只有"所在编号"，
         * 所以这里不再用 BSM 覆盖单体电压。 */
        if (ctx->has_bcs)
        {
            d->has_data    = 1;
            d->voltage     = ctx->bcs.measure_voltage;
            d->current     = ctx->bcs.measure_current;
            d->soc         = ctx->bcs.current_soc;
            d->cell_max_v  = ctx->bcs.max_single_voltage;
            d->cell_max_no = ctx->bcs.max_single_group;   /* 标准是"所在组号" */
        }
        if (ctx->has_bsm)
        {
            d->temp_max = ctx->bsm.max_temp;
            d->temp_min = ctx->bsm.min_temp;
        }
        if (ctx->has_bcp)
        {
            d->limit_v = ctx->bcp.max_total_voltage;
            d->limit_i = ctx->bcp.max_current;
        }
        /* 没有 BCP 时退回 BHM 的最高允许充电总电压（SPN2601）。
         * BCL 里**没有**允许值 —— 标准表 17 只有 5 字节 3 个字段。 */
        else if (ctx->has_bhm)
        {
            d->limit_v = ctx->bhm_max_total_voltage;
            d->limit_i = 0.0;
        }
    }
    else
    {
        d->state_name = "N/A";
        d->error_name = "N/A";
    }

    /* ---------------- 电量积分 ----------------
     * 约定：充电电流为负（BMS 侧测量方向），因此取绝对值。
     * 只在本帧与上一帧都处于充电状态时才累加，防止跨状态跳变。 */
    if (g_gui.last_tick_ms == 0)
    {
        g_gui.last_tick_ms = now;
    }
    dt_h = ((double)(now - g_gui.last_tick_ms)) / 3600000.0;

    if (ctx != NULL && d->charging && d->has_data &&
        dt_h > 0.0 && dt_h < 0.5)   /* 超过 0.5 s 的间隔视为异常，丢弃 */
    {
        g_gui.energy_kwh += fabs(d->voltage * d->current) * dt_h / 1000.0;
        g_gui.charge_sec += (uint32_t)(now - g_gui.last_tick_ms) / 1000u;
    }
    g_gui.last_tick_ms = now;

    /* 会话切换：清零积分 + 开一条新曲线 */
    if (ctx != NULL && ctx->session_id != g_gui.last_session_id)
    {
        g_gui.last_session_id = ctx->session_id;
        g_gui.energy_kwh = 0.0;
        g_gui.charge_sec = 0u;

        if (ctx->session_id != 0u)
        {
            curve_begin();      /* 新的一次充电，曲线从头开始画 */
        }
    }

    d->energy_kwh = g_gui.energy_kwh;
    d->charge_sec = g_gui.charge_sec;

    /* 本次曲线也带上累计电量，数据曲线页右下角才能实时变化 */
    g_gui.curve.energy_kwh = g_gui.energy_kwh;

    /* ---------------- CAN 总线 ---------------- */
    if (cl != NULL)
    {
        d->ifname   = cl->ifname;
        d->bitrate  = cl->bitrate;
        d->rx_frames = cl->stats.rx_frames;
        d->tx_frames = cl->stats.tx_frames;
        d->err_frames = cl->stats.err_frames;
        d->can_state = can_state_str(cl->stats.state);
    }
    else
    {
        d->ifname    = cfg->ifname ? cfg->ifname : "?";
        d->bitrate   = cfg->bitrate;
        d->can_state = "UNKNOWN";
    }

    /* ---------------- 存储 ---------------- */
    d->db_raw    = storage_raw_written();
    d->db_charge = storage_charge_written();
    d->db_size   = storage_db_size_bytes();

    /* ---------------- 充电阶段 + 本次曲线采集 ----------------
     * 曲线只在充电中采点，而且两点之间至少隔 1 秒 —— 否则一条 5 分钟的
     * 充电会塞进几千个点，既没必要也会让画图变慢。 */
    g_gui.now_us = gui_wall_us();

    if (d->has_data)
    {
        g_gui.soc_prev = g_gui.soc_now;
        g_gui.soc_now  = d->soc;
        g_gui.have_soc = 1;

        /* 记下见过的最高电量。
         * 不能只看"结束那一刻的 SOC"：充电到 100% 后 BMS 会先发 BST 收尾，
         * 等到我们判定会话结束时，最后一帧可能已经回到 99.5% 甚至更低 ——
         * 那就会把"充满"误判成"中途结束"，也正是现场看到"每次都显示充到 99"的原因。 */
        if (d->soc > g_gui.soc_max) { g_gui.soc_max = d->soc; }

        if (d->charging &&
            (g_gui.curve.n == 0 ||
             (g_gui.now_us - g_gui.curve.ts_last) >= 1000000LL))
        {
            curve_push(d->voltage, d->current, d->soc, d->temp_max);
        }
    }

    d->phase = infer_phase();
    d->charging_active = gb27930_is_active(g_gui.cfg.ctx);

    /* 一次充电结束（BMS 发了 BST 或回到空闲）就把这次存进历史。
     * curve_finish() 内部用 cur_saved 去重，重复调用不会存两次。 */
    if (charge_is_over())
    {
        /* 只有"自己充到满"才算充满结束；
         * 用户按「停止」中止的，哪怕刚好在 100% 也算中途结束。 */
        if (!g_gui.user_stopped && g_gui.soc_max >= 99.0)
        {
            g_gui.cur_full = 1;
        }
        curve_finish();
    }
}

/*==============================================================================
 *                          触摸事件处理
 *============================================================================*/

/**
 * @brief  处理一次触摸
 *
 * 关键点：只响应「按下」边沿，不响应移动和抬起 ——
 * 否则一次点击会在 30 ms 的轮询周期里被重复触发好几次，界面来回跳。
 */
static void handle_touch(void)
{
    int guard = 0;

    if (!g_gui.has_touch)
    {
        return;
    }

    while (guard++ < 8)
    {
        gui_btn_t btn;
        int x, y;

        if (uitouch_poll(&g_gui.touch, 0) <= 0)
        {
            break;
        }
        if (!g_gui.touch.pressed)
        {
            continue;
        }

        x = g_gui.touch.x;
        y = g_gui.touch.y;

        btn = gui_hit_test(g_gui.screen, x, y);

        switch (btn)
        {
        case GUI_BTN_CHARGE:
            /* 一个按钮两种作用，和现实里"插枪 / 拔枪"对应：
             *   没在充电 -> 开始充电
             *   正在充电 -> 中止充电（相当于把枪拔了）
             *
             * 【互锁】按下去之后 3 秒内不再响应同一个按钮。
             * 一次点击在触摸事件队列里很容易留下好几个"按下"事件：
             * 第一个把状态变成"结束中"，紧随其后的第二个又会被判成
             * "没在充电" -> 当成「充电」-> 重新开始。
             * 现场看到的就是"停一下又开始充电"。加时间互锁从根上断掉连击。 */
            if (gui_now_ms() < g_gui.charge_lock_ms)
            {
                break;
            }
            /* 只挡 800 ms：够滤掉一次点击重复投递的触摸事件，
             * 又不会让人感觉"按了没反应"。真正防误重启靠协议层的 stop_ms。 */
            g_gui.charge_lock_ms = gui_now_ms() + 800u;

            if (g_gui.cfg.ctx != NULL)
            {
                if (gb27930_is_active(g_gui.cfg.ctx))
                {
                    V("[GUI] 触摸 (%d,%d) -> 「停止」：中止充电\n", x, y);
                    /* 记下"这次是人工停的"，存历史时才能区分
                     * 「充满结束」和「中途结束」。 */
                    g_gui.user_stopped = 1;
                    gb27930_request_stop(g_gui.cfg.ctx);
                }
                else
                {
                    V("[GUI] 触摸 (%d,%d) -> 「充电」：开始充电\n", x, y);
                    gb27930_request_start(g_gui.cfg.ctx);
                }
            }
            break;

        case GUI_BTN_DATACURVE:
            V("[GUI] 触摸 (%d,%d) -> 数据曲线\n", x, y);
            g_gui.screen = GUI_SCREEN_DATACURVE;
            break;

        case GUI_BTN_HISTLIST:
            hist_reload();
            V("[GUI] 触摸 (%d,%d) -> 历史记录（%d 条）\n", x, y, g_gui.hist_n);
            g_gui.screen = GUI_SCREEN_HISTORY;
            break;

        case GUI_BTN_CELL:
        {
            int idx = gui_history_cell_at(x, y);

            if (idx >= 0 && idx < g_gui.hist_n)
            {
                V("[GUI] 触摸 (%d,%d) -> 查看历史 #%d\n",
                       x, y, g_gui.hist[idx].id);
                g_gui.detail_idx = idx;
                detail_load(idx);
                g_gui.screen = GUI_SCREEN_HISTDETAIL;
            }
            break;
        }

        case GUI_BTN_METRIC:
        {
            int idx = gui_metric_button_at(x, y);

            if (idx >= 0)
            {
                static const char *nm[GUI_METRIC_COUNT] =
                    { "电压", "电流", "温度", "电量" };

                g_gui.metric = (gui_metric_t)idx;
                V("[GUI] 触摸 (%d,%d) -> 曲线切换: %s\n", x, y, nm[idx]);
            }
            break;
        }

        case GUI_BTN_BACK:
            if (g_gui.screen == GUI_SCREEN_HISTDETAIL)
            {
                g_gui.screen = GUI_SCREEN_HISTORY;
            }
            else if (g_gui.screen == GUI_SCREEN_HISTORY)
            {
                g_gui.screen = GUI_SCREEN_DATACURVE;
            }
            else
            {
                g_gui.screen = GUI_SCREEN_DASHBOARD;
            }
            V("[GUI] 触摸 (%d,%d) -> 返回\n", x, y);
            break;

        default:
            /* 点在空白处：忽略。调试模式下要说出来 ——
             * "点了没反应"时，这一行能区分是没收到触摸还是没命中按钮。 */
            if (g_gui.cfg.touch_debug)
            {
                V("[GUI] 触摸 (%d,%d) -> 未命中任何按钮\n", x, y);
            }
            break;
        }

        /* 【重要】一次 handle_touch() 只处理一个动作就退出。
         *
         * 一次点击可能在事件队列里留下好几个"按下"事件。如果全都处理，
         * 点一下「停止」就会先停止、再被同一个按钮当成「充电」重新开始 ——
         * 现场看到的就是"STM32 停一下又开始充电"。 */
        break;
    }
}
/*==============================================================================
 *                          GUI 线程主体
 *============================================================================*/

static void *gui_thread(void *arg)
{
    gui_data_t d;
    uint64_t last_draw = 0;
    uint64_t last_touch = 0;
    uint64_t last_curve = 0;

    (void)arg;
    V("[GUI] 界面线程启动：%dx%d %d bpp，触摸 %s\n",
           g_gui.fb.w, g_gui.fb.h, g_gui.fb.bpp,
           g_gui.has_touch ? "已启用" : "不可用（仅显示）");

    gui_init(&g_gui.fb);
    g_gui.screen = GUI_SCREEN_DASHBOARD;
    g_gui.metric = GUI_METRIC_V;
    hist_reload();

    /* 首帧强制绘制 */
    last_draw = 0;

    while (!g_gui.quit)
    {
        uint64_t now = gui_now_ms();
        uint32_t period;

        /* ---- 历史列表后台刷新 ----
         * 本次曲线是在内存里实时采集的（见 curve_push），不需要查库；
         * 这里只是定期把"最近 9 次历史"的元信息刷一下，
         * 好让数据曲线页的「历史」按钮点进去时立刻就有内容。 */
        if (last_curve == 0 || now - last_curve >= GUI_CURVE_BG_MS)
        {
            last_curve = now;
            hist_reload();
            g_gui.curve_load_ms = now;
        }

        /* ---- 触摸 ---- */
        if (now - last_touch >= GUI_TOUCH_POLL_MS)
        {
            last_touch = now;
            handle_touch();
        }

        /* ---- 绘制 ---- */
        /* 主界面刷新快一点（实时读数），其它页面 1 Hz 足够 */
        period = (g_gui.screen == GUI_SCREEN_DASHBOARD)
                 ? GUI_REFRESH_DASH_MS : GUI_REFRESH_HIST_MS;

        if (last_draw == 0 || now - last_draw >= period)
        {
            last_draw = now;
            snapshot_build(&d);

            switch (g_gui.screen)
            {
                case GUI_SCREEN_DATACURVE:
                    gui_draw_datacurve(&g_gui.fb, &d, &g_gui.curve,
                                       g_gui.curve.n > 0, g_gui.metric);
                    break;

                case GUI_SCREEN_HISTORY:
                    gui_draw_histgrid(&g_gui.fb, g_gui.hist, g_gui.hist_n);
                    break;

                case GUI_SCREEN_HISTDETAIL:
                    gui_draw_histdetail(&g_gui.fb,
                        (g_gui.detail_idx < g_gui.hist_n)
                            ? &g_gui.hist[g_gui.detail_idx] : NULL,
                        &g_gui.detail_curve, g_gui.detail_curve.n > 0,
                        g_gui.metric);
                    break;

                default:
                    gui_draw_dashboard(&g_gui.fb, &d);
                    break;
            }

            fb_present(&g_gui.fb);

            /* 渲染留证：切页后的第一帧 + 之后按节流导出 */
            gui_dump_frame((g_gui.screen == GUI_SCREEN_DASHBOARD) ? "main" :
                           (g_gui.screen == GUI_SCREEN_DATACURVE) ? "curve" :
                           (g_gui.screen == GUI_SCREEN_HISTORY)   ? "grid"  :
                                                                    "detail");
        }

        /* 睡一小会儿；被信号打断也没关系，循环会重新判断 quit */
        usleep(10 * 1000);
    }

    V("[GUI] 界面线程退出\n");
    return NULL;
}

/*==============================================================================
 *                          对外接口
 *============================================================================*/

int gui_app_start(const gui_app_cfg_t *cfg)
{
    int rc;

    if (g_gui.running)
    {
        V("[GUI] 界面已经在运行\n");
        return 0;
    }

    /* 版本印章：日志里必须能看出跑的是哪一版界面代码。
     * 之前反复出现"改了代码、板上行为没变"，靠现象根本分不清是
     * 没重新编译/没传过去，还是改得不对。把关键特征直接打出来。 */
    /* 版本印章已按用户要求删除。
     * 以后确认板上跑的是哪一版，比对二进制的时间戳或 md5 即可。 */
    if (cfg == NULL)
    {
        return -1;
    }

    memset(&g_gui, 0, sizeof(g_gui));
    g_gui.cfg = *cfg;

    /* 打开充电历史库（和主库同一个文件；失败不影响界面显示） */
    if (cfg->db_path != NULL)
    {
        if (session_init(cfg->db_path) != 0)
        {
            V("[GUI] 历史库打开失败，九宫格将显示为空\n");
        }
    }
    if (g_gui.cfg.rec_limit <= 0)
    {
        g_gui.cfg.rec_limit = GUI_REC_DEFAULT_LIMIT;
    }

    /* 默认为空则走内置默认值 */
    snprintf(g_gui.fb_dev, sizeof(g_gui.fb_dev), "%s",
             (cfg->fb_dev != NULL) ? cfg->fb_dev : FB_DEFAULT_DEV);
    if (cfg->touch_dev != NULL)
    {
        snprintf(g_gui.touch_dev, sizeof(g_gui.touch_dev), "%s", cfg->touch_dev);
    }
    if (cfg->rec_dir != NULL)
    {
        snprintf(g_gui.rec_dir, sizeof(g_gui.rec_dir), "%s", cfg->rec_dir);
    }

    /* ---- framebuffer ----
     * fb_dev == "virtual" 时用纯内存虚拟屏：界面的整条链路（数据快照、
     * 布局、绘制、触摸切页、历史曲线加载）都能在没有任何屏幕的机器上跑通，
     * 配合 --gui-rec 把每一帧导出来目视检查。 */
    if (strcmp(g_gui.fb_dev, GUI_FB_VIRTUAL) == 0)
    {
        rc = fb_open_virtual(&g_gui.fb, GUI_DESIGN_W, GUI_DESIGN_H);
        if (rc != 0)
        {
            fprintf(stderr, "[GUI] 创建虚拟 framebuffer 失败\n");
            return -1;
        }
        V("[GUI] 使用虚拟 framebuffer（不接触 /dev/fb*）：%dx%d %d bpp\n",
               g_gui.fb.w, g_gui.fb.h, g_gui.fb.bpp);
    }
    else
    {
        rc = fb_open(&g_gui.fb, g_gui.fb_dev);
        if (rc != 0)
        {
            fprintf(stderr,
                    "[GUI] 打开 framebuffer %s 失败：%s\n"
                    "      可能原因：内核未编译 fb 驱动 / 设备节点名不同 / 权限不足。\n"
                    "      排查：ls -l /dev/fb*  ;  cat /proc/fb  ;  id\n"
                    "      提示：CAN 核心功能不受影响，程序继续以终端模式运行；\n"
                    "            若只想验证界面逻辑，可用 --fb virtual 跑虚拟屏。\n",
                    g_gui.fb_dev, strerror(errno));
            return -1;
        }
        V("[GUI] framebuffer: %s  %dx%d  %d bpp  stride=%d\n",
               g_gui.fb_dev, g_gui.fb.w, g_gui.fb.h,
               g_gui.fb.bpp, g_gui.fb.line_len);

        /* 板子如果带着桌面环境启动，会有人跟我们抢这块 fb：
         * 桌面会不停往 /dev/fb0 上重绘，界面就会被反复盖掉
         * （现象是"界面时不时闪回桌面"）。这里提前警告。 */
        gui_check_desktop_conflict();
    }

    /* ---- 触摸 ---- */
    rc = uitouch_open(&g_gui.touch,
                      (g_gui.touch_dev[0] != '\0') ? g_gui.touch_dev : NULL,
                      g_gui.fb.w, g_gui.fb.h);
    if (rc == 0)
    {
        int grabbed = 0;

        g_gui.has_touch = 1;

        /* 坐标修正与调试开关要在打印设备信息之前设好，
         * 这样 uitouch_dump_info() 里那几行诊断才是对的。 */
        uitouch_set_transform(&g_gui.touch, cfg->touch_swap,
                              cfg->touch_mirror_x, cfg->touch_mirror_y);
        if (cfg->cal_valid)
        {
            uitouch_set_range(&g_gui.touch, cfg->cal_xmin, cfg->cal_xmax,
                              cfg->cal_ymin, cfg->cal_ymax);
        }
        uitouch_set_debug(&g_gui.touch, cfg->touch_debug);
        uitouch_dump_info(&g_gui.touch);

        /* 独占抓取：桌面（X11）会把触摸事件全部消费掉，
         * 不抓取的话界面一点反应都没有 —— 现象就是「点右上角反而点到桌面」。
         *
         * 策略：**检测到桌面就自动抓取**。
         *   这是本工程反复踩过的坑（界面能显示、就是点不动），与其让人去记
         *   --touch-grab 这个参数，不如自动做掉；不想被抢的用 --no-touch-grab
         *   关掉即可。抓取只在「确实检测到桌面」时发生，无桌面的板子不受影响。
         *
         * 演示模式下没有真实设备（fd = -1），EVIOCGRAB 必然失败，直接跳过。 */
        {
            int want_grab = cfg->touch_grab;
            int auto_grab = 0;

            if (!want_grab && !cfg->no_touch_grab &&
                !g_gui.touch.demo && gui_desktop_running())
            {
                want_grab = 1;
                auto_grab = 1;
            }

            if (want_grab)
            {
                if (g_gui.touch.demo)
                {
                    V("[GUI] 演示模式没有真实触摸设备，跳过独占抓取\n");
                }
                else
                {
                    int grc = uitouch_grab(&g_gui.touch);

                    if (grc == 0)
                    {
                        grabbed = 1;
                        if (auto_grab)
                        {
                            V("[GUI] 检测到桌面环境在运行，已**自动**独占抓取触摸设备\n"
                                   "      （否则触摸事件会被桌面吃掉，表现就是「点了没反应」）\n"
                                   "      桌面在这期间收不到触摸；本程序退出时自动释放。\n"
                                   "      不想这样请加 --no-touch-grab，或先停掉桌面。\n");
                        }
                        else
                        {
                            V("[GUI] 已用 EVIOCGRAB 独占触摸设备"
                                   "（退出时会自动释放；期间桌面收不到触摸）\n");
                        }
                    }
                    else
                    {
                        fprintf(stderr,
                                "[GUI] 独占抓取触摸失败：%s\n"
                                "      触摸会被桌面抢走，界面点了没反应。\n"
                                "      建议：① 先停掉桌面再跑本程序（推荐）；\n"
                                "            ② 确认以 root 运行后重试 --touch-grab。\n",
                                strerror(-grc));
                    }
                }
            }
        }

        /* 没抓到独占权、又确实有桌面：把处置办法再讲一遍 */
        if (!grabbed && gui_desktop_running())
        {
            /* 桌面冲突提示已删除（见上） */
        }
    }
    else
    {
        g_gui.has_touch = 0;
        fprintf(stderr,
                "[GUI] 打开触摸设备失败：%s\n"
                "      界面仍会正常显示，但无法通过触摸切页。\n"
                "      排查：cat /proc/bus/input/devices 看触摸节点，\n"
                "            再用 --touch /dev/input/eventX 显式指定。\n",
                strerror(errno));
    }

    /* ---- 线程 ---- */
    g_gui.quit = 0;
    rc = pthread_create(&g_gui.tid, NULL, gui_thread, NULL);
    if (rc != 0)
    {
        fprintf(stderr, "[GUI] 创建界面线程失败：%s\n", strerror(rc));
        fb_close(&g_gui.fb);
        if (g_gui.has_touch)
        {
            uitouch_close(&g_gui.touch);
        }
        return -3;
    }

    g_gui.running = 1;
    return 0;
}

void gui_app_stop(void)
{
    if (!g_gui.running)
    {
        return;
    }

    g_gui.quit = 1;
    pthread_join(g_gui.tid, NULL);

    /* 【退出前补存一次会话】
     *
     * 正常路径上，一次充电结束（BMS 发 BST 或回到空闲态）时 snapshot_build()
     * 会调 curve_finish() 把这次会话写进 charge_session 表。
     * 但如果程序是在充电**进行中**被结束的（Ctrl+C / kill / 关机），状态机
     * 还没走到空闲，charge_is_over() 一直是假 —— 这次充电就一条记录都不留，
     * 历史九宫格里凭空少一次。
     *
     * 现场最典型的场景就是"中途拔枪"：不想充了，直接停掉程序。那种情况下
     * 这次充电恰恰最该被记下来（它停在多少电量是有意义的）。
     *
     * 所以在这里再存一次。curve_finish() 内部用 cur_saved 去重，
     * 正常结束已经存过的不会再存第二遍。
     *
     * 必须放在 pthread_join 之后：线程停了，g_gui.curve 的数据才不会再变。
     * 落下的是"中途结束"（info.full 取 g_gui.cur_full，没充到 100% 就是 0），
     * 正是我们想要的。 */
    curve_finish();

    if (g_gui.has_touch)
    {
        uitouch_close(&g_gui.touch);
    }
    fb_close(&g_gui.fb);

    g_gui.running = 0;
    g_gui.has_touch = 0;
}

int gui_app_is_active(void)
{
    return g_gui.running;
}
