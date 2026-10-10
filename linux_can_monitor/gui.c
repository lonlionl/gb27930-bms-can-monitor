/**
 * @file    gui.c
 * @brief   CAN 监控终端图形界面实现（framebuffer 直接绘制）
 *
 * 布局说明
 * --------
 * 界面按 480x272 设计（野火 4.3 寸 RGB 屏的实际分辨率）。
 * 若屏幕更大（例如 800x480），整体内容会居中显示而不是拉伸，
 * 这样文字始终是清晰的 16 像素点阵，不会被插值糊掉。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "gui.h"
#include "font16.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/*==============================================================================
 *                              布局常量（设计基准 480x272）
 *============================================================================*/

/* 设计基准尺寸取自 gui.h，避免和 gui_app.c 里的虚拟屏尺寸不一致 */
#define DW              GUI_DESIGN_W    /* 设计宽度 */
#define DH              GUI_DESIGN_H    /* 设计高度 */

#define TITLE_H         28      /* 标题栏高度 */
#define MARGIN          8       /* 左右边距 */

/* 标题栏按钮尺寸。
 * 「历史曲线」是 4 个汉字（4*16=64 像素），「返回」只有 2 个汉字（32 像素）。
 * 之前两个按钮都用同一个 88 像素宽，历史界面那个明显虚胖 ——
 * 手指点在文字右边老远也会返回。现在各按各的文案宽度来。 */
#define BTN_W_ACTION    88      /* 主界面「历史曲线」 */
#define BTN_W_BACK      64      /* 历史界面「返回」 */
#define BTN_H           20
#define BTN_GAP         6

#define ROW_V           56      /* 第一条数据行的 y */
#define ROW_STEP        26      /* 数据行间距 */

#define BAR_X           168     /* 进度条起点 x */
#define BAR_W           190
#define BAR_H           14

#define HIST_X0         52      /* 曲线区左边界 */
#define HIST_Y0         50
#define HIST_X1         472
#define HIST_Y1         200

static int g_ox = 0;            /* 内容偏移（大屏居中用） */
static int g_oy = 0;
static int g_w  = DW;
static int g_h  = DH;
static int g_screen_w = DW;
static int g_screen_h = DH;

/*==============================================================================
 *                              工具
 *============================================================================*/

static void rect_fill_border(fbdev_t *fb, int x, int y, int w, int h,
                             uint32_t fill, uint32_t border)
{
    fb_fill(fb, x, y, w, h, fill);
    fb_rect(fb, x, y, w, h, border);
}

/** 画一个指定填充色的标题栏按钮（用于「停止」这种需要醒目配色的） */
static void draw_button_ex(fbdev_t *fb, int x, int y, int w, int h,
                           const char *label, uint32_t fill)
{
    rect_fill_border(fb, x, y, w, h, fill, FB_COL_TEXT);
    fb_text_center(fb, x + w / 2, y + (h - (int)FONT_CELL) / 2, label,
                   FB_COL_BG);
}

/** 画一个标题栏按钮，返回其矩形 */
static void draw_button(fbdev_t *fb, int x, int y, int w, int h,
                        const char *label, int active)
{
    uint32_t fill   = active ? FB_COL_BLUE : FB_COL_PANEL;
    uint32_t border = active ? FB_COL_CYAN : FB_COL_BORDER;

    rect_fill_border(fb, x, y, w, h, fill, border);
    fb_text_center(fb, x + w / 2, y + (h - (int)FONT_CELL) / 2, label,
                   active ? FB_COL_BG : FB_COL_TEXT);
}

/** 带标签与单位的数值行：label  value  [bar]  extra */
static void draw_metric_row(fbdev_t *fb, int y,
                            const char *label, const char *value,
                            double ratio, uint32_t bar_color,
                            const char *extra)
{
    fb_text(fb, g_ox + MARGIN, g_oy + y, label, FB_COL_DIM);
    fb_text(fb, g_ox + 80, g_oy + y, value, FB_COL_TEXT);

    if (ratio >= 0.0)
    {
        fb_bar_h(fb, g_ox + BAR_X, g_oy + y + 1, BAR_W, BAR_H,
                 ratio, bar_color, FB_COL_PANEL);
    }
    if (extra != NULL)
    {
        fb_text(fb, g_ox + BAR_X + BAR_W + 10, g_oy + y, extra, FB_COL_DIM);
    }
}

/** 把秒数格式化成 HH:MM:SS */
static void fmt_hms(uint32_t sec, char *out, size_t cap)
{
    snprintf(out, cap, "%02u:%02u:%02u",
             (unsigned)(sec / 3600u), (unsigned)((sec / 60u) % 60u), (unsigned)(sec % 60u));
}

/** 把字节数格式化成人类可读 */
static void fmt_size(uint64_t bytes, char *out, size_t cap)
{
    if (bytes >= 1048576ull)
    {
        snprintf(out, cap, "%.1fMB", (double)bytes / 1048576.0);
    }
    else if (bytes >= 1024ull)
    {
        snprintf(out, cap, "%.0fKB", (double)bytes / 1024.0);
    }
    else
    {
        snprintf(out, cap, "%lluB", (unsigned long long)bytes);
    }
}

/*==============================================================================
 *                              初始化
 *============================================================================*/

void gui_init(const fbdev_t *fb)
{
    if (fb == NULL)
    {
        return;
    }
    g_screen_w = fb->w;
    g_screen_h = fb->h;
    g_w = fb->w;
    g_h = fb->h;

    /* 比设计尺寸大的屏：内容居中；比设计尺寸小的屏：从左上角开始画 */
    g_ox = (fb->w > DW) ? (fb->w - DW) / 2 : 0;
    g_oy = (fb->h > DH) ? (fb->h - DH) / 2 : 0;
}

/*==============================================================================
 *                              主界面
 *============================================================================*/

void gui_draw_dashboard(fbdev_t *fb, const gui_data_t *d)
{
    char buf[96];
    char buf2[64];
    int  y;
    uint32_t state_color;
    double lim_v, lim_i;

    if (fb == NULL || d == NULL)
    {
        return;
    }

    fb_clear(fb, FB_COL_BG);

    /*----------------------------- 标题栏 -----------------------------*/
    fb_fill(fb, g_ox, g_oy, g_w, TITLE_H, FB_COL_PANEL);
    fb_hline(fb, g_ox, g_oy + TITLE_H - 1, g_w, FB_COL_BORDER);

    fb_text(fb, g_ox + MARGIN, g_oy + (TITLE_H - (int)FONT_CELL) / 2,
            "GB/T 27930 充电监控", FB_COL_CYAN);

    /* 右上角两个按钮：「数据曲线」+「充电」。
     * 几何全部取自 gui_button_rect()，和命中判定共用同一份，不可能对不上。 */
    {
        int bx, by, bw, bh;

        gui_button_rect(GUI_SCREEN_DASHBOARD, GUI_BTN_DATACURVE,
                        &bx, &by, &bw, &bh);
        draw_button(fb, bx, by, bw, bh, "数据曲线", 0);

        gui_button_rect(GUI_SCREEN_DASHBOARD, GUI_BTN_CHARGE,
                        &bx, &by, &bw, &bh);
        /* 同一个按钮两种身份：
         *   没在充电 -> 显示「充电」并高亮（提示可以开始）
         *   正在充电 -> 显示「停止」并高亮成红色（提示可以中止） */
        if (d->charging_active)
        {
            draw_button_ex(fb, bx, by, bw, bh, "停止", FB_COL_RED);
        }
        else
        {
            draw_button(fb, bx, by, bw, bh, "充电",
                        (d->phase == GUI_PHASE_STANDBY ||
                         d->phase == GUI_PHASE_DISCHARGE) ? 1 : 0);
        }
    }

    /* 标题栏中间显示充电阶段色块 */
    switch (d->phase)
    {
        case GUI_PHASE_CHARGING:  state_color = FB_COL_GREEN;  break;
        case GUI_PHASE_FULL:      state_color = FB_COL_CYAN;   break;
        case GUI_PHASE_DISCHARGE: state_color = FB_COL_YELLOW; break;
        default:                  state_color = FB_COL_DIM;    break;
    }
    fb_fill(fb, g_ox + 200, g_oy + 10, 8, 8, state_color);

    /*----------------------------- 状态行 -----------------------------*/
    y = 34;
    fb_text(fb, g_ox + MARGIN, g_oy + y, "状态:", FB_COL_DIM);
    {
        const char *ph = (d->phase == GUI_PHASE_CHARGING)  ? "充电中" :
                         (d->phase == GUI_PHASE_FULL)      ? "充满"   :
                         (d->phase == GUI_PHASE_DISCHARGE) ? "放电中" : "待机";

        snprintf(buf2, sizeof(buf2), "%s / %s", ph,
                 d->state_name ? d->state_name : "-");
        fb_text(fb, g_ox + 80, g_oy + y, buf2,
                (d->phase == GUI_PHASE_CHARGING) ? FB_COL_GREEN :
                (d->phase == GUI_PHASE_FULL)     ? FB_COL_CYAN  : FB_COL_TEXT);
    }

    snprintf(buf, sizeof(buf), "会话 #%lu", (unsigned long)d->session_id);
    fb_text(fb, g_ox + 230, g_oy + y, buf, FB_COL_DIM);

    if (d->error_name != NULL && strcmp(d->error_name, "无") != 0)
    {
        snprintf(buf, sizeof(buf), "异常: %s", d->error_name);
        fb_text(fb, g_ox + 330, g_oy + y, buf, FB_COL_RED);
    }
    else
    {
        fb_text(fb, g_ox + 330, g_oy + y, "异常: 无", FB_COL_DIM);
    }

    /*----------------------------- 实时数据 -----------------------------*/
    lim_v = (d->limit_v > 1.0) ? d->limit_v : 600.0;
    lim_i = (d->limit_i > 1.0) ? d->limit_i : 250.0;

    if (d->has_data)
    {
        snprintf(buf, sizeof(buf), "%7.1f V", d->voltage);
        snprintf(buf2, sizeof(buf2), "上限 %.0fV", lim_v);
        draw_metric_row(fb, ROW_V, "总电压", buf,
                        d->voltage / lim_v, FB_COL_BLUE, buf2);

        snprintf(buf, sizeof(buf), "%7.1f A", d->current);
        snprintf(buf2, sizeof(buf2), "上限 %.0fA", lim_i);
        draw_metric_row(fb, ROW_V + ROW_STEP, "总电流", buf,
                        d->current / lim_i, FB_COL_ORANGE, buf2);

        snprintf(buf, sizeof(buf), "%7.1f %%", d->soc);
        draw_metric_row(fb, ROW_V + ROW_STEP * 2, "荷电状态", buf,
                        d->soc / 100.0, FB_COL_GREEN,
                        (d->soc >= 90.0) ? "接近满充" : "正常充电");

        /* 温度行 */
        snprintf(buf, sizeof(buf), "%5.1f C", d->temp_max);
        fb_text(fb, g_ox + MARGIN, g_oy + ROW_V + ROW_STEP * 3, "最高温度", FB_COL_DIM);
        fb_text(fb, g_ox + 80, g_oy + ROW_V + ROW_STEP * 3, buf,
                (d->temp_max >= 55.0) ? FB_COL_RED :
                (d->temp_max >= 45.0) ? FB_COL_YELLOW : FB_COL_GREEN);

        snprintf(buf, sizeof(buf), "最低 %5.1f C", d->temp_min);
        fb_text(fb, g_ox + 200, g_oy + ROW_V + ROW_STEP * 3, buf, FB_COL_DIM);

        /* 单体电压行 */
        snprintf(buf, sizeof(buf), "%6.3f V", d->cell_max_v);
        fb_text(fb, g_ox + MARGIN, g_oy + ROW_V + ROW_STEP * 4, "最高单体", FB_COL_DIM);
        fb_text(fb, g_ox + 80, g_oy + ROW_V + ROW_STEP * 4, buf, FB_COL_TEXT);

        snprintf(buf, sizeof(buf), "编号 %2d", d->cell_max_no);
        fb_text(fb, g_ox + 200, g_oy + ROW_V + ROW_STEP * 4, buf, FB_COL_DIM);

        fmt_hms(d->charge_sec, buf2, sizeof(buf2));
        snprintf(buf, sizeof(buf), "累计 %.1f kWh / %s", d->energy_kwh, buf2);
        fb_text(fb, g_ox + 290, g_oy + ROW_V + ROW_STEP * 4, buf, FB_COL_DIM);
    }
    else
    {
        fb_text(fb, g_ox + MARGIN, g_oy + ROW_V, "等待 BMS 上报数据 ...", FB_COL_DIM);
        fb_text(fb, g_ox + MARGIN, g_oy + ROW_V + ROW_STEP,
                "（BCP / BCL / BCS / BSM 尚未收到）", FB_COL_DIM);
    }

    /*----------------------------- 底部统计 -----------------------------*/
    y = 190;
    fb_hline(fb, g_ox + MARGIN, g_oy + y - 6, DW - MARGIN * 2, FB_COL_BORDER);

    snprintf(buf, sizeof(buf), "CAN %s  %u kbps   %s   收 %llu 帧   发 %llu 帧   错误 %llu",
             d->ifname ? d->ifname : "can0",
             (unsigned)(d->bitrate / 1000u),
             d->can_state ? d->can_state : "-",
             (unsigned long long)d->rx_frames,
             (unsigned long long)d->tx_frames,
             (unsigned long long)d->err_frames);
    fb_text(fb, g_ox + MARGIN, g_oy + y + 2, buf,
            (d->err_frames == 0) ? FB_COL_DIM : FB_COL_YELLOW);

    fmt_size(d->db_size, buf2, sizeof(buf2));
    snprintf(buf, sizeof(buf), "SQLite(WAL)  原始 %llu 帧   解析 %llu 条   库 %s",
             (unsigned long long)d->db_raw,
             (unsigned long long)d->db_charge, buf2);
    fb_text(fb, g_ox + MARGIN, g_oy + y + 24, buf, FB_COL_DIM);

    /* 底部提示条 */
    fb_fill(fb, g_ox, g_oy + DH - 20, g_w, 20, FB_COL_PANEL);
    fb_hline(fb, g_ox, g_oy + DH - 20, g_w, FB_COL_BORDER);
    fb_text(fb, g_ox + MARGIN, g_oy + DH - 18,
            "点右上角「数据曲线」看本次充电曲线", FB_COL_DIM);
}

/*==============================================================================
 *                          曲线绘制（数据曲线 / 历史共用）
 *
 * 横轴 = SOC（电量）0~100%
 * -------------------------
 *   用户要求：曲线只向右生长、画过的部分固定不动（不要心电图那样整条左移）。
 *   横轴的取值是**本次充电的电量增量**（当前 SOC − 起始 SOC），不是 SOC 的绝对值：
 *   起点永远对齐绘图区最左边，横轴用满整个宽度表示「从 0 充到 100%」。
 *   所以一次 45%->100% 的充电只占**左边** 55% 的宽度、右侧留空，
 *   而 0%->100% 的完整充电才会铺满。
 *============================================================================*/

/**
 * 把 Unix 微秒格式化成 "HH:MM"（横轴刻度用）
 * @note   定义必须放在 draw_curve_grid() 之前 —— 否则那里会先产生一个
 *         隐式声明（非 static），再遇到 static 定义就会报
 *         "static declaration follows non-static declaration"。
 */
static void fmt_hhmm(int64_t us, char *out, size_t cap)
{
    time_t    t = (time_t)(us / 1000000LL);
    struct tm tm;

    localtime_r(&t, &tm);
    snprintf(out, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/** 把 Unix 微秒格式化成 "HH:MM:SS"（横轴刻度用） */
static void fmt_hms8(int64_t us, char *out, size_t cap)
{
    time_t    t = (time_t)(us / 1000000LL);
    struct tm tm;

    localtime_r(&t, &tm);
    snprintf(out, cap, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

#define CURVE_X0        58      /* 曲线区左边界（左边留给刻度） */
#define CURVE_Y0        70      /* 上边留出"曲线切换按钮"那一行 */
#define CURVE_X1        472
#define CURVE_Y1        200

/** 横轴总刻度：把"充了多少电"分成 100 份（这个 100 不显示给用户） */
#define CURVE_SOC_FULL  1000    /* 100.0%，用 0.1% 为单位表示 */

/**
 * 把 SOC 映射到绘图区 x 坐标
 * --------------------------
 *   横轴 = **从本次充电开始算起，充进去了多少电**，满量程 100 格。
 *   所以：
 *     · 不管从 20% 还是 80% 开始充，起点永远是绘图区最左边
 *     · 从 0 充到 100 正好铺满整个横轴
 *     · 从 45 充到 100 只占左边 55%，右边 45% 是空的
 *
 *   横轴刻度上写的是**时间**（这个电量刻度本身不显示）。
 */
static int socgain_to_x(int soc_x10, int soc0_x10)
{
    int    span = CURVE_X1 - CURVE_X0;
    int    gain = soc_x10 - soc0_x10;

    if (gain < 0)              { gain = 0; }
    if (gain > CURVE_SOC_FULL) { gain = CURVE_SOC_FULL; }

    return g_ox + CURVE_X0 + span * gain / CURVE_SOC_FULL;
}

/*==============================================================================
 *                        曲线指标（一次只画一条，用按钮切换）
 *
 * 每条曲线都有一个**内定的纵坐标范围**（见下面的表）。
 * 这个范围是固定的，不跟着数据自动缩放 —— 因为自动缩放会把整段数据
 * 拉满整个高度，看着就是"一条从下到上的斜线"，完全没有形状可言。
 * 固定范围之后，电压从 480 涨到 584 就只占它该占的那一段高度。
 *============================================================================*/

/** 每条曲线的固定纵坐标范围；lo == hi 表示按数据自适应（没用到） */
static const struct
{
    const char *name;       /* 按钮上的字 */
    double      lo;         /* 纵轴下限（不显示为"坐标轴"，但要写在刻度上） */
    double      hi;         /* 纵轴上限 */
    const char *unit;       /* 刻度单位后缀 */
    uint32_t    color;      /* 曲线颜色 */
} g_metric[GUI_METRIC_COUNT] =
{
    { "电压 V",  400.0, 600.0, "V", FB_COL_BLUE   },
    { "电流 A",    0.0, 120.0, "A", FB_COL_ORANGE },
    { "温度 C",    0.0,  60.0, "C", FB_COL_YELLOW },
    { "电量 %",    0.0, 100.0, "%", FB_COL_GREEN  },
};

/* ---- 曲线切换按钮：一排四个，占满宽度 ---- */
#define MBTN_W      110
#define MBTN_H      18
#define MBTN_GAP    5
#define MBTN_X0     8
#define MBTN_Y      48

/** 画一排指标切换按钮（当前选中的高亮） */
static void draw_metric_bar(fbdev_t *fb, gui_metric_t m)
{
    int i;

    for (i = 0; i < GUI_METRIC_COUNT; i++)
    {
        int  bx = MBTN_X0 + i * (MBTN_W + MBTN_GAP);
        int  sel = ((int)m == i);

        rect_fill_border(fb, g_ox + bx, g_oy + MBTN_Y, MBTN_W, MBTN_H,
                         sel ? FB_COL_BLUE : FB_COL_PANEL,
                         sel ? FB_COL_TEXT : FB_COL_BORDER);
        fb_text_center(fb, g_ox + bx + MBTN_W / 2,
                       g_oy + MBTN_Y + (MBTN_H - (int)FONT_CELL) / 2,
                       g_metric[i].name, sel ? FB_COL_BG : FB_COL_DIM);
    }
}

/**
 * @brief  点在哪个指标按钮上
 * @return 0..3（电压/电流/温度/电量）；未命中返回 -1
 */
int gui_metric_button_at(int x, int y)
{
    int i;
    int bx = x - g_ox;
    int by = y - g_oy;

    if (by < MBTN_Y || by >= MBTN_Y + MBTN_H) { return -1; }

    for (i = 0; i < GUI_METRIC_COUNT; i++)
    {
        int x0 = MBTN_X0 + i * (MBTN_W + MBTN_GAP);

        if (bx >= x0 && bx < x0 + MBTN_W) { return i; }
    }
    return -1;
}

/**
 * 网格 + 左侧刻度（按当前指标）+ 底部"开始->结束"时间角标
 *
 * 底部**不再画时间刻度**：横轴不是真正的时间轴，用户要的只是
 * 一行 "12:10:12->12:12:12" 说明这次充电是从几点几分到几点几分，
 * 正在充电时右边那个时间随真实时间一直走。
 */
static void draw_curve_grid(fbdev_t *fb, const gui_curve_t *c, gui_metric_t m)
{
    int  i;
    char lbl[48];

    for (i = 0; i <= 4; i++)
    {
        int y = CURVE_Y0 + (CURVE_Y1 - CURVE_Y0) * i / 4;
        int k;

        for (k = CURVE_X0; k < CURVE_X1; k += 6)
        {
            fb_hline(fb, g_ox + k, g_oy + y, 3, FB_COL_BORDER);
        }
    }

    /* 左侧：当前指标的固定刻度 */
    for (i = 0; i <= 4; i++)
    {
        int    y = CURVE_Y0 + (CURVE_Y1 - CURVE_Y0) * i / 4;
        double v = g_metric[m].hi -
                   (g_metric[m].hi - g_metric[m].lo) * (double)i / 4.0;

        snprintf(lbl, sizeof(lbl), "%.0f", v);
        fb_text(fb, g_ox + 4, g_oy + y - (int)FONT_CELL / 2, lbl, FB_COL_DIM);
    }

    /* 底部：一行 "开始->结束"（正在充电时右边就是当前时刻） */
    if (c != NULL && c->n > 0)
    {
        char t0[16], t1[16];

        fmt_hms8(c->ts[0], t0, sizeof(t0));
        fmt_hms8(c->ts[c->n - 1], t1, sizeof(t1));
        snprintf(lbl, sizeof(lbl), "%s->%s", t0, t1);

        fb_text_center(fb, g_ox + (CURVE_X0 + CURVE_X1) / 2,
                       g_oy + CURVE_Y1 + 3, lbl, FB_COL_TEXT);
    }
    else
    {
        fb_text_center(fb, g_ox + (CURVE_X0 + CURVE_X1) / 2,
                       g_oy + CURVE_Y1 + 3, "--:--:-->--:--:--", FB_COL_DIM);
    }

    fb_fill(fb, g_ox + CURVE_X0, g_oy + CURVE_Y0, 1, CURVE_Y1 - CURVE_Y0,
            FB_COL_BORDER);
    fb_hline(fb, g_ox + CURVE_X0, g_oy + CURVE_Y1,
             CURVE_X1 - CURVE_X0, FB_COL_BORDER);
}

/**
 * @brief  画当前指标那一条曲线
 * @param  soc       每个点的 SOC（%）
 * @param  soc0_x10  本次充电的起始 SOC（0.1%），对应绘图区最左边
 * @param  val       数值序列（电压/电流/温度/电量）
 * @note   纵轴用 g_metric[] 里**固定**的范围，不随数据缩放。
 */
static void draw_series_metric(fbdev_t *fb, const double *soc, int soc0_x10,
                               const double *val, int n, gui_metric_t m,
                               int *xs, int *ys)
{
    double lo   = g_metric[m].lo;
    double hi   = g_metric[m].hi;
    double span = hi - lo;
    int    k;
    int    cnt  = 0;

    if (n <= 0) { return; }
    if (span <= 1e-9) { span = 1.0; }

    for (k = 0; k < n && cnt < GUI_MAX_POINTS; k++)
    {
        double t = (val[k] - lo) / span;
        int    y;

        if (t < 0.0) { t = 0.0; }       /* 超出范围就贴着边画，不裁掉 */
        if (t > 1.0) { t = 1.0; }

        y = CURVE_Y1 - (int)(t * (double)(CURVE_Y1 - CURVE_Y0));

        xs[cnt] = socgain_to_x((int)(soc[k] * 10.0 + 0.5), soc0_x10);
        ys[cnt] = g_oy + y;
        cnt++;
    }

    if (cnt >= 2)      { fb_polyline(fb, xs, ys, cnt, g_metric[m].color); }
    else if (cnt == 1) { fb_pixel(fb, xs[0], ys[0], g_metric[m].color); }
}

/*==============================================================================
 *                          时间格式化
 *============================================================================*/



/*==============================================================================
 *                          标题栏与按钮
 *============================================================================*/

/**
 * @brief  取某个按钮在标题栏里的矩形
 * @note   画面和命中判定都走这里，两者不可能对不上。
 */
void gui_button_rect(gui_screen_t screen, gui_btn_t btn,
                     int *x, int *y, int *w, int *h)
{
    int right = DW - MARGIN;            /* 最右边按钮的右边界 */
    int ty    = (TITLE_H - BTN_H) / 2;
    int bw    = 0;
    int bx    = 0;

    (void)screen;

    switch (btn)
    {
        case GUI_BTN_BACK:              /* 各页最右边 */
            bw = BTN_W_BACK;
            bx = right - bw;
            break;

        case GUI_BTN_HISTLIST:          /* 数据曲线页：历史（在返回左边） */
            bw = 70;
            bx = right - BTN_W_BACK - BTN_GAP - bw;
            break;

        case GUI_BTN_CHARGE:            /* 主界面：充电（最右） */
            bw = 64;
            bx = right - bw;
            break;

        case GUI_BTN_DATACURVE:         /* 主界面：数据曲线 */
            bw = 88;
            bx = right - 64 - BTN_GAP - bw;
            break;

        default:
            bw = 0;
            bx = right;
            break;
    }

    if (x != NULL) { *x = g_ox + bx; }
    if (y != NULL) { *y = g_oy + ty; }
    if (w != NULL) { *w = bw; }
    if (h != NULL) { *h = BTN_H; }
}

/** 画标题栏 + 若干按钮 */
static void draw_title_bar(fbdev_t *fb, gui_screen_t screen, const char *title)
{
    gui_btn_t btns[3];
    int       n = 0;
    int       i;

    fb_fill(fb, g_ox, g_oy, g_w, TITLE_H, FB_COL_PANEL);
    fb_hline(fb, g_ox, g_oy + TITLE_H - 1, g_w, FB_COL_BORDER);
    fb_text(fb, g_ox + MARGIN, g_oy + (TITLE_H - (int)FONT_CELL) / 2,
            title, FB_COL_CYAN);

    if (screen == GUI_SCREEN_DASHBOARD)
    {
        btns[n++] = GUI_BTN_CHARGE;
        btns[n++] = GUI_BTN_DATACURVE;
    }
    else if (screen == GUI_SCREEN_DATACURVE)
    {
        btns[n++] = GUI_BTN_HISTLIST;
        btns[n++] = GUI_BTN_BACK;
    }
    else
    {
        btns[n++] = GUI_BTN_BACK;
    }

    for (i = 0; i < n; i++)
    {
        int bx, by, bw, bh;

        gui_button_rect(screen, btns[i], &bx, &by, &bw, &bh);
        draw_button(fb, bx, by, bw, bh,
                    (btns[i] == GUI_BTN_BACK)      ? "返回" :
                    (btns[i] == GUI_BTN_CHARGE)    ? "充电" :
                    (btns[i] == GUI_BTN_HISTLIST)  ? "历史" : "数据曲线", 0);
    }
}

/*==============================================================================
 *                          数据曲线页（本次充电）
 *============================================================================*/

void gui_draw_datacurve(fbdev_t *fb, const gui_data_t *d,
                        const gui_curve_t *c, int has_c, gui_metric_t m)
{
    static int xs[GUI_MAX_POINTS];
    static int ys[GUI_MAX_POINTS];
    char  buf[64];
    int    n;

    /* 整屏清一次底色。
     * 不清的话，上一个页面画过的数字/参数会原样留在屏幕上 ——
     * 这就是现场看到的"点进数据曲线还能看到主界面的读数"。
     * 三个非主界面页面都必须先清。 */
    fb_clear(fb, FB_COL_BG);

    draw_title_bar(fb, GUI_SCREEN_DATACURVE, "数据曲线");

    n = has_c ? c->n : 0;

    /* 标题栏下面一行：阶段 + SOC 区间 + 电量 + 时长 */
    {
        const char *ph = "待机";
        char        t0[16] = "--:--:--";
        char        t1[16] = "--:--:--";

        if (d != NULL)
        {
            switch (d->phase)
            {
                case GUI_PHASE_CHARGING:  ph = "充电中"; break;
                case GUI_PHASE_FULL:      ph = "充满";   break;
                case GUI_PHASE_DISCHARGE: ph = "放电中"; break;
                default:                  ph = "待机";   break;
            }
        }

        if (n > 0)
        {
            /* 角标用带秒的格式，和用户要求的 "12:10:10 -> 12:11:10" 一致；
             * 正在充电时右边就是当前时刻，一直在走。 */
            fmt_hms8(c->ts_first, t0, sizeof(t0));
            fmt_hms8(c->ts_last,  t1, sizeof(t1));
        }

        /* 只留阶段。
         * 时间在下面横坐标那一行已经有了，这里再来一遍是重复；
         * 采样点数属于内部实现细节，也不该上屏。 */
        snprintf(buf, sizeof(buf), "本次: %s", ph);
        fb_text(fb, g_ox + MARGIN, g_oy + 31, buf, FB_COL_DIM);
    }

    /* 一排四个切换按钮：页面不变，只换曲线 */
    draw_metric_bar(fb, m);

    if (n == 0)
    {
        draw_curve_grid(fb, NULL, m);
        fb_text_center(fb, g_ox + (CURVE_X0 + CURVE_X1) / 2,
                       g_oy + (CURVE_Y0 + CURVE_Y1) / 2,
                       "尚未开始充电，点主界面「充电」", FB_COL_DIM);
    }
    else
    {
        int soc0 = (int)(c->soc[0] * 10.0 + 0.5);   /* 起始电量 -> 绘图区最左边 */
        const double *val;

        switch (m)
        {
            case GUI_METRIC_I:   val = c->i;    break;
            case GUI_METRIC_T:   val = c->temp; break;
            case GUI_METRIC_SOC: val = c->soc;  break;
            default:             val = c->v;    break;
        }

        draw_curve_grid(fb, c, m);
        draw_series_metric(fb, c->soc, soc0, val, n, m, xs, ys);
    }

    /* 底部统计 */

    if (n > 0)
    {
        const double *val = (m == GUI_METRIC_I)   ? c->i :
                            (m == GUI_METRIC_T)   ? c->temp :
                            (m == GUI_METRIC_SOC) ? c->soc : c->v;
        double v0 = val[0];
        double v1 = val[n - 1];

        /* 电量这一条：结束值取本次见过的最高电量。
         * 只看末点的话，充满后 BMS 收尾那一帧可能已经掉到 99.x，
         * 屏上就会显示 "99" 而不是 100。 */
        if (m == GUI_METRIC_SOC && c->soc_max > v1) { v1 = c->soc_max; }

        snprintf(buf, sizeof(buf),
                 "电量 %.2f kWh    %s 实测 %.1f -> %.1f",
                 c->energy_kwh, g_metric[m].name, v0, v1);
        fb_text(fb, g_ox + MARGIN, g_oy + DH - 18, buf, FB_COL_TEXT);
    }
    else
    {
        fb_text(fb, g_ox + MARGIN, g_oy + DH - 18,
                "充电开始后这里会实时画出曲线", FB_COL_DIM);
    }
}

/*==============================================================================
 *                          历史九宫格
 *============================================================================*/

#define CELL_W      152
#define CELL_H      74
#define CELL_GAP    5
#define GRID_X0     6
#define GRID_Y0     34

/** 取第 idx 个缩略图的外框矩形（idx = 0..8，0 在左上角） */
static void cell_rect(int idx, int *x, int *y)
{
    int r = idx / 3;
    int c = idx % 3;

    *x = GRID_X0 + c * (CELL_W + CELL_GAP);
    *y = GRID_Y0 + r * (CELL_H + CELL_GAP);
}

/**
 * @brief  触摸点在哪个缩略图里
 * @return 0..8；不在任何格子里返回 -1
 * @note   九宫格排序：0 = 左上 = **最近一次**，8 = 右下 = 最远一次。
 */
int gui_history_cell_at(int x, int y)
{
    int bx = x - g_ox;
    int by = y - g_oy;
    int i;

    for (i = 0; i < SESSION_MAX; i++)
    {
        int cx, cy;

        cell_rect(i, &cx, &cy);
        if (bx >= cx && bx < cx + CELL_W &&
            by >= cy && by < cy + CELL_H)
        {
            return i;
        }
    }
    return -1;
}

/**
 * @brief  画历史九宫格
 * @param  list 会话列表（新的在前，list[0] 是最近一次）
 * @param  n    有效条数
 */
void gui_draw_histgrid(fbdev_t *fb, const session_info_t *list, int n)
{
    int i;

    fb_clear(fb, FB_COL_BG);        /* 同上：不清就会残留上一个页面的内容 */

    draw_title_bar(fb, GUI_SCREEN_HISTORY, "历史充电记录");

    for (i = 0; i < SESSION_MAX; i++)
    {
        int  cx, cy;
        char lbl[64];

        cell_rect(i, &cx, &cy);

        if (i >= n)
        {
            /* 空格子：画个虚框提示"还没有记录" */
            fb_rect(fb, g_ox + cx, g_oy + cy, CELL_W, CELL_H, FB_COL_BORDER);
            fb_text_center(fb, g_ox + cx + CELL_W / 2,
                           g_oy + cy + CELL_H / 2 - (int)FONT_CELL / 2,
                           "—", FB_COL_BORDER);
            continue;
        }

        rect_fill_border(fb, g_ox + cx, g_oy + cy, CELL_W, CELL_H,
                         FB_COL_PANEL, FB_COL_BORDER);

        /* 缩略曲线：SOC 从起始画到结束，横轴同样按 0~100% 铺满 */
        {
            int    px0 = cx + 4;
            int    py0 = cy + 4;
            int    pw  = CELL_W - 8;
            int    ph  = CELL_H - 8 - 12;      /* 底下留 12 像素写时间 */
            int    s0  = list[i].soc_start_x10;
            int    s1  = list[i].soc_end_x10;
            int    span = s1 - s0;
            int    xs[2];
            int    ys[2];
            int    bx0, by0;
            char   v[48];

            if (span == 0) { span = 1; }

            /* ---- 缩略曲线：按**实际电量值**定位 ----
             *
             * 横坐标 = 电量 0~100% 铺满格宽，所以：
             *   从 50% 充到 75%  ->  曲线从格子正中间画到四分之三处
             *   从  0% 充到100%  ->  正好铺满整格
             * 一眼就能看出"这次是从多少充到多少"。
             * （只影响缩略图；数据曲线页/详情页的主图不变。） */
            bx0 = g_ox + px0 + 3;
            by0 = g_oy + py0 + ph - 2;

            xs[0] = bx0 + (pw - 6) * s0 / 1000;
            ys[0] = by0 - ph * s0 / 1000;
            xs[1] = bx0 + (pw - 6) * s1 / 1000;
            ys[1] = by0 - ph * s1 / 1000;

            fb_hline(fb, bx0, by0, pw - 6, FB_COL_BORDER);
            fb_polyline(fb, xs, ys, 2, FB_COL_GREEN);

            /* 起点 / 终点各立一根小柱：灰 = 起始电量，绿 = 结束电量 */
            {
                int bh;

                bh = ph * s0 / 1000;
                if (bh < 1) { bh = 1; }
                fb_fill(fb, xs[0], by0 - bh, 3, bh, FB_COL_DIM);

                bh = ph * s1 / 1000;
                if (bh < 1) { bh = 1; }
                fb_fill(fb, xs[1] - 3 < bx0 ? bx0 : xs[1] - 3, by0 - bh, 3, bh,
                        FB_COL_GREEN);
            }

            /* 四舍五入到整百分比：995 应该显示 100 而不是 99 */
            snprintf(v, sizeof(v), "%d%%->%d%%",
                     (s0 + 5) / 10, (s1 + 5) / 10);
            fb_text(fb, g_ox + px0 + 14, g_oy + py0, v, FB_COL_TEXT);
        }

        /* 底部：充电时间段 + 电量 */
        {
            char t0[16], t1[16];

            fmt_hhmm(list[i].start_us, t0, sizeof(t0));
            fmt_hhmm(list[i].end_us,   t1, sizeof(t1));

            snprintf(lbl, sizeof(lbl), "%s-%s  %.1fkWh",
                     t0, t1, (double)list[i].energy_wh / 1000.0);
            fb_text(fb, g_ox + cx + 4, g_oy + cy + CELL_H - 13,
                    lbl, FB_COL_DIM);
        }
    }
}

/*==============================================================================
 *                          历史详情页
 *============================================================================*/

void gui_draw_histdetail(fbdev_t *fb, const session_info_t *info,
                         const gui_curve_t *c, int has_c, gui_metric_t m)
{
    static int xs[GUI_MAX_POINTS];
    static int ys[GUI_MAX_POINTS];
    char   buf[64];
    char   t0[16], t1[16];
    int    n;

    fb_clear(fb, FB_COL_BG);        /* 同上 */

    draw_title_bar(fb, GUI_SCREEN_HISTDETAIL, "历史充电详情");

    n = has_c ? c->n : 0;

    if (info != NULL)
    {
        fmt_hms8(info->start_us, t0, sizeof(t0));
        fmt_hms8(info->end_us,   t1, sizeof(t1));

        /* 只留"记录号 + 结束方式"：时间在下面的横坐标上已经有了 */
        snprintf(buf, sizeof(buf), "#%d   %s",
                 info->id,
                 info->full ? "充满结束" : "中途结束");
        fb_text(fb, g_ox + MARGIN, g_oy + 31, buf, FB_COL_DIM);
    }

    draw_metric_bar(fb, m);

    if (n == 0)
    {
        draw_curve_grid(fb, NULL, m);
        fb_text_center(fb, g_ox + (CURVE_X0 + CURVE_X1) / 2,
                       g_oy + (CURVE_Y0 + CURVE_Y1) / 2,
                       "这条记录没有曲线数据", FB_COL_DIM);
    }
    else
    {
        int soc0 = (int)(c->soc[0] * 10.0 + 0.5);
        const double *val;

        switch (m)
        {
            case GUI_METRIC_I:   val = c->i;    break;
            case GUI_METRIC_T:   val = c->temp; break;
            case GUI_METRIC_SOC: val = c->soc;  break;
            default:             val = c->v;    break;
        }

        draw_curve_grid(fb, c, m);
        draw_series_metric(fb, c->soc, soc0, val, n, m, xs, ys);
    }



    if (info != NULL)
    {
        double v0 = 0.0, v1 = 0.0;

        if (n > 0)
        {
            const double *val = (m == GUI_METRIC_I)   ? c->i :
                                (m == GUI_METRIC_T)   ? c->temp :
                                (m == GUI_METRIC_SOC) ? c->soc : c->v;

            v0 = val[0];
            v1 = val[n - 1];
            if (m == GUI_METRIC_SOC && c->soc_max > v1) { v1 = c->soc_max; }
        }

        snprintf(buf, sizeof(buf),
                 "电量 %.2f kWh    %s 实测 %.1f -> %.1f",
                 (double)info->energy_wh / 1000.0, g_metric[m].name, v0, v1);
        fb_text(fb, g_ox + MARGIN, g_oy + DH - 18, buf, FB_COL_TEXT);
    }
}

/*==============================================================================
 *                          触摸命中测试
 *============================================================================*/

gui_btn_t gui_hit_test(gui_screen_t screen, int x, int y)
{
    gui_btn_t cand[3];
    int       n = 0;
    int       i;

    if (g_screen_w <= 0 || g_screen_h <= 0)
    {
        gui_init(NULL);
    }

    if (screen == GUI_SCREEN_DASHBOARD)
    {
        cand[n++] = GUI_BTN_CHARGE;
        cand[n++] = GUI_BTN_DATACURVE;
    }
    else if (screen == GUI_SCREEN_DATACURVE)
    {
        cand[n++] = GUI_BTN_HISTLIST;
        cand[n++] = GUI_BTN_BACK;
        cand[n++] = GUI_BTN_METRIC;
    }
    else if (screen == GUI_SCREEN_HISTORY)
    {
        cand[n++] = GUI_BTN_BACK;
        /* 九宫格的缩略图单独判定（gui_history_cell_at） */
    }
    else
    {
        cand[n++] = GUI_BTN_BACK;
        cand[n++] = GUI_BTN_METRIC;     /* 历史详情页也能切曲线 */
    }

    for (i = 0; i < n; i++)
    {
        int bx, by, bw, bh;

        if (cand[i] == GUI_BTN_METRIC)
        {
            /* 指标按钮不是标题栏按钮，走的是一排四个矩形 */
            if (gui_metric_button_at(x, y) >= 0) { return GUI_BTN_METRIC; }
            continue;
        }

        gui_button_rect(screen, cand[i], &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh)
        {
            return cand[i];
        }
    }

    /* 九宫格页：点在格子里返回 GUI_BTN_CELL */
    if (screen == GUI_SCREEN_HISTORY)
    {
        if (gui_history_cell_at(x, y) >= 0) { return GUI_BTN_CELL; }
    }

    return GUI_BTN_NONE;
}
