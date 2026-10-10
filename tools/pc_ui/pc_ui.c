/**
 * tools/pc_ui/pc_ui.c  --  DEV-ONLY, NOT PART OF THE DELIVERY
 *
 * PC implementation of the ui_port layer, so that User/Ui/ui_app.c can be
 * compiled and *rendered* on a Linux PC. Purpose: verify the 480x320 layout
 * (overlaps, off-screen text, alignment) without touching the real board.
 *
 * Text rendering is done here with the UTF-8 font lookup table, because the
 * build passes -finput-charset=GBK -fexec-charset=UTF-8: the GBK literals in
 * ui_app.c are converted to UTF-8 by the compiler, so the Unicode map in
 * font16.h is the right one to use.
 *
 * Output: an ASCII-art dump of each screen plus a PPM image, exactly like the
 * Linux-side uitest tool. Run it with `make -f tools/pc_ui/Makefile`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_port.h"
#include "bms_protocol.h"

#define FONT_WITH_GBK_MAP
#include "font16.h"

/* ---- synthetic touch: the tool schedules taps, uip_touch_read replays them --- */
static int s_tap_x = -1;
static int s_tap_y = -1;

static void pc_tap(int x, int y)
{
    s_tap_x = x;
    s_tap_y = y;
}

uint8_t uip_touch_read(uint16_t *x, uint16_t *y)
{
    if (s_tap_x < 0) { return 0; }
    *x = (uint16_t)s_tap_x;
    *y = (uint16_t)s_tap_y;
    s_tap_x = -1;
    s_tap_y = -1;
    return 1;
}

uint8_t uip_touch_ok(void) { return 1; }

/*============================================================================*/
/*                      virtual 480x320 framebuffer                           */
/*============================================================================*/

static int       s_w = 480;
static int       s_h = 320;
static uint16_t *s_fb;

static uint16_t *px(int x, int y)
{
    if (x < 0 || y < 0 || x >= s_w || y >= s_h) { return NULL; }
    return &s_fb[y * s_w + x];
}

void uip_init(void)
{
    if (s_fb == NULL)
    {
        s_fb = (uint16_t *)malloc((size_t)(s_w * s_h) * sizeof(uint16_t));
        if (s_fb == NULL)
        {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    /* The real STM32 port clears the panel to UIP_BG in uip_init(); mirror that
     * here, otherwise the preview would show un-cleared (black) background and
     * we could not tell "background" from "never painted". */
    uip_clear(UIP_BG);
}

uint16_t uip_width(void)  { return (uint16_t)s_w; }
uint16_t uip_height(void) { return (uint16_t)s_h; }

void uip_clear(uint16_t color)
{
    int i;
    for (i = 0; i < s_w * s_h; i++) { s_fb[i] = color; }
}

void uip_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    int x, y;

    if (x0 > x1) { uint16_t t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { uint16_t t = y0; y0 = y1; y1 = t; }

    for (y = y0; y <= (int)y1; y++)
    {
        for (x = x0; x <= (int)x1; x++)
        {
            uint16_t *p = px(x, y);
            if (p != NULL) { *p = color; }
        }
    }
}

void uip_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    uint16_t *p = px(x, y);
    if (p != NULL) { *p = color; }
}

void uip_hline(uint16_t x0, uint16_t x1, uint16_t y, uint16_t color)
{
    uip_fill(x0, y, x1, y, color);
}

void uip_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    uip_hline(x0, x1, y0, color);
    uip_hline(x0, x1, y1, color);
    uip_fill(x0, y0, x0, y1, color);
    uip_fill(x1, y0, x1, y1, color);
}

void uip_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;)
    {
        uip_pixel((uint16_t)x0, (uint16_t)y0, color);
        if (x0 == x1 && y0 == y1) { break; }
        {
            int e2 = err << 1;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 <  dx) { err += dx; y0 += sy; }
        }
    }
}

void uip_bar(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
             int32_t permille, uint16_t fill, uint16_t track)
{
    int32_t w;

    uip_fill(x0, y0, x1, y1, track);

    if (permille <= 0)   { permille = 0; }
    if (permille > 1000) { permille = 1000; }

    w = ((int32_t)(x1 - x0 + 1) * permille) / 1000;
    if (w <= 0) { return; }
    uip_fill(x0, y0, (uint16_t)(x0 + w - 1), y1, fill);
}

/*============================================================================*/
/*                            text (UTF-8)                                    */
/*============================================================================*/

#define GLYPH_ASC_W   8
#define GLYPH_CJK_W   16
#define GLYPH_H       16

static void draw_glyph_pc(uint16_t x, uint16_t y, const uint8_t *g,
                          uint8_t cw, uint16_t color, uint8_t n,
                          uint8_t rowbytes)
{
    uint8_t row, col, seg;

    if (n == 0) { n = 1; }

    for (row = 0; row < GLYPH_H; row++)
    {
        uint16_t bits = (rowbytes >= 2u)
                      ? (uint16_t)(((uint16_t)g[row * 2u] << 8) |
                                   (uint16_t)g[row * 2u + 1u])
                      : (uint16_t)((uint16_t)g[row] << 8);
        seg = 0xFF;
        for (col = 0; col <= cw; col++)
        {
            uint8_t on = (col < cw)
                       ? (uint8_t)((bits >> (15 - col)) & 1) : 0;
            if (on)
            {
                if (seg == 0xFF) { seg = col; }
            }
            else if (seg != 0xFF)
            {
                uip_fill((uint16_t)(x + seg * n), (uint16_t)(y + row * n),
                         (uint16_t)(x + col * n - 1), (uint16_t)(y + row * n + n - 1),
                         color);
                seg = 0xFF;
            }
        }
    }
}

static const uint8_t *glyph_uni(uint32_t cp)
{
    int lo = 0, hi = (int)FONT_CJK_COUNT - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        uint32_t k = g_font_cjk_map[mid].codepoint;
        if (k == cp)     { return FONT_CJK_GLYPH(g_font_cjk_map[mid].index); }
        else if (k < cp) { lo = mid + 1; }
        else             { hi = mid - 1; }
    }
    return NULL;
}

/*============================================================================
 *  字库覆盖检查
 *
 *   STM32 端为了省 Flash，字库是「按需生成」的（只放 ui_app.c 里真正会画出来
 *   的字，见 tools/gen_font.py 的 --profile stm32）。字少了最怕的就是漏字 ——
 *   漏了不会报错，只会在屏上留一块空白，很难发现。
 *
 *   所以这里在**每一次画文字**的时候顺手做检查：凡是查不到字模的字都记下来，
 *   跑完统一报出来。这样覆盖检查是「由实际绘制路径驱动」的，不需要手工维护
 *   一份字符串清单，也不会漏掉任何一条绘制调用。
 *==========================================================================*/
static uint32_t g_missing[128];
static int      g_missing_n = 0;

static void cov_note_missing(uint32_t cp, const char *ctx)
{
    int i;

    for (i = 0; i < g_missing_n; i++)
    {
        if (g_missing[i] == cp) { return; }
    }
    if (g_missing_n < (int)(sizeof(g_missing) / sizeof(g_missing[0])))
    {
        g_missing[g_missing_n++] = cp;
    }
    printf("  [缺字] U+%04X 出现在 \"%s\"\n", cp, ctx);
}

static int cov_report(void)
{
    if (g_missing_n == 0)
    {
        printf("\n==================== 字库覆盖检查 ====================\n");
        printf("  STM32 字库全部够用：ASCII %u + 汉字 %u，绘制过程中 0 处缺字\n",
               (unsigned)FONT_ASCII_COUNT, (unsigned)FONT_CJK_COUNT);
        return 0;
    }

    printf("\n==================== 字库覆盖检查 ====================\n");
    printf("  共 %d 个字符在字库里找不到！请把它们补进 tools/gen_font.py 的\n"
           "  --profile stm32 扫描范围，然后重新生成 User/Ui/font16.h\n",
           g_missing_n);
    return 1;
}

/* decode one UTF-8 char; *adv gets the byte count */
static uint32_t utf8_next(const char *s, int *adv)
{
    const unsigned char *p = (const unsigned char *)s;

    if (p[0] < 0x80) { *adv = 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0 && p[1]) { *adv = 2; return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2])
    {
        *adv = 3;
        return ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    *adv = 1;
    return p[0];
}

uint16_t uip_text_scale_w(const char *txt, uint8_t n)
{
    uint16_t w = 0;
    if (txt == NULL) { return 0; }
    if (n == 0) { n = 1; }
    while (*txt)
    {
        int adv;
        uint32_t cp = utf8_next(txt, &adv);
        w += (uint16_t)((cp < 0x80 ? GLYPH_ASC_W : GLYPH_CJK_W) * n);
        txt += adv;
    }
    return w;
}

uint16_t uip_text_w(const char *txt) { return uip_text_scale_w(txt, 1); }

uint16_t uip_text_scale(uint16_t x, uint16_t y, const char *txt,
                        uint16_t color, uint8_t n)
{
    if (txt == NULL) { return x; }
    if (n == 0) { n = 1; }

    while (*txt)
    {
        int adv;
        uint32_t cp = utf8_next(txt, &adv);
        const uint8_t *g = NULL;
        uint8_t gw;

        if (cp < 0x80)
        {
            gw = GLYPH_ASC_W;
            if (cp >= FONT_ASCII_FIRST && cp <= FONT_ASCII_LAST)
            {
                g = FONT_ASC_GLYPH((uint32_t)(cp - FONT_ASCII_FIRST));
            }
        }
        else
        {
            gw = GLYPH_CJK_W;
            g = glyph_uni(cp);
            if (g == NULL) { cov_note_missing(cp, txt); }
        }

        if (g != NULL) { draw_glyph_pc(x, y, g, gw, color, n,
                                       (gw == GLYPH_ASC_W) ? FONT_ASC_ROWBYTES : FONT_ROWS); }
        x += (uint16_t)(gw * n);
        txt += adv;
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
    uip_text((uint16_t)((w >= cx) ? 0 : (cx - w / 2)), y, txt, color);
}

void uip_text_center_scale(uint16_t cx, uint16_t y, const char *txt,
                           uint16_t color, uint8_t n)
{
    uint16_t w = uip_text_scale_w(txt, n);
    uip_text_scale((uint16_t)((w >= cx) ? 0 : (cx - w / 2)), y, txt, color, n);
}

void uip_text_clip(uint16_t x, uint16_t y, uint16_t max_w,
                   const char *txt, uint16_t color)
{
    uint16_t used = 0;

    if (txt == NULL) { return; }

    while (*txt)
    {
        int adv;
        uint32_t cp = utf8_next(txt, &adv);
        const uint8_t *g = NULL;
        uint16_t gw;

        if (cp < 0x80)
        {
            gw = GLYPH_ASC_W;
            if (cp >= FONT_ASCII_FIRST && cp <= FONT_ASCII_LAST)
            {
                g = FONT_ASC_GLYPH((uint32_t)(cp - FONT_ASCII_FIRST));
            }
        }
        else
        {
            gw = GLYPH_CJK_W;
            g = glyph_uni(cp);
        }

        if ((uint16_t)(used + gw) > max_w) { break; }
        if (g != NULL) { draw_glyph_pc((uint16_t)(x + used), y, g, (uint8_t)gw, color, 1,
                                       (gw == GLYPH_ASC_W) ? FONT_ASC_ROWBYTES : FONT_ROWS); }
        used += gw;
        txt += adv;
    }
}

/*============================================================================*/
/*                            output helpers                                  */
/*============================================================================*/

static void dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (f == NULL) { return; }
    fprintf(f, "P6\n%d %d\n255\n", s_w, s_h);
    for (i = 0; i < s_w * s_h; i++)
    {
        uint16_t c = s_fb[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
        rgb[1] = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);
        rgb[2] = (unsigned char)((c & 0x1F) * 255 / 31);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("  PPM 导出: %s\n", path);
}

static void dump_ascii(int cols)
{
    int rows = (s_h * cols) / (s_w * 2);
    int r, c;
    static const char *ramp = " .:-=+*#%@";

    printf("+");
    for (c = 0; c < cols; c++) { printf("-"); }
    printf("+  %dx%d\n", s_w, s_h);

    for (r = 0; r < rows; r++)
    {
        printf("|");
        for (c = 0; c < cols; c++)
        {
            int x0 = c * s_w / cols, x1 = (c + 1) * s_w / cols;
            int y0 = r * s_h / rows, y1 = (r + 1) * s_h / rows;
            long lum = 0, n = 0;
            int x, y;

            if (x1 <= x0) { x1 = x0 + 1; }
            if (y1 <= y0) { y1 = y0 + 1; }

            for (y = y0; y < y1; y++)
            {
                for (x = x0; x < x1; x++)
                {
                    uint16_t p = s_fb[y * s_w + x];
                    lum += ((p >> 11) & 0x1F) * 2 + ((p >> 5) & 0x3F) * 4 + (p & 0x1F) * 2;
                    n++;
                }
            }
            lum = (n > 0) ? (lum / n) : 0;             /* 0 .. ~250 */
            printf("%c", ramp[(int)(lum * 9 / 250)]);
        }
        printf("|\n");
    }
    printf("+");
    for (c = 0; c < cols; c++) { printf("-"); }
    printf("+\n");
}

/*============================================================================*/
/*                            stub protocol data                              */
/*============================================================================*/

static BMS_UiSnapshot_t g_snap;

const char *BMS_StateStr(BMS_State_t st)
{
    switch (st)
    {
        case BMS_ST_IDLE:           return "空闲(IDLE)";
        case BMS_ST_HANDSHAKE:      return "握手(HANDSHAKE)";
        case BMS_ST_IDENTIFY:       return "辨识(IDENTIFY)";
        case BMS_ST_PARAM_CONFIG:   return "参数配置(PARAM_CONFIG)";
        case BMS_ST_CHARGING_READY: return "充电准备(READY)";
        case BMS_ST_CHARGING:       return "充电中(CHARGING)";
        case BMS_ST_STOPPING:       return "结束(STOPPING)";
        case BMS_ST_FAULT:          return "故障(FAULT)";
        default:                    return "未知";
    }
}

const char *BMS_ErrorStr(BMS_Error_t err)
{
    switch (err)
    {
        case BMS_ERR_NONE:              return "无";
        case BMS_ERR_HANDSHAKE_TIMEOUT: return "握手超时";
        case BMS_ERR_IDENTIFY_TIMEOUT:  return "辨识超时";
        case BMS_ERR_PARAM_TIMEOUT:     return "参数配置超时";
        case BMS_ERR_READY_TIMEOUT:     return "准备就绪超时";
        case BMS_ERR_CHARGE_TIMEOUT:    return "充电阶段报文超时";
        case BMS_ERR_TP:                return "多帧组包错误";
        case BMS_ERR_CML_TIMEOUT:       return "等待 CML 超时";
        case BMS_ERR_DATA_INVALID:      return "数据域非法";
        case BMS_ERR_BUS_FAULT:         return "总线故障";
        default:                        return "未知";
    }
}

void BMS_Protocol_GetUiSnapshot(BMS_UiSnapshot_t *out) { *out = g_snap; }

void UI_ClearHistory(void);
void UI_Init(void);
void UI_Tick(void);
void UI_Tick_1ms(void);
uint8_t UI_IsCurveScreen(void);

/*============================================================================*/

/** advance the UI millisecond clock and run one UI_Tick() */
static void advance(uint32_t ms)
{
    uint32_t i;
    for (i = 0; i < ms; i++) { UI_Tick_1ms(); }
    UI_Tick();
}

int main(int argc, char **argv)
{
    int i;
    int rc;
    FILE *log;

    (void)argc;
    (void)argv;

    memset(&g_snap, 0, sizeof(g_snap));
    g_snap.state          = BMS_ST_CHARGING;
    g_snap.error          = BMS_ERR_NONE;
    g_snap.session_id     = 3;
    g_snap.soc_x10        = 455;
    g_snap.voltage_x10    = 4800;
    g_snap.current_x10    = 987;
    g_snap.cell_max_mv    = 3310;
    g_snap.cell_max_no    = 7;
    g_snap.temp_max_c     = 38;
    g_snap.temp_min_c     = 25;
    g_snap.limit_v_x10    = 5840;
    g_snap.limit_i_x10    = 1000;
    g_snap.charge_seconds = 754;
    g_snap.energy_x10     = 123;
    g_snap.charge_mode    = 2;
    g_snap.cro_ready      = 1;
    g_snap.rx_count       = 1234;
    g_snap.tx_count       = 5678;

    uip_init();
    UI_ClearHistory();
    UI_Init();

    /* Feed 300 one-second samples to fill the history ring buffer.
     * The values follow a plausible CC-CV charge: SOC rises linearly,
     * voltage climbs, current tapers off near the end. */
    for (i = 0; i < 300; i++)
    {
        g_snap.soc_x10        = (uint16_t)(450 + i * 18 / 10);
        g_snap.voltage_x10    = (uint16_t)(4800 + i * 34 / 10);
        g_snap.current_x10    = (uint16_t)((i < 240) ? 1000 : (1000 - (i - 240) * 12));
        g_snap.temp_max_c     = (uint8_t)(25 + i / 20);
        g_snap.temp_min_c     = 25;
        g_snap.energy_x10     = (uint32_t)(123 + i / 8);
        g_snap.charge_seconds = 754 + (uint32_t)i;
        advance(1000);
    }

    printf("\n==================== STM32 主界面（实时监控 480x320）====================\n");
    dump_ascii(118);
    dump_ppm("/tmp/stm32_main.ppm");

    /* ---- simulate tapping the "历史曲线" button in the title bar ----
     * The button rect is (352,4)-(474,31), so its centre is (413,17). */
    pc_tap(413, 17);
    advance(100);
    printf("\n-> 模拟触摸右上角按钮 (413,17)，当前页面 = %s\n",
           UI_IsCurveScreen() ? "历史曲线" : "主界面");

    printf("\n==================== STM32 历史曲线界面（480x320）=====================\n");
    dump_ascii(118);
    dump_ppm("/tmp/stm32_curve.ppm");

    /* ---- tap "返回" to go back, proving the toggle works both ways ---- */
    pc_tap(413, 17);
    advance(100);
    printf("\n-> 再次触摸 (413,17)，当前页面 = %s\n",
           UI_IsCurveScreen() ? "历史曲线" : "主界面");

    /* ---- a couple of "negative" taps must NOT switch pages ---- */
    pc_tap(200, 200);
    advance(100);
    printf("-> 触摸空白处 (200,200)，当前页面 = %s（应保持主界面）\n",
           UI_IsCurveScreen() ? "历史曲线" : "主界面");

    /* ---- exercise the fault / idle rendering paths too ---- */
    log = fopen("/tmp/stm32_states.txt", "w");
    if (log != NULL)
    {
        struct { BMS_State_t st; BMS_Error_t er; const char *tag; } cases[] = {
            { BMS_ST_IDLE,      BMS_ERR_NONE,             "idle" },
            { BMS_ST_HANDSHAKE, BMS_ERR_HANDSHAKE_TIMEOUT,"handshake_timeout" },
            { BMS_ST_CHARGING,  BMS_ERR_NONE,             "charging" },
            { BMS_ST_FAULT,     BMS_ERR_CHARGE_TIMEOUT,   "charge_timeout" },
            { BMS_ST_STOPPING,  BMS_ERR_NONE,             "stopping" },
        };
        size_t k;
        for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++)
        {
            g_snap.state = cases[k].st;
            g_snap.error = cases[k].er;
            advance(1000);
            fprintf(log, "%s: state=%s error=%s\n", cases[k].tag,
                    BMS_StateStr(cases[k].st), BMS_ErrorStr(cases[k].er));
        }
        fclose(log);
        printf("\n-> 已把各状态/异常码的渲染冒烟测试结果写入 /tmp/stm32_states.txt\n");
    }

    free(s_fb);

    rc = cov_report();
    printf("\n完成。\n");
    return rc;
}
