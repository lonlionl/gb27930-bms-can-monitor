/**
 * @file    fbdev.c
 * @brief   Linux framebuffer 图形抽象层实现
 *
 * 实现要点
 * --------
 * 1. 起手用 FBIOGET_VSCREENINFO + FBIOGET_FSCREENINFO 拿到真实的分辨率、
 *    bpp、每行字节数、可见区偏移以及 R/G/B 在像素里的位域位置，
 *    这样同一份代码在 RGB565 / RGB888(24bpp) / ARGB8888 三种常见
 *    屏上都能正确定色（I.MX6ULL 的 RGB LCD 这三种都可能遇到）。
 * 2. 每行字节数 line_len 常常大于 w*bpp/8（显存按 16 字节对齐），
 *    写显存时必须按 line_len 步进，不能按 w 算。
 * 3. 所有绘制都发生在 32 位 ARGB 后台缓冲上，fb_present() 才做格式转换，
 *    因此绘图代码不用关心底层格式。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fbdev.h"
#include "font16.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

/*==============================================================================
 *                              生命周期
 *============================================================================*/

static int alloc_back(fbdev_t *fb, int w, int h)
{
    size_t n = (size_t)w * (size_t)h * sizeof(uint32_t);

    fb->back = (uint32_t *)malloc(n);
    if (fb->back == NULL)
    {
        return -ENOMEM;
    }
    fb->w = w;
    fb->h = h;
    memset(fb->back, 0, n);
    return 0;
}

int fb_open(fbdev_t *fb, const char *dev)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    const char *path = (dev != NULL) ? dev : FB_DEFAULT_DEV;
    size_t map_len;

    if (fb == NULL)
    {
        return -EINVAL;
    }
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;

    fb->fd = open(path, O_RDWR);
    if (fb->fd < 0)
    {
        return -1000;   /* 设备不存在 */
    }

    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) < 0)
    {
        int e = errno;
        close(fb->fd);
        fb->fd = -1;
        return -e;
    }

    fb->w        = (int)var.xres;
    fb->h        = (int)var.yres;
    fb->bpp      = (int)var.bits_per_pixel;
    fb->line_len = (int)fix.line_length;
    fb->xoff     = (int)var.xoffset;
    fb->yoff     = (int)var.yoffset;

    fb->r_off = var.red.offset;    fb->r_len = var.red.length;
    fb->g_off = var.green.offset;  fb->g_len = var.green.length;
    fb->b_off = var.blue.offset;   fb->b_len = var.blue.length;

    map_len = (size_t)fix.line_length * (size_t)var.yres_virtual;
    if (map_len == 0)
    {
        map_len = (size_t)fix.line_length * (size_t)var.yres;
    }

    fb->mem = (uint8_t *)mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED)
    {
        int e = errno;
        fb->mem = NULL;
        close(fb->fd);
        fb->fd = -1;
        return -e;
    }
    fb->mem_len = map_len;

    if (alloc_back(fb, fb->w, fb->h) != 0)
    {
        munmap(fb->mem, fb->mem_len);
        fb->mem = NULL;
        close(fb->fd);
        fb->fd = -1;
        return -ENOMEM;
    }

    fb->opened = 1;
    fb->virtual_mode = 0;
    return 0;
}

int fb_open_virtual(fbdev_t *fb, int w, int h)
{
    if (fb == NULL || w <= 0 || h <= 0)
    {
        return -EINVAL;
    }
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;
    fb->virtual_mode = 1;

    /* 虚拟模式假定 16bpp RGB565，仅用于离线渲染出图 */
    fb->bpp = 16;
    fb->line_len = w * 2;
    fb->r_off = 11; fb->r_len = 5;
    fb->g_off = 5;  fb->g_len = 6;
    fb->b_off = 0;  fb->b_len = 5;

    if (alloc_back(fb, w, h) != 0)
    {
        return -ENOMEM;
    }
    fb->opened = 1;
    return 0;
}

void fb_close(fbdev_t *fb)
{
    if (fb == NULL)
    {
        return;
    }
    if (fb->mem != NULL && fb->mem_len > 0)
    {
        munmap(fb->mem, fb->mem_len);
        fb->mem = NULL;
    }
    if (fb->fd >= 0)
    {
        close(fb->fd);
        fb->fd = -1;
    }
    free(fb->back);
    fb->back = NULL;
    fb->opened = 0;
}

/*==============================================================================
 *                              格式转换与刷新
 *============================================================================*/

/** 从 ARGB 里按位域取一段并放到目标位置 */
static inline uint32_t field(uint32_t v, uint32_t off, uint32_t len)
{
    if (len == 0)
    {
        return 0;
    }
    if (len > 8)
    {
        len = 8;
    }
    return ((v >> (8 - len)) & ((1u << len) - 1u)) << off;
}

static inline uint32_t pack_pixel(const fbdev_t *fb, uint32_t argb)
{
    uint32_t r = (argb >> 16) & 0xFF;
    uint32_t g = (argb >> 8) & 0xFF;
    uint32_t b = argb & 0xFF;

    return field(r, fb->r_off, fb->r_len) |
           field(g, fb->g_off, fb->g_len) |
           field(b, fb->b_off, fb->b_len);
}

void fb_present(fbdev_t *fb)
{
    int x, y;

    if (fb == NULL || !fb->opened || fb->virtual_mode || fb->mem == NULL)
    {
        return;
    }

    for (y = 0; y < fb->h; y++)
    {
        uint8_t *dst = fb->mem + (size_t)(y + fb->yoff) * (size_t)fb->line_len;
        const uint32_t *src = fb->back + (size_t)y * (size_t)fb->w;

        switch (fb->bpp)
        {
            case 16:
            {
                uint16_t *p = (uint16_t *)(dst + (size_t)fb->xoff * 2);
                for (x = 0; x < fb->w; x++)
                {
                    p[x] = (uint16_t)pack_pixel(fb, src[x]);
                }
                break;
            }
            case 24:
            {
                uint8_t *p = dst + (size_t)fb->xoff * 3;
                for (x = 0; x < fb->w; x++)
                {
                    uint32_t v = pack_pixel(fb, src[x]);
                    p[0] = (uint8_t)(v & 0xFF);
                    p[1] = (uint8_t)((v >> 8) & 0xFF);
                    p[2] = (uint8_t)((v >> 16) & 0xFF);
                    p += 3;
                }
                break;
            }
            case 32:
            default:
            {
                uint32_t *p = (uint32_t *)(dst + (size_t)fb->xoff * 4);
                for (x = 0; x < fb->w; x++)
                {
                    p[x] = pack_pixel(fb, src[x]);
                }
                break;
            }
        }
    }
}

/*==============================================================================
 *                              绘图原语
 *============================================================================*/

void fb_clear(fbdev_t *fb, uint32_t color)
{
    size_t i, n;

    if (fb == NULL || fb->back == NULL)
    {
        return;
    }
    n = (size_t)fb->w * (size_t)fb->h;
    for (i = 0; i < n; i++)
    {
        fb->back[i] = color;
    }
}

void fb_pixel(fbdev_t *fb, int x, int y, uint32_t color)
{
    if (fb == NULL || fb->back == NULL)
    {
        return;
    }
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h)
    {
        return;
    }
    fb->back[(size_t)y * (size_t)fb->w + (size_t)x] = color;
}

void fb_hline(fbdev_t *fb, int x, int y, int w, uint32_t color)
{
    int i;

    if (fb == NULL || fb->back == NULL || w <= 0)
    {
        return;
    }
    if (y < 0 || y >= fb->h)
    {
        return;
    }
    if (x < 0) { w += x; x = 0; }
    if (x + w > fb->w) { w = fb->w - x; }
    for (i = 0; i < w; i++)
    {
        fb->back[(size_t)y * (size_t)fb->w + (size_t)(x + i)] = color;
    }
}

void fb_vline(fbdev_t *fb, int x, int y, int h, uint32_t color)
{
    int i;

    if (fb == NULL || fb->back == NULL || h <= 0)
    {
        return;
    }
    if (x < 0 || x >= fb->w)
    {
        return;
    }
    if (y < 0) { h += y; y = 0; }
    if (y + h > fb->h) { h = fb->h - y; }
    for (i = 0; i < h; i++)
    {
        fb->back[(size_t)(y + i) * (size_t)fb->w + (size_t)x] = color;
    }
}

void fb_fill(fbdev_t *fb, int x, int y, int w, int h, uint32_t color)
{
    int i;

    for (i = 0; i < h; i++)
    {
        fb_hline(fb, x, y + i, w, color);
    }
}

void fb_rect(fbdev_t *fb, int x, int y, int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0)
    {
        return;
    }
    fb_hline(fb, x, y, w, color);
    fb_hline(fb, x, y + h - 1, w, color);
    fb_vline(fb, x, y, h, color);
    fb_vline(fb, x + w - 1, y, h, color);
}

void fb_line(fbdev_t *fb, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;)
    {
        int e2;

        fb_pixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }

        /* 【重要】两个判断必须用**同一个** e2。
         *
         * 原来是写成 (err << 1) 各算一次，而第一个判断已经把 err 改掉了 ——
         * 于是第二个判断用的是更新后的 err，不再是经典 Bresenham。
         * 后果：某些斜率的直线会走歪且**永远到不了终点**：
         * 比如 (38,89)->(154,41) 会一直往右走、x 越界也不停，
         * 整个程序就卡死在这一条线里（实测就是这个现象）。
         *
         * 先算一次 e2 再判断两次，才是正确的写法。 */
        e2 = err << 1;
        if (e2 > -dy)
        {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

void fb_polyline(fbdev_t *fb, const int *xs, const int *ys, int n, uint32_t color)
{
    int i;

    if (xs == NULL || ys == NULL || n < 2)
    {
        return;
    }
    for (i = 0; i + 1 < n; i++)
    {
        fb_line(fb, xs[i], ys[i], xs[i + 1], ys[i + 1], color);
    }
}

void fb_bar_h(fbdev_t *fb, int x, int y, int w, int h,
              double ratio, uint32_t fill, uint32_t track)
{
    int inner = w - 4;

    if (inner < 1)
    {
        inner = 1;
    }
    if (ratio < 0.0) { ratio = 0.0; }
    if (ratio > 1.0) { ratio = 1.0; }

    fb_fill(fb, x, y, w, h, track);
    fb_fill(fb, x + 2, y + 2, (int)(inner * ratio + 0.5), h - 4, fill);
    fb_rect(fb, x, y, w, h, FB_COL_BORDER);
}

/*==============================================================================
 *                              文字
 *============================================================================*/

/** UTF-8 解码：返回码点并前移指针 */
static uint32_t utf8_next(const char **pp)
{
    const unsigned char *s = (const unsigned char *)(*pp);
    uint32_t cp;

    if (s[0] == 0)
    {
        return 0;
    }
    if (s[0] < 0x80u)
    {
        *pp += 1;
        return s[0];
    }
    if ((s[0] & 0xE0u) == 0xC0u && s[1] != 0)
    {
        cp = ((uint32_t)(s[0] & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
        *pp += 2;
        return cp;
    }
    if ((s[0] & 0xF0u) == 0xE0u && s[1] != 0 && s[2] != 0)
    {
        cp = ((uint32_t)(s[0] & 0x0Fu) << 12) |
             ((uint32_t)(s[1] & 0x3Fu) << 6) |
              (uint32_t)(s[2] & 0x3Fu);
        *pp += 3;
        return cp;
    }
    if ((s[0] & 0xF8u) == 0xF0u && s[1] != 0 && s[2] != 0 && s[3] != 0)
    {
        cp = ((uint32_t)(s[0] & 0x07u) << 18) |
             ((uint32_t)(s[1] & 0x3Fu) << 12) |
             ((uint32_t)(s[2] & 0x3Fu) << 6) |
              (uint32_t)(s[3] & 0x3Fu);
        *pp += 4;
        return cp;
    }
    *pp += 1;
    return '?';
}

/** 码点 -> 字模下标；-1 表示字库里没有 */
static int glyph_index(uint32_t cp)
{
    int i;

    if (cp >= FONT_ASCII_FIRST && cp <= FONT_ASCII_LAST)
    {
        return (int)(cp - FONT_ASCII_FIRST);
    }
    for (i = 0; i < (int)FONT_CJK_COUNT; i++)
    {
        if (g_font_cjk_map[i].codepoint == cp)
        {
            return (int)g_font_cjk_map[i].index;
        }
    }
    return -1;
}

/**
 * @brief  画出单个字模
 * @param  width 该字模的绘制宽度：ASCII 为 FONT_ASC_WIDTH，汉字为 FONT_CELL
 */
static void draw_glyph(fbdev_t *fb, int x, int y, int idx, int width, uint32_t color)
{
    const uint8_t *g;
    int rx, ry;

    if (idx < 0)
    {
        return;
    }
    g = &g_font_data[(size_t)idx * FONT_GLYPH_BYTES];

    for (ry = 0; ry < (int)FONT_CELL; ry++)
    {
        uint16_t row = (uint16_t)(((uint16_t)g[ry * FONT_ROWS] << 8) |
                                   (uint16_t)g[ry * FONT_ROWS + 1]);
        for (rx = 0; rx < width; rx++)
        {
            if (row & (uint16_t)(0x8000u >> rx))
            {
                fb_pixel(fb, x + rx, y + ry, color);
            }
        }
    }
}

int fb_text_w(const char *utf8)
{
    const char *p = utf8;
    int w = 0;

    if (utf8 == NULL)
    {
        return 0;
    }
    while (*p != '\0')
    {
        uint32_t cp = utf8_next(&p);
        if (cp == 0)
        {
            break;
        }
        w += (cp < 0x80u) ? (int)FONT_ASC_WIDTH : (int)FONT_CELL;
    }
    return w;
}

int fb_text(fbdev_t *fb, int x, int y, const char *utf8, uint32_t color)
{
    const char *p = utf8;
    int cx = x;

    if (utf8 == NULL)
    {
        return x;
    }
    while (*p != '\0')
    {
        uint32_t cp = utf8_next(&p);
        int idx, wdt;

        if (cp == 0)
        {
            break;
        }
        if (cp == '\n')
        {
            continue;
        }
        idx = glyph_index(cp);
        if (cp < 0x80u)
        {
            wdt = (int)FONT_ASC_WIDTH;
            draw_glyph(fb, cx, y, idx, wdt, color);
        }
        else
        {
            wdt = (int)FONT_CELL;
            draw_glyph(fb, cx, y, idx, wdt, color);
        }
        cx += wdt;
    }
    return cx;
}

void fb_text_center(fbdev_t *fb, int cx, int y, const char *utf8, uint32_t color)
{
    fb_text(fb, cx - fb_text_w(utf8) / 2, y, utf8, color);
}

void fb_text_clip(fbdev_t *fb, int x, int y, int max_w,
                  const char *utf8, uint32_t color)
{
    const char *p = utf8;
    int cx = x;

    if (utf8 == NULL)
    {
        return;
    }
    while (*p != '\0')
    {
        uint32_t cp = utf8_next(&p);
        int idx, wdt;

        if (cp == 0)
        {
            break;
        }
        wdt = (cp < 0x80u) ? (int)FONT_ASC_WIDTH : (int)FONT_CELL;
        if (cx + wdt > x + max_w)
        {
            break;
        }
        idx = glyph_index(cp);
        draw_glyph(fb, cx, y, idx, wdt, color);
        cx += wdt;
    }
}

/*==============================================================================
 *                              自检 / 出图
 *============================================================================*/

int fb_dump_ppm(const fbdev_t *fb, const char *path)
{
    FILE *fp;
    int x, y;

    if (fb == NULL || fb->back == NULL || path == NULL)
    {
        return -1;
    }
    fp = fopen(path, "wb");
    if (fp == NULL)
    {
        return -1;
    }
    fprintf(fp, "P6\n%d %d\n255\n", fb->w, fb->h);
    for (y = 0; y < fb->h; y++)
    {
        for (x = 0; x < fb->w; x++)
        {
            uint32_t v = fb->back[(size_t)y * (size_t)fb->w + (size_t)x];
            fputc((int)((v >> 16) & 0xFF), fp);
            fputc((int)((v >> 8) & 0xFF), fp);
            fputc((int)(v & 0xFF), fp);
        }
    }
    fclose(fp);
    return 0;
}

void fb_dump_ascii(const fbdev_t *fb, int cols)
{
    static const char *ramp = " .:-=+*#%@";
    int rows, cw, ch, r, c;

    if (fb == NULL || fb->back == NULL || cols < 20)
    {
        return;
    }
    /* 字符画是宽大于高的，按 2:1 的字符宽高比换算行数 */
    cw = fb->w / cols;
    if (cw < 1)
    {
        cw = 1;
    }
    ch = cw * 2;
    rows = fb->h / ch;

    printf("+");
    for (c = 0; c < cols; c++)
    {
        putchar('-');
    }
    printf("+  %dx%d\n", fb->w, fb->h);

    for (r = 0; r < rows; r++)
    {
        putchar('|');
        for (c = 0; c < cols; c++)
        {
            long sum = 0;
            int n = 0, x, y, lv;

            for (y = r * ch; y < (r + 1) * ch && y < fb->h; y++)
            {
                for (x = c * cw; x < (c + 1) * cw && x < fb->w; x++)
                {
                    uint32_t v = fb->back[(size_t)y * (size_t)fb->w + (size_t)x];
                    /* 粗略亮度 */
                    sum += (long)(((v >> 16) & 0xFF) * 30 +
                                  ((v >> 8) & 0xFF) * 59 +
                                  (v & 0xFF) * 11) / 100;
                    n++;
                }
            }
            lv = (n > 0) ? (int)(sum / n) : 0;
            lv = lv * 9 / 255;
            putchar(ramp[lv]);
        }
        printf("|\n");
    }

    printf("+");
    for (c = 0; c < cols; c++)
    {
        putchar('-');
    }
    printf("+\n");
}

/******************* (C) COPYRIGHT 2025 CAN Monitor *****END OF FILE****/
