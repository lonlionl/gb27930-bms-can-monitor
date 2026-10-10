/**
 * @file    fbdev.h
 * @brief   Linux framebuffer 图形抽象层（零依赖，直接操作 /dev/fb0）
 *
 * 设计说明
 * --------
 * 1. **为什么不用 Qt**
 *    I.MX6ULL 只有 512MB DDR、跑 Debian，交叉编译一套 Qt 需要几 GB 工具链
 *    和 1~3 小时构建；而本项目 UI 只需要「几个矩形 + 文字 + 一条折线」，
 *    直接操作 framebuffer 反而更小更快，还能和已有的 C 代码/SQLite 共用
 *    同一个进程，省掉 IPC。所以这里用 mmap + 自绘的方式实现。
 *
 * 2. **双缓冲**
 *    所有绘制先落在内存中的 32 位 ARGB 后台缓冲上，画完调用 fb_present()
 *    一次性刷到 /dev/fb0。这样既没有闪烁，也避免了直接写显存时的撕裂。
 *
 * 3. **像素格式无关**
 *    后台缓冲固定 32 位 ARGB，fb_present() 按 fb0 的实际 bpp
 *    （16/24/32）做转换，因此在 RGB565 / RGB888 / ARGB8888 三种屏上都能用。
 *
 * 4. **文字**
 *    使用 tools/gen_font.py 生成的 16x16 点阵字库：
 *      · ASCII 占 8 列、步进 8；汉字占 16 列、步进 16
 *      · 自带 UTF-8 解码，可直接 printf 风格传中文字符串
 */

#ifndef FBDEV_H
#define FBDEV_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 默认 framebuffer 设备 */
#define FB_DEFAULT_DEV   "/dev/fb0"

/** 颜色构造（0xRRGGBB） */
#define FB_RGB(r, g, b)   ((uint32_t)(((uint32_t)(r) << 16) | \
                                      ((uint32_t)(g) << 8)  | \
                                       (uint32_t)(b)))

/*---------------------------------------------------------------------------
 * 工业 UI 配色（深色主题，和 CAN 监控终端的终端版配色保持一致）
 *-------------------------------------------------------------------------*/
#define FB_COL_BG         FB_RGB(0x14, 0x18, 0x20)   /* 背景       */
#define FB_COL_PANEL      FB_RGB(0x1E, 0x24, 0x30)   /* 面板底色   */
#define FB_COL_BORDER     FB_RGB(0x35, 0x40, 0x52)   /* 边框       */
#define FB_COL_TEXT       FB_RGB(0xE6, 0xEC, 0xF4)   /* 主文字     */
#define FB_COL_DIM        FB_RGB(0x8A, 0x96, 0xA8)   /* 次要文字   */
#define FB_COL_GREEN      FB_RGB(0x3D, 0xD6, 0x8C)   /* 正常       */
#define FB_COL_YELLOW     FB_RGB(0xF2, 0xC1, 0x4E)   /* 警告       */
#define FB_COL_RED        FB_RGB(0xF0, 0x5A, 0x5A)   /* 故障       */
#define FB_COL_BLUE       FB_RGB(0x4C, 0x9A, 0xFF)   /* 强调/曲线  */
#define FB_COL_CYAN       FB_RGB(0x42, 0xD0, 0xE0)   /* 标题       */
#define FB_COL_ORANGE     FB_RGB(0xFF, 0x9A, 0x3D)   /* 电流曲线   */

/** framebuffer 上下文 */
typedef struct
{
    int       fd;            /**< /dev/fb0 文件描述符，虚拟模式为 -1 */
    uint8_t  *mem;           /**< mmap 到的显存首地址 */
    size_t    mem_len;       /**< 显存长度 */
    int       w;             /**< 可见宽度（像素） */
    int       h;             /**< 可见高度（像素） */
    int       line_len;      /**< 每行字节数（可能大于 w*bpp/8） */
    int       bpp;           /**< 每像素位数：16 / 24 / 32 */
    int       xoff;          /**< 可见区在显存中的偏移 */
    int       yoff;
    uint32_t *back;          /**< 32 位 ARGB 后台缓冲 */
    int       opened;
    int       virtual_mode;  /**< 1 = 没打开真实设备（离线渲染/自检用） */

    /* 显存像素格式描述（取自 FBIOGET_VSCREENINFO，用于把 ARGB 打包成显存格式） */
    uint32_t  r_off, r_len;
    uint32_t  g_off, g_len;
    uint32_t  b_off, b_len;
} fbdev_t;

/*---------------------------------------------------------------------------
 * 生命周期
 *-------------------------------------------------------------------------*/

/**
 * @brief  打开 framebuffer 设备
 * @param  fb   上下文
 * @param  dev  设备路径，NULL 用 FB_DEFAULT_DEV
 * @return 0 成功；负值为 errno；-1000 表示设备不存在
 */
int  fb_open(fbdev_t *fb, const char *dev);

/**
 * @brief  创建纯内存（虚拟）framebuffer，不接触真实设备
 * @note   用于在 PC 上离线渲染 UI 做自检 / 出图，
 *         这样布局代码不依赖开发板就能验证。
 */
int  fb_open_virtual(fbdev_t *fb, int w, int h);

/** 关闭并释放资源 */
void fb_close(fbdev_t *fb);

/** 把后台缓冲刷新到显存（虚拟模式下为空操作） */
void fb_present(fbdev_t *fb);

/*---------------------------------------------------------------------------
 * 绘图原语（全部画在后台缓冲上）
 *-------------------------------------------------------------------------*/

void fb_clear(fbdev_t *fb, uint32_t color);
void fb_pixel(fbdev_t *fb, int x, int y, uint32_t color);
void fb_hline(fbdev_t *fb, int x, int y, int w, uint32_t color);
void fb_vline(fbdev_t *fb, int x, int y, int h, uint32_t color);
void fb_fill(fbdev_t *fb, int x, int y, int w, int h, uint32_t color);
void fb_rect(fbdev_t *fb, int x, int y, int w, int h, uint32_t color);
void fb_line(fbdev_t *fb, int x0, int y0, int x1, int y1, uint32_t color);
/** 画一条折线（用于历史曲线） */
void fb_polyline(fbdev_t *fb, const int *xs, const int *ys, int n, uint32_t color);
/** 横向/纵向进度条 */
void fb_bar_h(fbdev_t *fb, int x, int y, int w, int h,
              double ratio, uint32_t fill, uint32_t track);

/*---------------------------------------------------------------------------
 * 文字
 *-------------------------------------------------------------------------*/

/** 计算 UTF-8 字符串的像素宽度 */
int  fb_text_w(const char *utf8);

/**
 * @brief  在 (x, y) 画一行 UTF-8 文本
 * @param  y 文本单元格的左上角 y
 * @return 绘制结束后的 x 坐标
 */
int  fb_text(fbdev_t *fb, int x, int y, const char *utf8, uint32_t color);

/** 以 cx 为中心画一行文本 */
void fb_text_center(fbdev_t *fb, int cx, int y, const char *utf8, uint32_t color);

/** 以 cx 为中心、限定在 [x0,x1] 内画文本，超出则截断 */
void fb_text_clip(fbdev_t *fb, int x, int y, int max_w,
                  const char *utf8, uint32_t color);

/*---------------------------------------------------------------------------
 * 自检 / 出图
 *-------------------------------------------------------------------------*/

/**
 * @brief  把后台缓冲写成 PPM(P6) 文件，便于在 PC 上目视检查布局
 * @return 0 成功
 */
int  fb_dump_ppm(const fbdev_t *fb, const char *path);

/**
 * @brief  把后台缓冲降采样成 ASCII 艺术图打印到 stdout
 * @param  cols 输出列数（建议 100~140）
 * @note   这是给「没有图形界面/没有看图工具」的场景准备的自检手段：
 *         直接把整屏布局画成字符画，一眼就能看出元素位置、是否重叠、
 *         文字有没有画到框外。
 */
void fb_dump_ascii(const fbdev_t *fb, int cols);

#ifdef __cplusplus
}
#endif

#endif /* FBDEV_H */
