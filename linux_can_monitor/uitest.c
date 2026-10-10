/**
 * @file    uitest.c
 * @brief   GUI 离线渲染自检（不需要 framebuffer 设备，也不需要开发板）
 *
 * 做什么
 * ------
 *   用 fb_open_virtual() 建一块纯内存 480x272 画布，把主界面和历史界面
 *   各渲染一遍，然后：
 *     ① 用 fb_dump_ascii() 把整屏降采样成字符画打到终端 —— 一眼就能看出
 *        元素位置、是否互相压盖、文字有没有画到框外面；
 *     ② 用 fb_dump_ppm() 导出 PPM，可在 PC 上转 PNG 目视；
 *     ③ 顺带检查所有 UI 文案里的汉字是否都在点阵字库里（缺字会变成空白，
 *        这种 bug 在板子上极难发现，这里必须拦住）。
 *
 * 用法:  ./uitest            终端字符画 + /tmp/ui_*.ppm
 */

#include "fbdev.h"
#include "gui.h"
#include "font16.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*---------------------------------------------------------------------------
 * 缺字检查：把字库里的码点做成一张查找表
 *-------------------------------------------------------------------------*/
static int glyph_missing(const char *utf8, char *miss, size_t cap)
{
    size_t n = 0;
    const char *p = utf8;

    while (*p != '\0')
    {
        unsigned char c = (unsigned char)*p;
        uint32_t cp;
        int i, found = 0;

        if (c < 0x80u)
        {
            p++;
            continue;
        }
        if ((c & 0xE0u) == 0xC0u)      { cp = ((uint32_t)(c & 0x1Fu) << 6)  | ((uint32_t)(p[1] & 0x3Fu)); p += 2; }
        else if ((c & 0xF0u) == 0xE0u) { cp = ((uint32_t)(c & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu); p += 3; }
        else                           { cp = ((uint32_t)(c & 0x07u) << 18) | ((uint32_t)(p[1] & 0x3Fu) << 12) | ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu); p += 4; }

        for (i = 0; i < (int)FONT_CJK_COUNT; i++)
        {
            if (g_font_cjk_map[i].codepoint == cp)
            {
                found = 1;
                break;
            }
        }
        if (!found && n + 8 < cap)
        {
            /* 把缺的码点用 UTF-8 写回去，方便定位 */
            if (cp < 0x800u)
            {
                miss[n++] = (char)(0xC0u | (cp >> 6));
                miss[n++] = (char)(0x80u | (cp & 0x3Fu));
            }
            else
            {
                miss[n++] = (char)(0xE0u | (cp >> 12));
                miss[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                miss[n++] = (char)(0x80u | (cp & 0x3Fu));
            }
        }
    }
    miss[n] = '\0';
    return (int)n;
}

/*---------------------------------------------------------------------------
 * 构造测试数据
 *-------------------------------------------------------------------------*/
static void fill_demo(gui_data_t *d)
{
    memset(d, 0, sizeof(*d));
    d->state_name = "充电中(CHARGING)";
    d->error_name = "无";
    d->session_id = 1;
    d->charging   = 1;
    d->has_data   = 1;
    d->voltage    = 512.3;
    d->current    = 98.7;
    d->soc        = 62.5;
    d->cell_max_v = 3.412;
    d->cell_max_no = 12;
    d->temp_max   = 38.0;
    d->temp_min   = 31.0;
    d->limit_v    = 584.0;
    d->limit_i    = 100.0;
    d->energy_kwh = 12.3;
    d->charge_sec = 3725;
    d->ifname     = "can0";
    d->can_state  = "ERROR_ACTIVE";
    d->bitrate    = 250000;
    d->rx_frames  = 12345;
    d->tx_frames  = 4210;
    d->err_frames = 0;
    d->db_raw     = 123456;
    d->db_charge  = 3600;
    d->db_size    = 4u * 1024u * 1024u;
}

static void fill_curve(gui_curve_t *c)
{
    int i;
    memset(c, 0, sizeof(*c));
    c->n = 200;
    for (i = 0; i < c->n; i++)
    {
        double t = (double)i / (double)(c->n - 1);
        /* 恒流段 -> 恒压段的典型充电曲线 */
        c->soc[i]    = 45.0 + 55.0 * t;
        c->v[i]      = 480.0 + 104.0 * t;
        c->i[i]      = (t < 0.85) ? 100.0 : 100.0 * (1.0 - (t - 0.85) / 0.15) + 8.0;
        c->temp[i]   = 25.0 + 14.0 * t;
    }
    c->ts_first   = 1700000000000000LL;
    c->ts_last    = c->ts_first + 3725LL * 1000000LL;
    c->energy_kwh = 12.3;
}

/*---------------------------------------------------------------------------
 * 所有 UI 文案（用于缺字检查）
 *-------------------------------------------------------------------------*/
static const char *UI_TEXT[] = {
    "GB/T 27930 充电监控", "历史曲线", "返回",
    "状态:", "会话 #", "异常: ", "异常: 无",
    "总电压", "总电流", "荷电状态", "最高温度", "最低温度", "最高单体", "编号",
    "上限", "V", "A", "%", "C", "接近满充", "正常充电",
    "累计", "kWh", "等待 BMS 上报数据 ...",
    "（BCP / BCL / BCS / BSM 尚未收到）",
    "CAN", "kbps", "收", "发", "错误", "帧",
    "SQLite(WAL)", "原始", "解析", "条", "库",
    "点右上角「数据曲线」看本次充电曲线",
    "历史电量变化曲线", "电压 V", "电流 A", "SOC %", "采样", "点",
    "暂无历史数据", "->", "温度", "积分电量", "点击「返回」回到监控",
    NULL
};

int main(int argc, char **argv)
{
    fbdev_t     fb;
    gui_data_t  d;
    gui_curve_t c;
    int         i;
    int         missing_total = 0;
    int         hit_fail = 0;

    /*========== ① 缺字检查 ==========*/
    printf("==================== 点阵字库覆盖检查 ====================\n");
    for (i = 0; UI_TEXT[i] != NULL; i++)
    {
        char miss[128];
        if (glyph_missing(UI_TEXT[i], miss, sizeof(miss)) > 0)
        {
            printf("  [缺字] \"%s\" 缺少: %s\n", UI_TEXT[i], miss);
            missing_total++;
        }
    }
    if (missing_total == 0)
    {
        printf("  全部 UI 文案的汉字都在字库中 (ASCII %u + 汉字 %u)\n",
               (unsigned)FONT_ASCII_COUNT, (unsigned)FONT_CJK_COUNT);
    }
    else
    {
        printf("  共 %d 处缺字，请把缺失的汉字补进 tools/gen_font.py 的 CJK_TEXT "
               "后重新生成 font16.h\n", missing_total);
    }

    /*========== ② 离线渲染 ==========*/
    if (fb_open_virtual(&fb, 480, 272) != 0)
    {
        fprintf(stderr, "fb_open_virtual 失败\n");
        return 1;
    }
    gui_init(&fb);

    fill_demo(&d);
    fill_curve(&c);

    gui_draw_dashboard(&fb, &d);
    printf("\n==================== 主界面（实时监控 480x272）====================\n");
    fb_dump_ascii(&fb, 118);
    if (fb_dump_ppm(&fb, "/tmp/ui_dashboard.ppm") != 0)
    {
        fprintf(stderr, "导出 ppm 失败\n");
    }

    gui_draw_datacurve(&fb, &d, &c, 1, GUI_METRIC_V);
    printf("\n==================== 数据曲线页（本次充电，横轴 = SOC）====================\n");
    fb_dump_ascii(&fb, 118);
    fb_dump_ppm(&fb, "/tmp/ui_history.ppm");

    /*========== ③ 触摸命中测试 ==========
     *
     * 逐像素扫描每个界面，确认「命中区 == 画出来的矩形」。
     * 这样"看着是这么宽、实际点一大片"这类问题测试里会直接暴露，
     * 不用靠手指去猜。
     */
    printf("\n==================== 触摸命中测试 ====================\n");
    {
        struct { gui_screen_t sc; gui_btn_t btn; const char *name; } cases[] = {
            { GUI_SCREEN_DASHBOARD,  GUI_BTN_CHARGE,     "主界面-充电"     },
            { GUI_SCREEN_DASHBOARD,  GUI_BTN_DATACURVE,  "主界面-数据曲线" },
            { GUI_SCREEN_DATACURVE,  GUI_BTN_HISTLIST,   "数据曲线-历史"   },
            { GUI_SCREEN_DATACURVE,  GUI_BTN_BACK,       "数据曲线-返回"   },
            { GUI_SCREEN_HISTORY,    GUI_BTN_BACK,       "九宫格-返回"     },
            { GUI_SCREEN_HISTDETAIL, GUI_BTN_BACK,       "详情页-返回"     },
        };
        unsigned ci;
        int      bad = 0;

        for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++)
        {
            int bx = 0, by = 0, bw = 0, bh = 0;
            int x, y, inside = 0, leak = 0, missed = 0;

            gui_button_rect(cases[ci].sc, cases[ci].btn, &bx, &by, &bw, &bh);

            for (y = 0; y < fb.h; y++)
            {
                for (x = 0; x < fb.w; x++)
                {
                    int hit = (gui_hit_test(cases[ci].sc, x, y) == cases[ci].btn);
                    int in  = (x >= bx && x < bx + bw && y >= by && y < by + bh);

                    if (in) { inside++; if (!hit) { missed++; } }
                    else if (hit) { leak++; }
                }
            }

            printf("  %-16s x=%3d..%3d y=%2d..%2d  (%dx%d)  外溢 %d / 漏判 %d  %s\n",
                   cases[ci].name, bx, bx + bw - 1, by, by + bh - 1, bw, bh,
                   leak, missed,
                   (leak == 0 && missed == 0) ? "[OK]" : "[FAIL]");
            bad += leak + missed;
        }

        printf("  结论: %s\n", (bad == 0) ? "全部按钮的命中区与画面严格一致"
                                          : "存在不一致，请检查 gui_button_rect 的使用");
        if (bad != 0) { hit_fail = 1; }
    }

    /*========== ④ 九宫格 */
    printf("\n==================== 历史九宫格 ====================\n");
    {
        session_info_t list[SESSION_MAX];
        int i, cnt = 4;

        memset(list, 0, sizeof(list));
        for (i = 0; i < cnt; i++)
        {
            list[i].id            = 100 - i;
            list[i].start_us      = (int64_t)(1759000000 + i * 600) * 1000000LL;
            list[i].end_us        = (int64_t)(1759000600 + i * 600) * 1000000LL;
            list[i].soc_start_x10 = 200 + i * 150;
            list[i].soc_end_x10   = 1000;
            list[i].energy_wh     = 5100 - i * 400;
            list[i].charge_sec    = 3600 - i * 300;
            list[i].full          = 1;
            list[i].n             = 10;
        }

        gui_draw_histgrid(&fb, list, cnt);
        fb_dump_ascii(&fb, 118);
        fb_dump_ppm(&fb, "/tmp/ui_grid.ppm");

        /* 九宫格排序：0 = 左上 = 最近一次 */
        printf("  第 0 格（左上）对应 id=%d（应为最近一次 100）  %s\n",
               list[0].id, (list[0].id == 100) ? "[OK]" : "[FAIL]");
        /* 九宫格布局：(30,50)=左上第 0 格，(30,200)=左下第 6 格，
         * (2,2) 在标题栏里，不属于任何格子，应当返回 -1。 */
        printf("  格子命中: (30,50)->%d  (30,200)->%d  (2,2)->%d（应为 -1）\n",
               gui_history_cell_at(30, 50), gui_history_cell_at(30, 200),
               gui_history_cell_at(2, 2));
        if (gui_history_cell_at(30, 50)  != 0) { hit_fail = 1; }
        if (gui_history_cell_at(30, 200) != 6) { hit_fail = 1; }
        if (gui_history_cell_at(2, 2)    != -1) { hit_fail = 1; }
    }
    fb_close(&fb);
    printf("\n导出: /tmp/ui_dashboard.ppm  /tmp/ui_history.ppm\n");

    (void)argc;
    (void)argv;
    return (missing_total == 0 && hit_fail == 0) ? 0 : 2;
}
