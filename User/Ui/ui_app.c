/**
  ******************************************************************************
  * @file    ui_app.c
  * @brief   STM32 本地界面 —— 应用层实现
  *
  * 设计要点
  * --------
  * 1. **界面层不认识协议层**
  *    所有数据都通过 BMS_Protocol_GetUiSnapshot() 一次性取回（值拷贝），
  *    界面只做「格式化 + 画图」。这样协议层怎么改都不会影响界面，
  *    界面也不可能误改到状态机。
  *
  * 2. **不做动态内存分配**
  *    历史采样用固定长度环形数组（300 点 × 8 字节 = 2.4 KB），
  *    单片机项目里 malloc 是故障高发区，这里一个字节都不动态申请。
  *
  * 3. **刷新节奏与闪烁**
  *    主界面 2 Hz、历史界面 1 Hz —— 数字读数完全够用。
  *    绘制分「静态层 + 动态层」：标题栏/面板/固定标签只画一次，
  *    会变的数值各自带一块小矩形和一个"上次内容"备忘，只有内容真的
  *    变了才清那一小块并重画。这样稳态下每帧只改几百个像素，
  *    彻底避免了"整屏重绘 -> 每 0.5 秒闪一下"的现象。
  *
  * 4. **单位与缩放**
  *    协议快照里全是「缩放整数」（0.1 V / 0.1 A / 0.1 % / mV），
  *    显示时才插入小数点。全程不出现浮点，省掉 Keil 的浮点打印库
  *    （那一套要占 6~8 KB Flash）。
  ******************************************************************************
  */

#include "ui_app.h"
#include "ui_port.h"
#include "bms_protocol.h"
#include <string.h>
#include <stdio.h>

#if UI_ENABLE

/*==============================================================================
 *                              布局常量
 *   全部按 480x320 设计；若屏幕更宽（例如 800x480），
 *   UI_Init() 会把整体居中，不会出现「内容挤在左上角」。
 *============================================================================*/

#define UI_DSG_W            480         /* 设计宽度 */
#define UI_DSG_H            320         /* 设计高度 */

#define UI_TITLE_H          35          /* 标题栏高度 */
/* 【已废弃】右上角按钮的触摸热区。
 * 触摸与曲线界面已经整块删除，这四个宏没有任何引用点，留着只是
 * 让熟悉旧版的人能对上号。下次清理时可以一并删掉。 */
#define UI_BTN_X0           352
#define UI_BTN_Y0           4
#define UI_BTN_X1           474
#define UI_BTN_Y1           31

#define UI_METRIC_Y0        40          /* 三个主要读数区起始 y */
#define UI_METRIC_H         36          /* 每行高度（32 像素大字 + 4 间距）*/
#define UI_METRIC_LABEL_X   10          /* 标签 x */
#define UI_METRIC_VALUE_X   86          /* 大字 x */
#define UI_METRIC_BAR_X0    300         /* 进度条 x 范围 */
#define UI_METRIC_BAR_X1    470
#define UI_METRIC_VALUE_W   196         /* 大字数值占用的宽度（最多 6 位） */
#define UI_METRIC_UNIT_X    286         /* 单位固定画在这里，数值位数变化时不会左右跳 */

#define UI_INFO_Y0          152         /* 信息面板 */
#define UI_INFO_Y1          228
#define UI_INFO_LINE_H      18          /* 4 行文字加留白正好放进 152..228 */
#define UI_INFO_ROW0_Y      (UI_INFO_Y0 + 4)
#define UI_INFO_COL2_X      250         /* 第二列 x */

#define UI_CAN_Y0           232         /* CAN 面板 */
#define UI_CAN_Y1           (UI_DSG_H - 4)
#define UI_CAN_ROW0_Y       (UI_CAN_Y0 + 6)

/*==============================================================================
 *                              运行状态
 *
 * 【重要】本机界面只做「显示」——没有触摸、也没有曲线界面。
 *
 * 现场那块 GT1151 电容屏一直探测不到（软件 I2C 换了几种时序都不行），
 * 而这块屏的触摸对系统没有任何必要：STM32 这边是「电池」，
 * 电量、状态、报文统计全部都是被动上报的，不需要人去点它。
 * 曲线图更没必要 —— i.MX 那边有完整的数据曲线和历史九宫格。
 *
 * 所以这里把触摸和曲线整块删掉：
 *   · 界面只剩一个只读的主界面（静态层 + 动态层局部重绘）
 *   · ui_port.c 不再调用 touch.c，链接器会把整个触摸驱动剥掉
 *   · 省下的 Flash 很可观（触摸驱动 + 曲线绘制 + 300 点历史缓冲）
 *============================================================================*/

static uint32_t    s_last_draw_ms  = 0;
static uint16_t    s_off_x = 0;         /* 大屏居中偏移 */
static uint16_t    s_off_y = 0;
static uint8_t     s_inited = 0;

/** 界面自己的毫秒计时（由 UI_Tick 累加，不依赖 SysTick，避免与协议层抢时基）*/
static volatile uint32_t s_ui_ms = 0;

/*==============================================================================
 *                              小工具
 *============================================================================*/

/**
  * @brief  把缩放整数格式化成带一位小数的十进制串
  * @param  buf 输出缓冲（至少 8 字节）
  * @param  v   缩放整数，例如 4800 -> "480.0"
  * @return 字符串长度
  * @note   自己实现而不用 sprintf：Keil 的 printf 家族会额外吃掉 4~6 KB Flash，
  *         而这里只需要「整数 + 小数点」这一种格式。
  */
static uint8_t fmt_x10(char *buf, uint16_t v)
{
    char    tmp[6];
    uint8_t n = 0;
    uint8_t i;
    uint8_t len = 0;

    if (v == 0)
    {
        buf[0] = '0';
        buf[1] = '.';
        buf[2] = '0';
        buf[3] = '\0';
        return 3;
    }

    while (v > 0 && n < sizeof(tmp))
    {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    /* tmp 里是倒序：最低位在前。先输出除最后一位外的整数部分 */
    if (n == 1)                     /* 0.1 这种 */
    {
        buf[len++] = '0';
    }
    else
    {
        for (i = n; i > 1; i--)
        {
            buf[len++] = tmp[i - 1];
        }
    }

    buf[len++] = '.';
    buf[len++] = tmp[0];            /* 小数位 */
    buf[len]   = '\0';
    return len;
}

/** 把秒数格式化成 "HH:MM:SS"（超过 99 小时按 99 小时截断） */
static void fmt_hms(char *buf, uint32_t sec)
{
    uint32_t h = sec / 3600u;
    uint32_t m = (sec % 3600u) / 60u;
    uint32_t s = sec % 60u;

    if (h > 99u) { h = 99u; }

    buf[0] = (char)('0' + (h / 10u));
    buf[1] = (char)('0' + (h % 10u));
    buf[2] = ':';
    buf[3] = (char)('0' + (m / 10u));
    buf[4] = (char)('0' + (m % 10u));
    buf[5] = ':';
    buf[6] = (char)('0' + (s / 10u));
    buf[7] = (char)('0' + (s % 10u));
    buf[8] = '\0';
}

/** 把 0~99 的整数写成两位十进制（编号显示用） */
static void fmt_u2(char *buf, uint16_t v)
{
    if (v > 99u) { v = 99u; }
    buf[0] = (char)('0' + (v / 10u));
    buf[1] = (char)('0' + (v % 10u));
    buf[2] = '\0';
}

/**
  * @brief  取充电阶段名
  *
  * 这是屏幕上最该让人一眼看到的信息：
  *   待机 / 充电中 / 充满 / 放电中
  * 协议状态（握手、参数配置、结束…）对现场操作人员来说太细了，
  * 所以状态格改成显示阶段，协议状态仍然照常往串口日志打。
  */
static const char *phase_name(uint8_t ph)
{
    switch (ph)
    {
        case 1u: return "充电中";
        case 2u: return "充满";
        case 3u: return "放电中";
        default: return "待机";
    }
}

/** 阶段对应的指示色 */
static uint16_t phase_color(uint8_t ph)
{
    switch (ph)
    {
        case 1u: return UIP_GREEN;
        case 2u: return UIP_CYAN;
        case 3u: return UIP_YELLOW;
        default: return UIP_DIM;
    }
}

/** 坐标换算：把设计坐标(0..480, 0..320)映射到实际屏幕（居中） */
static uint16_t lx(uint16_t x) { return (uint16_t)(s_off_x + x); }
static uint16_t ly(uint16_t y) { return (uint16_t)(s_off_y + y); }

/*==============================================================================
 *                              公共部件
 *============================================================================*/

/*==============================================================================
 *                          局部重绘：脏值缓存
 *
 *   为什么需要它
 *   ------------
 *   最早的实现每 500 ms 把整屏（480x320 = 15 万像素）重画一遍：
 *   先清底色、再画面板、最后画字。清底和画字之间有一段"空白期"，
 *   再加上 16 位并口刷满一整屏本身就要几十毫秒 ——
 *   肉眼看就是"屏幕每半秒闪一下"。
 *
 *   现在的做法分两层：
 *     1. 静态层：标题栏、面板外框、所有固定标签，只在进入主界面时画一次；
 *     2. 动态层：只有会变的数值。每个数值有自己的一小块矩形，并且只在
 *        **内容真的变了**的时候才"清这个小矩形 + 重画这个字符串"。
 *
 *   稳态下每 500 ms 实际改动的像素只有几百个，闪烁基本看不出来了。
 *   代价是每个单元多存 20 字节的上次内容（共 18 个单元，360 字节 RAM）。
 *============================================================================*/

/** 会变化、需要单独跟踪的文本单元 */
typedef enum
{
    UICELL_VOLT = 0,        /* 总电压大字 */
    UICELL_CURR,            /* 总电流大字 */
    UICELL_SOC,             /* 荷电状态大字 */
    UICELL_CELL_NO,         /* 最高单体编号 */
    UICELL_TMAX,            /* 最高温度 */
    UICELL_TMIN,            /* 最低温度 */
    UICELL_CELL_MV,         /* 最高单体电压 */
    UICELL_LIMIT_V,         /* 允许电压上限 */
    UICELL_LIMIT_I,         /* 允许电流上限 */
    UICELL_ENERGY,          /* 累计电量 */
    UICELL_TIME,            /* 充电时长 */
    UICELL_SESSION,         /* 会话号 */
    UICELL_MODE,            /* 充电模式 */
    UICELL_CRO,             /* 充电机就绪状态 */
    UICELL_RX,              /* 接收帧数 */
    UICELL_TX,              /* 发送帧数 */
    UICELL_STATE,           /* 充电状态名 */
    UICELL_ERROR,           /* 异常信息 */
    UICELL_COUNT
} UI_Cell_t;

#define UI_CELL_TXT_MAX     24          /* 单个单元缓存的最大字符数 */
#define UI_BAR_NONE         (-100000)   /* "这个进度条还没画过" */

/** 文本单元上次画的内容：前 4 个字符存颜色，后面存文字 */
static char    s_cell_txt[UICELL_COUNT][UI_CELL_TXT_MAX];
static uint8_t s_cell_have = 0u;        /* 0 = 一帧都还没画过，全部按"脏"处理 */
static uint8_t s_static_ok = 0u;        /* 0 = 静态层需要重画 */

/** 三个进度条上次的值与颜色（用来判断要不要重画） */
static int32_t  s_bar_last[3]     = { UI_BAR_NONE, UI_BAR_NONE, UI_BAR_NONE };
static uint16_t s_bar_last_col[3] = { 0u, 0u, 0u };

/**
  * @brief  记录某个单元的内容，返回 1 表示和上次不同（需要重画）
  * @note   颜色也被拼进比较键：状态从"充电中"变"故障"时文字可能没变，
  *         但颜色必须变。只比文字就会漏掉这一帧。
  */
static uint8_t ui_cell_changed(UI_Cell_t id, uint16_t color, const char *txt)
{
    char key[UI_CELL_TXT_MAX];

    key[0] = (char)('A' + (uint8_t)((color >> 12) & 0x0Fu));
    key[1] = (char)('A' + (uint8_t)((color >> 8)  & 0x0Fu));
    key[2] = (char)('A' + (uint8_t)((color >> 4)  & 0x0Fu));
    key[3] = (char)('A' + (uint8_t)(color & 0x0Fu));
    strncpy(&key[4], txt, (size_t)(UI_CELL_TXT_MAX - 5));
    key[UI_CELL_TXT_MAX - 1] = '\0';

    if (s_cell_have != 0u && strcmp(s_cell_txt[id], key) == 0)
    {
        return 0u;
    }

    strncpy(s_cell_txt[id], key, (size_t)(UI_CELL_TXT_MAX - 1));
    s_cell_txt[id][UI_CELL_TXT_MAX - 1] = '\0';
    return 1u;
}

/**
  * @brief  画一个会变化的文本单元；内容没变就一个像素都不碰
  * @param  id     单元编号（用来记住上次画的内容）
  * @param  x,y    设计坐标（矩形左上角）
  * @param  w,h    该单元占用的矩形，用来擦掉上一次的旧内容
  * @param  txt    要显示的文字
  * @param  color  文字颜色
  * @param  bg     这个区域的底色（读数是屏幕底色，面板里是面板底色）
  * @param  mode   0 = 16 像素并裁剪到 w；1 = 16 像素；2 = 32 像素大字
  */
static void draw_cell(UI_Cell_t id, uint16_t x, uint16_t y,
                      uint16_t w, uint16_t h,
                      const char *txt, uint16_t color,
                      uint16_t bg, uint8_t mode)
{
    if (ui_cell_changed(id, color, txt) == 0u)
    {
        return;                     /* 内容和颜色都没变，跳过 */
    }

    /* 只清自己这一小块 —— 闪烁的来源就是"每次都清整屏" */
    uip_fill(lx(x), ly(y),
             lx((uint16_t)(x + w - 1u)), ly((uint16_t)(y + h - 1u)), bg);

    if (mode >= 2u)
    {
        (void)uip_text_scale(lx(x), ly(y), txt, color, 2u);
    }
    else if (mode == 0u)
    {
        uip_text_clip(lx(x), ly(y), w, txt, color);
    }
    else
    {
        (void)uip_text(lx(x), ly(y), txt, color);
    }
}

/**
  * @brief  画一条进度条；变化太小或者颜色没变就先不动它
  * @param  idx      0/1/2（三行读数各一条）
  * @param  y        读数行顶部的设计坐标
  * @param  permille 千分比 0~1000
  * @param  color    填充色
  */
static void draw_bar(uint8_t idx, uint16_t y, int32_t permille, uint16_t color)
{
    int32_t d;

    if (permille < 0)    { permille = 0; }
    if (permille > 1000) { permille = 1000; }

    d = permille - s_bar_last[idx];
    if (d < 0) { d = -d; }

    /* 变化不到 8‰（约 1.4 个像素）就不重画：
     * 进度条每帧重画会有细微抖动，而且绝大多数帧的变化本来就看不出。 */
    if (s_bar_last[idx] != UI_BAR_NONE && d < 8 && s_bar_last_col[idx] == color)
    {
        return;
    }

    s_bar_last[idx]     = permille;
    s_bar_last_col[idx] = color;

    uip_bar(lx(UI_METRIC_BAR_X0), ly((uint16_t)(y + 10)),
            lx(UI_METRIC_BAR_X1), ly((uint16_t)(y + 25)),
            permille, color, UIP_TRACK);
}

/** 主界面切回来时把所有缓存判为"脏"，保证整屏刷新一次 */
static void invalidate_main_cache(void)
{
    s_cell_have = 0u;
    s_static_ok = 0u;
    s_bar_last[0] = UI_BAR_NONE;
    s_bar_last[1] = UI_BAR_NONE;
    s_bar_last[2] = UI_BAR_NONE;
}

/*==============================================================================
 *                              公共部件
 *============================================================================*/

/**
  * @brief  画标题栏
  * @param  title    标题文字
  * @note   属于静态层，只在进入界面时画一次。
  *         右上角不再有按钮 —— 本机界面是只读的，没有触摸交互。
  */
static void draw_title(const char *title)
{
    uint16_t w = uip_width();

    /* 标题栏底 + 一条分隔线。右上角不再有按钮 —— 本机界面是只读的。 */
    uip_fill(0, 0, (uint16_t)(w - 1), UI_TITLE_H, UIP_PANEL);
    uip_hline(0, (uint16_t)(w - 1), UI_TITLE_H, UIP_BORDER);

    uip_text(lx(8), ly(9), title, UIP_CYAN);
}

/*==============================================================================
 *                              主界面
 *============================================================================*/

/**
  * @brief  画主界面的静态层（底色 / 标题栏 / 面板 / 所有固定标签与单位）
  * @note   只在进入主界面时画一次，之后每帧只更新动态层。
  *         这里是"屏幕每 0.5 秒闪一下"的根治点。
  */
static void draw_main_static(void)
{
    uint16_t y;
    uint16_t cy;

    uip_clear(UIP_BG);

    draw_title("国标充电监控终端");

    /* ---------------- 三个读数区：标签 / 单位 / 进度条外框 ---------------- */

    /* --- 总电压 --- */
    uip_text(lx(UI_METRIC_LABEL_X), ly((uint16_t)(UI_METRIC_Y0 + 8)),
             "总电压", UIP_DIM);
    uip_text(lx(UI_METRIC_UNIT_X), ly((uint16_t)(UI_METRIC_Y0 + 16)),
             "V", UIP_DIM);
    uip_rect(lx(UI_METRIC_BAR_X0), ly((uint16_t)(UI_METRIC_Y0 + 10)),
             lx(UI_METRIC_BAR_X1), ly((uint16_t)(UI_METRIC_Y0 + 25)), UIP_BORDER);

    /* --- 总电流 --- */
    y = (uint16_t)(UI_METRIC_Y0 + UI_METRIC_H);
    uip_text(lx(UI_METRIC_LABEL_X), ly((uint16_t)(y + 8)), "总电流", UIP_DIM);
    uip_text(lx(UI_METRIC_UNIT_X), ly((uint16_t)(y + 16)), "A", UIP_DIM);
    uip_rect(lx(UI_METRIC_BAR_X0), ly((uint16_t)(y + 10)),
             lx(UI_METRIC_BAR_X1), ly((uint16_t)(y + 25)), UIP_BORDER);

    /* --- 荷电状态 --- */
    y = (uint16_t)(y + UI_METRIC_H);
    uip_text(lx(UI_METRIC_LABEL_X), ly((uint16_t)(y + 8)), "荷电状态", UIP_DIM);
    uip_text(lx(UI_METRIC_UNIT_X), ly((uint16_t)(y + 16)), "%", UIP_DIM);
    uip_rect(lx(UI_METRIC_BAR_X0), ly((uint16_t)(y + 10)),
             lx(UI_METRIC_BAR_X1), ly((uint16_t)(y + 25)), UIP_BORDER);

    /*------------------------- 信息面板 ------------------------*/
    uip_fill(lx(4), ly(UI_INFO_Y0), lx(UI_DSG_W - 4), ly(UI_INFO_Y1), UIP_PANEL);
    uip_rect(lx(4), ly(UI_INFO_Y0), lx(UI_DSG_W - 4), ly(UI_INFO_Y1), UIP_BORDER);

    /* 第 1 行：最高单体电压编号与温度
     * 偏移量都是按「16 点阵」量出来的：
     *   汉字 16 像素宽、ASCII 8 像素宽，所以「最高单体 #」= 4*16+8+8 = 80 像素，
     *   跟在 10 后面就是 90，编号从 94 开始画。下面所有 x 偏移同理。 */
    y = UI_INFO_ROW0_Y;
    uip_text(lx(10), ly(y), "最高单体 #", UIP_DIM);
    uip_text(lx(UI_INFO_COL2_X), ly(y), "温度 ", UIP_DIM);
    uip_text(lx((uint16_t)(UI_INFO_COL2_X + 56)), ly(y), " / ", UIP_DIM);
    uip_text(lx((uint16_t)(UI_INFO_COL2_X + 100)), ly(y), "C", UIP_DIM);

    /* 第 2 行：单体电压 + 允许上限 */
    y = (uint16_t)(y + UI_INFO_LINE_H);
    uip_text(lx(10), ly(y), "单体电压 ", UIP_DIM);
    uip_text(lx(130), ly(y), "V", UIP_DIM);
    uip_text(lx(UI_INFO_COL2_X), ly(y), "允许 ", UIP_DIM);
    uip_text(lx((uint16_t)(UI_INFO_COL2_X + 88)), ly(y), "V ", UIP_DIM);
    uip_text(lx((uint16_t)(UI_INFO_COL2_X + 148)), ly(y), "A", UIP_DIM);

    /* 第 3 行：累计电量与充电时长 */
    y = (uint16_t)(y + UI_INFO_LINE_H);
    uip_text(lx(10), ly(y), "累计电量 ", UIP_DIM);
    uip_text(lx(130), ly(y), "kWh", UIP_DIM);
    uip_text(lx(UI_INFO_COL2_X), ly(y), "时长 ", UIP_DIM);

    /* 第 4 行：会话号、充电模式、充电机状态 */
    y = (uint16_t)(y + UI_INFO_LINE_H);
    uip_text(lx(10), ly(y), "会话 #", UIP_DIM);
    uip_text(lx(150), ly(y), "模式 ", UIP_DIM);
    uip_text(lx(UI_INFO_COL2_X), ly(y), "充电机 ", UIP_DIM);

    /*------------------------- CAN 面板 ------------------------*/
    uip_fill(lx(4), ly(UI_CAN_Y0), lx(UI_DSG_W - 4), ly(UI_CAN_Y1), UIP_PANEL);
    uip_rect(lx(4), ly(UI_CAN_Y0), lx(UI_DSG_W - 4), ly(UI_CAN_Y1), UIP_BORDER);

    cy = UI_CAN_ROW0_Y;
    uip_text(lx(10), ly(cy), "CAN 250 kbps", UIP_CYAN);

    cy = (uint16_t)(cy + UI_INFO_LINE_H);
    uip_text(lx(10), ly(cy), "接收 ", UIP_DIM);
    uip_text(lx(96), ly(cy), "帧", UIP_DIM);
    uip_text(lx(140), ly(cy), "发送 ", UIP_DIM);
    uip_text(lx(232), ly(cy), "帧", UIP_DIM);

    cy = (uint16_t)(cy + UI_INFO_LINE_H);
    uip_text(lx(10), ly(cy), "状态 ", UIP_DIM);
    uip_text(lx(UI_INFO_COL2_X), ly(cy), "异常 ", UIP_DIM);

    /* 底部提示：本机界面只读，数据曲线在 i.MX 那边看 */
    cy = (uint16_t)(cy + UI_INFO_LINE_H);
    uip_text_center(lx((uint16_t)(UI_DSG_W / 2u)), ly(cy),
                    "只读显示，曲线见 i.MX 端", UIP_DIM);

    s_static_ok = 1u;
}

/**
  * @brief  画主界面的动态层（只更新会变的数值）
  * @note   每一个数值都带一个"上次内容"备忘，没变就完全不碰屏幕。
  */
static void draw_main_dynamic(const BMS_UiSnapshot_t *s)
{
    char     buf[24];
    char     buf2[24];
    uint16_t y;
    int32_t  permille;
    uint16_t vcolor;

    /*------------------------- 三个主要读数 -------------------
     * 进度条量程取协议层给出的「最高允许」值：
     *   电压 0 ~ 584.0 V，电流 0 ~ 100.0 A，SOC 0 ~ 100 %
     * 这样进度条本身就是「离上限还有多远」的直观提示。
     *---------------------------------------------------------*/

    /* --- 总电压 --- */
    fmt_x10(buf, s->voltage_x10);
    permille = (s->limit_v_x10 > 0)
             ? ((int32_t)s->voltage_x10 * 1000) / (int32_t)s->limit_v_x10 : 0;
    vcolor = (permille >= 1000) ? UIP_RED
           : (permille >= 950)  ? UIP_YELLOW : UIP_BLUE;
    draw_cell(UICELL_VOLT, UI_METRIC_VALUE_X, UI_METRIC_Y0,
              UI_METRIC_VALUE_W, UI_METRIC_H, buf, vcolor, UIP_BG, 2u);
    draw_bar(0u, UI_METRIC_Y0, permille, vcolor);

    /* --- 总电流 --- */
    y = (uint16_t)(UI_METRIC_Y0 + UI_METRIC_H);
    fmt_x10(buf, s->current_x10);
    permille = (s->limit_i_x10 > 0)
             ? ((int32_t)s->current_x10 * 1000) / (int32_t)s->limit_i_x10 : 0;
    vcolor = (permille >= 1000) ? UIP_RED
           : (permille >= 950)  ? UIP_YELLOW : UIP_CYAN;
    draw_cell(UICELL_CURR, UI_METRIC_VALUE_X, y,
              UI_METRIC_VALUE_W, UI_METRIC_H, buf, vcolor, UIP_BG, 2u);
    draw_bar(1u, y, permille, vcolor);

    /* --- 荷电状态 --- */
    y = (uint16_t)(y + UI_METRIC_H);
    fmt_x10(buf, s->soc_x10);
    permille = (int32_t)s->soc_x10;         /* SOC 本身就是千分比 */
    vcolor = (s->soc_x10 >= 900) ? UIP_YELLOW : UIP_GREEN;
    draw_cell(UICELL_SOC, UI_METRIC_VALUE_X, y,
              UI_METRIC_VALUE_W, UI_METRIC_H, buf, vcolor, UIP_BG, 2u);
    draw_bar(2u, y, permille, vcolor);

    /*------------------------- 信息面板 ------------------------*/
    y = UI_INFO_ROW0_Y;

    fmt_u2(buf, s->cell_max_no);
    draw_cell(UICELL_CELL_NO, 94, y, 32, 16, buf, UIP_TEXT, UIP_PANEL, 1u);

    fmt_u2(buf, s->temp_max_c);
    draw_cell(UICELL_TMAX, (uint16_t)(UI_INFO_COL2_X + 40), y, 16, 16,
              buf, UIP_ORANGE, UIP_PANEL, 1u);

    fmt_u2(buf, s->temp_min_c);
    draw_cell(UICELL_TMIN, (uint16_t)(UI_INFO_COL2_X + 80), y, 16, 16,
              buf, UIP_CYAN, UIP_PANEL, 1u);

    /* 第 2 行：单体电压 + 允许上限 */
    y = (uint16_t)(y + UI_INFO_LINE_H);

    /* mV -> V，保留 3 位小数：3310 mV -> "3.310" */
    buf[0] = (char)('0' + (s->cell_max_mv / 1000u) % 10u);
    buf[1] = '.';
    buf[2] = (char)('0' + (s->cell_max_mv / 100u) % 10u);
    buf[3] = (char)('0' + (s->cell_max_mv / 10u) % 10u);
    buf[4] = (char)('0' + (s->cell_max_mv) % 10u);
    buf[5] = '\0';
    draw_cell(UICELL_CELL_MV, 90, y, 40, 16, buf, UIP_GREEN, UIP_PANEL, 1u);

    fmt_x10(buf, s->limit_v_x10);
    draw_cell(UICELL_LIMIT_V, (uint16_t)(UI_INFO_COL2_X + 48), y, 40, 16,
              buf, UIP_TEXT, UIP_PANEL, 1u);

    fmt_x10(buf, s->limit_i_x10);
    draw_cell(UICELL_LIMIT_I, (uint16_t)(UI_INFO_COL2_X + 108), y, 40, 16,
              buf, UIP_TEXT, UIP_PANEL, 1u);

    /* 第 3 行：累计电量与充电时长 */
    y = (uint16_t)(y + UI_INFO_LINE_H);

    fmt_x10(buf, (uint16_t)s->energy_x10);
    draw_cell(UICELL_ENERGY, 90, y, 40, 16, buf, UIP_YELLOW, UIP_PANEL, 1u);

    fmt_hms(buf2, s->charge_seconds);
    draw_cell(UICELL_TIME, (uint16_t)(UI_INFO_COL2_X + 48), y, 64, 16,
              buf2, UIP_TEXT, UIP_PANEL, 1u);

    /* 第 4 行：会话号、充电模式、充电机状态 */
    y = (uint16_t)(y + UI_INFO_LINE_H);

    {
        /* 会话号最多 4 位十进制，自己转换避免 sprintf */
        uint32_t v = s->session_id;
        uint8_t  n = 0;
        uint8_t  i;

        if (v == 0u) { buf[n++] = '0'; }
        while (v > 0u && n < 10u) { buf[n++] = (char)('0' + (v % 10u)); v /= 10u; }
        for (i = 0; i < (uint8_t)(n / 2u); i++)
        {
            char t = buf[i];
            buf[i] = buf[n - 1u - i];
            buf[n - 1u - i] = t;
        }
        buf[n] = '\0';
    }
    draw_cell(UICELL_SESSION, 58, y, 32, 16, buf, UIP_TEXT, UIP_PANEL, 1u);

    draw_cell(UICELL_MODE, 198, y, 32, 16,
              (s->charge_mode == 1u) ? "恒压" : "恒流",
              UIP_TEXT, UIP_PANEL, 1u);

    draw_cell(UICELL_CRO, (uint16_t)(UI_INFO_COL2_X + 64), y, 48, 16,
              (s->cro_ready != 0u) ? "就绪" : "未就绪",
              (s->cro_ready != 0u) ? UIP_GREEN : UIP_DIM, UIP_PANEL, 1u);

    /*------------------------- CAN 面板 ------------------------*/
    y = (uint16_t)(UI_CAN_ROW0_Y + UI_INFO_LINE_H);

    /* 帧数最多显示 5 位十进制，超了就停在 99999：
     * 右边那个"帧"字的位置是固定的，位数再多就会压到它上面。 */
    {
        uint32_t v = s->rx_count;
        uint8_t  n = 0;
        uint8_t  i;

        if (v > 99999u) { v = 99999u; }
        if (v == 0u) { buf[n++] = '0'; }
        while (v > 0u && n < 10u) { buf[n++] = (char)('0' + (v % 10u)); v /= 10u; }
        for (i = 0; i < (uint8_t)(n / 2u); i++)
        {
            char t = buf[i];
            buf[i] = buf[n - 1u - i];
            buf[n - 1u - i] = t;
        }
        buf[n] = '\0';
    }
    draw_cell(UICELL_RX, 50, y, 45, 16, buf, UIP_TEXT, UIP_PANEL, 0u);

    {
        uint32_t v = s->tx_count;
        uint8_t  n = 0;
        uint8_t  i;

        if (v > 99999u) { v = 99999u; }
        if (v == 0u) { buf[n++] = '0'; }
        while (v > 0u && n < 10u) { buf[n++] = (char)('0' + (v % 10u)); v /= 10u; }
        for (i = 0; i < (uint8_t)(n / 2u); i++)
        {
            char t = buf[i];
            buf[i] = buf[n - 1u - i];
            buf[n - 1u - i] = t;
        }
        buf[n] = '\0';
    }
    draw_cell(UICELL_TX, 180, y, 51, 16, buf, UIP_TEXT, UIP_PANEL, 0u);

    y = (uint16_t)(y + UI_INFO_LINE_H);
    /* 状态格显示"充电阶段"：充满 / 放电中 这些是用户最关心的 */
    draw_cell(UICELL_STATE, 50, y, 196, 16, phase_name(s->phase),
              phase_color(s->phase), UIP_PANEL, 0u);

    draw_cell(UICELL_ERROR, (uint16_t)(UI_INFO_COL2_X + 48), y, 176, 16,
              BMS_ErrorStr(s->error),
              (s->error == BMS_ERR_NONE) ? UIP_DIM : UIP_RED, UIP_PANEL, 0u);
}


static void redraw(const BMS_UiSnapshot_t *s)
{
    /* 静态层只在需要时画一次；之后每一帧只更新会变的数值。
     * 这就是"屏幕每 0.5 秒闪一下"的根治点。 */
    if (s_static_ok == 0u)
    {
        draw_main_static();
    }
    draw_main_dynamic(s);
}

/*==============================================================================
 *                              对外接口
 *============================================================================*/

void UI_Init(void)
{
    uip_init();

    /* 屏幕比设计尺寸大时整体居中，保证不出现「内容缩在左上角」 */
    if (uip_width() > UI_DSG_W)
    {
        s_off_x = (uint16_t)((uip_width() - UI_DSG_W) / 2u);
    }
    if (uip_height() > UI_DSG_H)
    {
        s_off_y = (uint16_t)((uip_height() - UI_DSG_H) / 2u);
    }

    s_ui_ms        = 0;
    s_last_draw_ms = 0;
    s_inited       = 1;

    /* 清掉局部重绘的缓存，保证上电第一帧是完整的一屏 */
    invalidate_main_cache();

    printf("[UI ] 界面版本: 2025-UI-READONLY (只读显示，无触摸、无曲线)\r\n");
    printf("[UI ] UI ready: %ux%u\r\n",
           (unsigned int)uip_width(), (unsigned int)uip_height());
    /* 画首帧：让用户上电就看到内容，而不是黑屏等一秒 */
    {
        BMS_UiSnapshot_t snap;
        BMS_Protocol_GetUiSnapshot(&snap);
        redraw(&snap);
    }
}

void UI_Tick(void)
{
    BMS_UiSnapshot_t s;
    uint32_t         now;

    if (!s_inited)
    {
        return;
    }

    now = s_ui_ms;          /* 由 UI_Tick_1ms() 在 SysTick 里累加的真实毫秒时钟 */

    /* ---------------- 重绘：固定 500 ms ----------------
     * 本机界面是只读的：没有触摸要扫描，也没有曲线要采样。
     * 每 500 ms 取一次快照，只有真的变了才会碰屏幕（见 draw_main_dynamic
     * 里的逐格"上次内容"比对），所以静态画面下 LCD 基本不写。 */
    if (s_last_draw_ms == 0u || (now - s_last_draw_ms) >= 500u)
    {
        s_last_draw_ms = now;
        BMS_Protocol_GetUiSnapshot(&s);
        redraw(&s);
    }
}
/**
  * @brief  界面毫秒时基（由 SysTick 中断每秒调用 1000 次）
  * @note   放在中断里只做一次加法，开销可忽略；
  *         真正的绘图全部在 UI_Tick() 里完成，绝不在中断中碰 LCD。
  */
void UI_Tick_1ms(void)
{
    s_ui_ms++;
}

/* 下面两个接口保留空实现：外部（协议层/上位机脚本）还有调用点，
 * 但本机界面已经没有曲线界面、也没有历史缓冲了。 */
uint8_t UI_IsCurveScreen(void)
{
    return 0u;
}

void UI_ClearHistory(void)
{
    /* 无事可做：STM32 端不再保存曲线 */
}

#else   /* ---------------- UI_ENABLE == 0：不要界面 ---------------- */

/*
 * 关闭界面时的空实现。
 *
 * 这样 main.c 里的 UI_Init() / UI_Tick() 与 stm32f10x_it.c 里的
 * UI_Tick_1ms() 都能照常调用，一行都不用改，工程也不再依赖任何
 * LCD / 触摸驱动 —— 屏幕还没调通时先把 CAN 跑起来。
 */

void UI_Init(void)
{
    /* 什么都不做 */
}

void UI_Tick(void)
{
    /* 什么都不做 */
}

void UI_Tick_1ms(void)
{
    /* 什么都不做 */
}

uint8_t UI_IsCurveScreen(void)
{
    return 0;
}

void UI_ClearHistory(void)
{
    /* 什么都不做 */
}

#endif  /* UI_ENABLE */
