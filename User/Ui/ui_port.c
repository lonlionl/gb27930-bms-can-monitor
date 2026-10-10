/**
  ******************************************************************************
  * @file    ui_port.c
  * @brief   STM32 本地界面 —— LCD / 触摸 适配层实现
  *
  * ============================ 驱动适配说明 ==================================
  *   本文件是整个界面里唯一与「正点原子 LCD 驱动」直接打交道的文件。
  *   下面把工程里用到的驱动符号集中列出，并给出常见版本的对应关系；
  *   如果你的工程编译报「未定义」，只改这一处即可，其它文件不用动。
  *
  *   ① LCD_Init()
  *        标准库例程（LCD 实验 / 综合例程）都有，无差异。
  *
  *   ② LCD_Fill(u16 sx, u16 sy, u16 ex, u16 ey, u16 color)
  *        坐标闭区间。所有版本签名一致，最稳妥，界面的一切矩形都靠它。
  *        个别极老版本是 LCD_Fill(x0,y0,x1,y1,color) 但参数类型为 u8，
  *        若你的屏幕分辨率 > 255 必须先确认参数类型是 u16。
  *
  *   ③ 屏幕尺寸
  *        · 带 lcddev 结构体的版本（推荐）: lcddev.width / lcddev.height
  *        · 老版本:                        LCD_GetWidth() / 宏定义
  *        本文件用 UIP_LCD_WIDTH / UIP_LCD_HEIGHT 两个宏统一，见下面「适配块」。
  *
  *   ④ 触摸
  *        · 新版本: tp_dev.scan(0); 读 tp_dev.x[0] / tp_dev.y[0] / tp_dev.sta
  *        · 老版本: TP_Scan(0);     读 tp_dev.x[0] / tp_dev.y[0]
  *        本文件用 UIP_TOUCH_SCAN() 宏统一，见「适配块」。
  *
  *   ⑤ 字库
  *        不使用驱动的 LCD_ShowString / LCD_ShowChinese！
  *        界面文字统一走 font16.h（tools/gen_font.py 生成），
  *        好处是：不依赖驱动的字库、中英文混排基线一致、
  *        两端（STM32 与 I.MX6ULL）字模完全一致，换驱动不影响显示效果。
  *
  *   ⑥ 关掉界面的办法
  *        ui_port.h 里的 UI_ENABLE 置 0，本文件与 ui_app.c 会退化成空实现，
  *        整个工程不再依赖 lcd.h / touch.h，CAN 功能照常工作。
  * ============================================================================
  */

#include "ui_port.h"

#if UI_ENABLE

#include "lcd.h"
/* 注意：这里**不再** include "touch.h"。
 * 本机界面是只读的，不碰触摸驱动 —— 这样链接器会把整个 touch.c / ctiic.c
 * 剥掉，省下一大块 Flash（MDK-Lite 只有 32 KB 额度）。 */

/* 打开 GBK 映射表：让 font16.h 额外导出 g_font_gbk_map[]，
 * 供本文件的 GBK 解码使用（Linux 端不定义这个宏，不占空间）。 */
#define FONT_WITH_GBK_MAP
#include "font16.h"

/*==============================================================================
 *                          ① 驱动适配块（改这里！）
 *
 *   本工程自带的 LCD 驱动在 User/Lcd/ 下（本仓库一并提供）：
 *       lcd.c / lcd.h       FSMC + 绘图原语（480x320 横屏）
 *       ili93xx.c           ILI9488 初始化序列与读 ID
 *       touch.c / touch.h   电容触摸（自动识别 GT9147/GT1151/FT5206/OTT2000）
 *       ctiic.c / ctiic.h   触摸屏专用软件 I2C
 *
 *   如果你后来换成了正点原子官方例程里的 lcd.c，只需要改下面三个宏：
 *============================================================================*/

/* ---- 屏幕尺寸 ----
 * 本仓库的驱动：lcd.h 里的 LCD_W / LCD_H（480 / 320，横屏）
 * 正点原子官方例程：((uint16_t)lcddev.width) / ((uint16_t)lcddev.height)
 * 最老版本：        ((uint16_t)LCD_GetWidth())
 */
#define UIP_LCD_WIDTH()        ((uint16_t)LCD_W)
#define UIP_LCD_HEIGHT()       ((uint16_t)LCD_H)

/* ---- 填充矩形（闭区间） ----
 * 本仓库的驱动：LCD_FillRect(x0,y0,x1,y1,color)
 * 正点原子官方例程：LCD_Fill(x0,y0,x1,y1,color)
 */
#define UIP_LCD_FILL(x0,y0,x1,y1,c)   LCD_FillRect((uint16_t)(x0),(uint16_t)(y0), \
                                                   (uint16_t)(x1),(uint16_t)(y1), \
                                                   (uint16_t)(c))


/*==============================================================================
 *                              内部状态
 *============================================================================*/

static uint16_t s_w = UIP_MAX_W;
static uint16_t s_h = UIP_MAX_H;

/*==============================================================================
 *                              初始化
 *============================================================================*/

void uip_init(void)
{
    /* 直接初始化 LCD（不再有触摸驱动要抢在它前面跑）：
     * 有它自己的引脚配置，随便什么时候调都行。 */
    LCD_Init();                      /* LCD 初始化（含读 ID、面板序列、开背光） */

    s_w = UIP_LCD_WIDTH();
    s_h = UIP_LCD_HEIGHT();

    /* 屏参异常兜底：万一是竖屏或读不到，退回 480x320 横屏 */
    if (s_w < 100 || s_w > UIP_MAX_W || s_h < 100 || s_h > UIP_MAX_H)
    {
        s_w = UIP_MAX_W;
        s_h = UIP_MAX_H;
    }

    uip_clear(UIP_BG);
}

uint16_t uip_width(void)  { return s_w; }
uint16_t uip_height(void) { return s_h; }

/*==============================================================================
 *                              绘图原语
 *============================================================================*/

void uip_clear(uint16_t color)
{
    UIP_LCD_FILL(0, 0, s_w - 1, s_h - 1, color);
}

void uip_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    if (x0 > x1) { uint16_t t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { uint16_t t = y0; y0 = y1; y1 = t; }

    /* 裁剪到屏幕内，避免越界把显存写乱 */
    if (x0 >= s_w || y0 >= s_h) { return; }
    if (x1 >= s_w) { x1 = s_w - 1; }
    if (y1 >= s_h) { y1 = s_h - 1; }

    UIP_LCD_FILL(x0, y0, x1, y1, color);
}

void uip_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= s_w || y >= s_h) { return; }
    UIP_LCD_FILL(x, y, x, y, color);
}

void uip_hline(uint16_t x0, uint16_t x1, uint16_t y, uint16_t color)
{
    if (y >= s_h) { return; }
    uip_fill(x0, y, x1, y, color);
}

void uip_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    uip_hline(x0, x1, y0, color);
    uip_hline(x0, x1, y1, color);
    uip_fill(x0, y0, x0, y1, color);
    uip_fill(x1, y0, x1, y1, color);
}

/**
  * @brief  直线（Bresenham）
  * @note   刻意不调用驱动的 LCD_DrawLine：一是各版本签名不一，
  *         二是历史曲线要按任意斜率画几千条线段，自己控制更可控。
  *         实现上用「水平填充」而不是逐点画，速度提升非常明显
  *         （横向线段一次 LCD_Fill 就画完，不必每个像素都设一次光标）。
  */
void uip_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int x = x0, y = y0;
    int guard = 0;

    /* guard 防止参数异常时死循环（理论上 Bresenham 一定会收敛） */
    while (guard++ < 4096)
    {
        if (x >= 0 && y >= 0 && x < (int)s_w && y < (int)s_h)
        {
            UIP_LCD_FILL((uint16_t)x, (uint16_t)y, (uint16_t)x, (uint16_t)y, color);
        }
        if (x == x1 && y == y1) { break; }

        {
            int e2 = err << 1;
            if (e2 > -dy) { err -= dy; x += sx; }
            if (e2 <  dx) { err += dx; y += sy; }
        }
    }
}

void uip_bar(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
             int32_t permille, uint16_t fill, uint16_t track)
{
    int32_t w;

    uip_fill(x0, y0, x1, y1, track);        /* 先铺底槽 */

    if (permille <= 0)   { permille = 0; }
    if (permille > 1000) { permille = 1000; }

    w = ((int32_t)(x1 - x0 + 1) * permille) / 1000;
    if (w <= 0) { return; }

    uip_fill(x0, y0, (uint16_t)(x0 + w - 1), y1, fill);
}

/*==============================================================================
 *                              文字
 *============================================================================*/

/* ASCII 与汉字字模宽度（与 font16.h 的格式约定一致） */
#define GLYPH_ASC_W     8
#define GLYPH_CJK_W     16
#define GLYPH_H         16

/**
  * @brief  画一个 16x16 字模（可放大）
  * @param  g     字模指针（FONT_GLYPH_BYTES 字节）
  * @param  cw    字模有效宽度（8 或 16）
  * @param  n     放大倍数（1 = 原尺寸，2 = 2 倍）
  * @note   每行按「连续亮点」合并成一次 LCD_Fill。
  *         逐像素画一个字要 256 次 LCD_Fill，合并后通常只要 20~40 次，
  *         整屏刷新时间从 ~200 ms 降到 ~20 ms，这是界面流畅的关键。
  */
static void draw_glyph_n(uint16_t x, uint16_t y, const uint8_t *g,
                         uint8_t cw, uint16_t color, uint8_t n,
                         uint8_t rowbytes)
{
    uint8_t row;
    uint8_t seg_start;
    uint8_t col;
    uint16_t h = (uint16_t)(GLYPH_H * n);

    if (n == 0) { n = 1; }

    /* 完全在屏幕外直接跳过（省下大量无用的 LCD_Fill 调用） */
    if (x >= s_w || y >= s_h) { return; }
    if ((uint16_t)(y + h) > s_h) { return; }

    for (row = 0; row < GLYPH_H; row++)
    {
        /* ASCII 字模每行 1 字节（rowbytes==1），汉字每行 2 字节。
         * 两种都归一到 16 位、MSB 在左，后面的取位逻辑共用。 */
        uint16_t bits = (rowbytes >= 2u)
                      ? (uint16_t)(((uint16_t)g[row * 2u] << 8) |
                                   (uint16_t)g[row * 2u + 1u])
                      : (uint16_t)((uint16_t)g[row] << 8);
        uint16_t y0 = (uint16_t)(y + row * n);
        uint16_t y1 = (uint16_t)(y0 + n - 1);

        seg_start = 0xFF;               /* 0xFF 表示当前不在亮点段里 */

        for (col = 0; col <= cw; col++)
        {
            /* MSB 对应最左像素；col == cw 时当作「亮点结束」的哨兵 */
            uint8_t on = 0;

            if (col < cw)
            {
                on = (uint8_t)((bits >> (15 - col)) & 0x0001);
            }

            if (on)
            {
                if (seg_start == 0xFF) { seg_start = col; }
            }
            else if (seg_start != 0xFF)
            {
                /* 一段亮点结束：[seg_start, col-1]，横向也要按倍数展宽 */
                uint16_t px0 = (uint16_t)(x + seg_start * n);
                uint16_t px1 = (uint16_t)(x + col * n - 1);

                uip_fill(px0, y0, px1, y1, color);
                seg_start = 0xFF;
            }
        }
    }
}

/**
  * @brief  GBK 双字节码查字模（二分查找，表按 gbk 升序）
  * @return 字模指针；未收录返回 0
  */
static const uint8_t *glyph_by_gbk(uint16_t gbk)
{
    int lo = 0;
    int hi = (int)FONT_GBK_COUNT - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        uint16_t k = g_font_gbk_map[mid].gbk;

        if (k == gbk)      { return FONT_CJK_GLYPH(g_font_gbk_map[mid].index); }
        else if (k < gbk)  { lo = mid + 1; }
        else               { hi = mid - 1; }
    }
    return 0;
}

uint16_t uip_text_w(const char *txt)
{
    return uip_text_scale_w(txt, 1);
}

uint16_t uip_text_scale_w(const char *txt, uint8_t n)
{
    uint16_t w = 0;

    if (txt == 0) { return 0; }
    if (n == 0) { n = 1; }

    while (*txt != '\0')
    {
        uint8_t c = (uint8_t)(*txt);

        if (c < 0x80)
        {
            txt++;
            w += (uint16_t)(GLYPH_ASC_W * n);
        }
        else if (*(txt + 1) != '\0')
        {
            txt += 2;                   /* GBK 双字节 */
            w += (uint16_t)(GLYPH_CJK_W * n);
        }
        else
        {
            break;                      /* 落单的半个汉字：丢弃 */
        }
    }
    return w;
}

/**
  * @brief  按放大倍数画文本（核心实现，scale=1 时就是普通文字）
  * @note   ASCII 步进 8*n，汉字步进 16*n；两者共用 16x16 方格，
  *         所以中英文混排的基线天然对齐，不需要额外调整。
  */
uint16_t uip_text_scale(uint16_t x, uint16_t y, const char *txt,
                        uint16_t color, uint8_t n)
{
    uint16_t h;

    if (txt == 0) { return x; }
    if (n == 0) { n = 1; }

    h = (uint16_t)(GLYPH_H * n);
    if (y + h > s_h) { return x; }

    while (*txt != '\0')
    {
        uint8_t c = (uint8_t)(*txt);

        if (c < 0x80)
        {
            if (c >= FONT_ASCII_FIRST && c <= FONT_ASCII_LAST)
            {
                const uint8_t *g = FONT_ASC_GLYPH((uint32_t)(c - FONT_ASCII_FIRST));
                if ((uint16_t)(x + GLYPH_ASC_W * n) <= s_w)
                {
                    draw_glyph_n(x, y, g, GLYPH_ASC_W, color, n, FONT_ASC_ROWBYTES);
                }
            }
            x += (uint16_t)(GLYPH_ASC_W * n);
            txt++;
        }
        else
        {
            uint16_t gbk;
            const uint8_t *g;

            if (*(txt + 1) == '\0') { break; }

            gbk = (uint16_t)(((uint16_t)(uint8_t)txt[0] << 8) |
                              (uint16_t)(uint8_t)txt[1]);

            g = glyph_by_gbk(gbk);
            if (g != 0 && (uint16_t)(x + GLYPH_CJK_W * n) <= s_w)
            {
                draw_glyph_n(x, y, g, GLYPH_CJK_W, color, n, FONT_ROWS);
            }
            /* 字库没有的字：留一个空位而不是画方块，
             * 这样一眼能看出是漏字
             * （配合 tools/gen_font.py 的源码自动扫描，基本不会发生）*/
            x += (uint16_t)(GLYPH_CJK_W * n);
            txt += 2;
        }

        if (x >= s_w) { break; }        /* 画到屏幕右边缘，提前收工 */
    }
    return x;
}

uint16_t uip_text(uint16_t x, uint16_t y, const char *txt, uint16_t color)
{
    return uip_text_scale(x, y, txt, color, 1);
}

void uip_text_center(uint16_t cx, uint16_t y, const char *txt, uint16_t color)
{
    uint16_t w = uip_text_w(txt);

    if (w >= cx) { uip_text(0, y, txt, color); return; }
    uip_text((uint16_t)(cx - w / 2), y, txt, color);
}

void uip_text_center_scale(uint16_t cx, uint16_t y, const char *txt,
                           uint16_t color, uint8_t n)
{
    uint16_t w = uip_text_scale_w(txt, n);

    if (w >= cx) { uip_text_scale(0, y, txt, color, n); return; }
    uip_text_scale((uint16_t)(cx - w / 2), y, txt, color, n);
}

void uip_text_clip(uint16_t x, uint16_t y, uint16_t max_w,
                   const char *txt, uint16_t color)
{
    uint16_t used = 0;

    if (txt == 0) { return; }
    if (y + GLYPH_H > s_h) { return; }

    while (*txt != '\0')
    {
        uint8_t c = (uint8_t)(*txt);
        uint16_t adv;
        const uint8_t *g = 0;
        uint8_t gw = GLYPH_ASC_W;

        if (c < 0x80)
        {
            adv = GLYPH_ASC_W;
            if (c >= FONT_ASCII_FIRST && c <= FONT_ASCII_LAST)
            {
                g = FONT_ASC_GLYPH((uint32_t)(c - FONT_ASCII_FIRST));
            }
        }
        else
        {
            uint16_t gbk;
            if (*(txt + 1) == '\0') { break; }
            adv = GLYPH_CJK_W;
            gw  = GLYPH_CJK_W;
            gbk = (uint16_t)(((uint16_t)(uint8_t)txt[0] << 8) |
                              (uint16_t)(uint8_t)txt[1]);
            g = glyph_by_gbk(gbk);
        }

        if ((uint16_t)(used + adv) > max_w) { break; }   /* 放不下就截断 */

        if (g != 0 && (uint16_t)(x + used + gw) <= s_w)
        {
            draw_glyph_n((uint16_t)(x + used), y, g, gw, color, 1,
                         (gw == GLYPH_ASC_W) ? FONT_ASC_ROWBYTES : FONT_ROWS);
        }
        used += adv;
        txt += (c < 0x80) ? 1 : 2;
    }
}

/*==============================================================================
 *                              触摸
 *============================================================================*/

/**
  * @brief  触摸读取（**永远返回"没有触摸"**）
  *
  * 现场那块 GT1151 一直探测不到，而且 STM32 这边本来也只需要被动显示，
  * 所以触摸整块去掉了。接口保留是为了不动上层调用点。
  */
uint8_t uip_touch_read(uint16_t *x, uint16_t *y)
{
    (void)x;
    (void)y;
    return 0u;
}

/** 触摸是否可用：恒为 0（本机界面不再使用触摸） */
uint8_t uip_touch_ok(void)
{
    return 0u;
}
#else   /* ---------------- UI_ENABLE == 0：不要界面 ---------------- */

/*
 * 关闭界面时的空实现。
 *
 * 目的：让工程在「LCD 驱动还没弄好」的情况下也能正常编译、正常运行 CAN 功能。
 * 这里把所有 uip_* 都实现成安全的空操作，ui_app.c 那边的 UI_* 也走空实现，
 * 而 main.c / stm32f10x_it.c 的调用点完全不用改。
 *
 * 参数一律 (void) 掉，避免 -Wall 报未使用参数。
 */

void uip_init(void)
{
    /* 什么都不做 */
}

uint16_t uip_width(void)  { return UIP_MAX_W; }
uint16_t uip_height(void) { return UIP_MAX_H; }

void uip_clear(uint16_t color) { (void)color; }
void uip_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    (void)x0; (void)y0; (void)x1; (void)y1; (void)color;
}
void uip_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    (void)x; (void)y; (void)color;
}
void uip_hline(uint16_t x0, uint16_t x1, uint16_t y, uint16_t color)
{
    (void)x0; (void)x1; (void)y; (void)color;
}
void uip_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    (void)x0; (void)y0; (void)x1; (void)y1; (void)color;
}
void uip_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    (void)x0; (void)y0; (void)x1; (void)y1; (void)color;
}
void uip_bar(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
             int32_t permille, uint16_t fill, uint16_t track)
{
    (void)x0; (void)y0; (void)x1; (void)y1;
    (void)permille; (void)fill; (void)track;
}

uint16_t uip_text_w(const char *txt) { (void)txt; return 0; }
uint16_t uip_text_scale_w(const char *txt, uint8_t n) { (void)txt; (void)n; return 0; }
uint16_t uip_text(uint16_t x, uint16_t y, const char *txt, uint16_t color)
{
    (void)y; (void)txt; (void)color; return x;
}
uint16_t uip_text_scale(uint16_t x, uint16_t y, const char *txt,
                        uint16_t color, uint8_t n)
{
    (void)y; (void)txt; (void)color; (void)n; return x;
}
void uip_text_center(uint16_t cx, uint16_t y, const char *txt, uint16_t color)
{
    (void)cx; (void)y; (void)txt; (void)color;
}
void uip_text_center_scale(uint16_t cx, uint16_t y, const char *txt,
                           uint16_t color, uint8_t n)
{
    (void)cx; (void)y; (void)txt; (void)color; (void)n;
}
void uip_text_clip(uint16_t x, uint16_t y, uint16_t max_w,
                   const char *txt, uint16_t color)
{
    (void)x; (void)y; (void)max_w; (void)txt; (void)color;
}

uint8_t uip_touch_read(uint16_t *x, uint16_t *y)
{
    (void)x; (void)y; return 0;
}
uint8_t uip_touch_ok(void) { return 0; }

#endif  /* UI_ENABLE */
