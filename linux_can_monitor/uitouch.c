/**
 * @file    uitouch.c
 * @brief   触摸屏输入实现（evdev）
 *
 * 自动发现逻辑
 * ------------
 *   野火 4.3 寸屏（GT1151）在 /dev/input/eventX 上注册，X 不固定，
 *   所以不能写死 event0。这里依次枚举 event0..event15：
 *     ① EVIOCGNAME 取设备名，名字里含 gt/Goodix/touch/capacitive 的优先；
 *     ② EVIOCGBIT(EV_ABS) 检查有没有 ABS_X 或 ABS_MT_POSITION_X，
 *         并 EVIOCGBIT(EV_KEY) 检查有没有 BTN_TOUCH；
 *     ③ 两者都满足才认作触摸屏。
 *   另外会读 EVIOCGABS 拿到 ABS 的 min/max 用于坐标映射。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "uitouch.h"

extern int g_verbose;   /* 由 main.c 定义：调试日志开关 */

/* 只在 -v 时输出的日志宏（和 main.c 里的同名宏一致） */
#ifndef V
#define V(...)   do { if (g_verbose) { printf(__VA_ARGS__); } } while (0)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <time.h>               /* clock_gettime / CLOCK_MONOTONIC（演示模式计时） */
#include <sys/ioctl.h>
#include <linux/input.h>

/*==============================================================================
 *                              内部工具
 *============================================================================*/

/** 检查某个位是否在 bitmask 里 */
static int bit_is_set(const unsigned long *bits, int bit)
{
    return (int)((bits[bit / (int)(8 * sizeof(unsigned long))] >>
                  (bit % (int)(8 * sizeof(unsigned long)))) & 1UL);
}

/** 读取某个 ABS 轴的范围 */
static int read_abs_range(int fd, int axis, int *minv, int *maxv)
{
    struct input_absinfo info;

    memset(&info, 0, sizeof(info));
    if (ioctl(fd, EVIOCGABS(axis), &info) < 0)
    {
        return -1;
    }
    *minv = info.minimum;
    *maxv = info.maximum;
    return 0;
}

/**
 * @brief  判断一个 event 设备是否是触摸屏，并返回优先级
 * @return 0 = 不是；数字越大越可能是触摸屏
 */
static int probe_device(const char *path, char *name_out, size_t name_cap,
                        int *has_mt, int *amx, int *amxx, int *amy, int *amxy)
{
    unsigned long abs_bits[8];
    unsigned long key_bits[8];
    char name[64];
    int fd, score = 0;

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
    {
        return 0;
    }

    memset(name, 0, sizeof(name));
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
    {
        close(fd);
        return 0;
    }

    memset(abs_bits, 0, sizeof(abs_bits));
    memset(key_bits, 0, sizeof(key_bits));
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);

    {
        int has_abs_xy   = bit_is_set(abs_bits, ABS_X) && bit_is_set(abs_bits, ABS_Y);
        int has_mt_xy    = bit_is_set(abs_bits, ABS_MT_POSITION_X) &&
                           bit_is_set(abs_bits, ABS_MT_POSITION_Y);
        int has_btn      = bit_is_set(key_bits, BTN_TOUCH);

        if (!has_abs_xy && !has_mt_xy)
        {
            close(fd);
            return 0;
        }
        if (has_abs_xy || has_mt_xy)
        {
            score = 1;
        }
        if (has_btn)
        {
            score += 2;          /* 有 BTN_TOUCH 基本可以确定是触摸 */
        }
        if (has_mt_xy)
        {
            score += 1;
        }
    }

    /* 名字里带触摸关键字再加分 */
    {
        static const char *kw[] = { "gt", "goodix", "touch", "capacitive",
                                    "ts", "tp", NULL };
        /* 名字里带这些词的**减分**：它们虽然也上报 ABS_X/ABS_Y（绝对坐标），
         * 但根本不是触摸屏。最典型的例子是 VirtualBox 的 "USB Tablet" ——
         * 它和触摸屏一样有 ABS_X/ABS_Y，只看能力位会被误判成触摸屏，
         * 结果程序去读一个永远不会来的 BTN_TOUCH，表现为"点了没反应"。 */
        static const char *neg[] = { "tablet", "mouse", "keyboard", "power",
                                     "button", "video", "sleep", NULL };
        int i;
        char lower[64];
        size_t k;

        for (k = 0; k < sizeof(lower) - 1 && name[k] != '\0'; k++)
        {
            char c = name[k];
            lower[k] = (char)((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c);
        }
        lower[k] = '\0';

        for (i = 0; kw[i] != NULL; i++)
        {
            if (strstr(lower, kw[i]) != NULL)
            {
                score += 3;
                break;
            }
        }
        for (i = 0; neg[i] != NULL; i++)
        {
            if (strstr(lower, neg[i]) != NULL)
            {
                score -= 5;
                break;
            }
        }
    }

    /* 读取量程 */
    *has_mt = bit_is_set(abs_bits, ABS_MT_POSITION_X) ? 1 : 0;
    if (read_abs_range(fd, ABS_X, amx, amxx) != 0 &&
        read_abs_range(fd, ABS_MT_POSITION_X, amx, amxx) != 0)
    {
        *amx = 0; *amxx = 0;
    }
    if (read_abs_range(fd, ABS_Y, amy, amxy) != 0 &&
        read_abs_range(fd, ABS_MT_POSITION_Y, amy, amxy) != 0)
    {
        *amy = 0; *amxy = 0;
    }

    if (name_out != NULL && name_cap > 0)
    {
        snprintf(name_out, name_cap, "%s", name);
    }
    close(fd);
    return score;
}

/** 在所有 /dev/input/event* 里挑一个最像触摸屏的 */
static int find_touch_device(char *path_out, size_t cap,
                             char *name_out, size_t name_cap,
                             int *has_mt, int *amx, int *amxx, int *amy, int *amxy)
{
    int best_score = 0;
    int i;

    for (i = 0; i < 32; i++)
    {
        char path[64];
        char name[64];
        int  mt = 0, mx = 0, mxx = 0, my = 0, mxy = 0;
        int  score;

        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (access(path, R_OK) != 0)
        {
            continue;
        }
        score = probe_device(path, name, sizeof(name), &mt, &mx, &mxx, &my, &mxy);
        if (score > best_score)
        {
            best_score = score;
            snprintf(path_out, cap, "%s", path);
            snprintf(name_out, name_cap, "%s", name);
            *has_mt = mt;
            *amx = mx; *amxx = mxx; *amy = my; *amxy = mxy;
        }
    }
    return (best_score > 0) ? 0 : -1;
}

/*==============================================================================
 *                          演示模式（无触摸硬件时的脚本化触摸）
 *============================================================================*/

/** 演示模式的设备路径关键字 */
#define UITOUCH_DEMO_DEV   "demo"

/**
 * 演示脚本：{ 触发时刻(ms), x, y }
 *   坐标取标题栏按钮矩形的中心，矩形由 gui.c 的 gui_button_rect() 算出
 *   （下面的 DEMO_SCRIPT 里逐条列了对应的范围）：
 *     主界面   「数据曲线」x 314..401 -> (357,14)
 *     数据曲线 「历史」    x 332..401 -> (366,14)
 *   按钮都在标题栏里，y 4..23，中心 y = 14。
 *   第三次点的 (200,270) 用来验证"点空白不会误触发"：脚本跑到它时界面停在
 *   历史九宫格页，该页 y 270 这一行在九宫格与标题栏之外 —— 九宫格按
 *   cell_rect() 从 GRID_Y0=34 起、每格 CELL_H=74 + CELL_GAP=5，三行到
 *   y 265 结束；标题栏止于 y 23。y 270 落在 265 以下的空白带里。
 */
static const struct
{
    int at_ms;
    int x;
    int y;
} DEMO_SCRIPT[] = {
    /* 坐标必须和 gui_button_rect() 算出来的矩形对得上，
     * 否则"演示"就变成了点空白。当前布局（480x272）：
     *   主界面   充电      x 408..471   数据曲线 x 314..401
     *   数据曲线 历史      x 332..401   返回     x 408..471
     *   按钮都在标题栏里，y 4..23，中心 y = 14。 */
    { 2000, 357, 14 },      /* 主界面 ->「数据曲线」 */
    { 5000, 366, 14 },      /* 数据曲线 ->「历史」九宫格 */
    { 8000, 200, 270 },     /* 九宫格与标题栏之外的空白带：不应有任何切页 */
};
#define DEMO_SCRIPT_N  ((int)(sizeof(DEMO_SCRIPT) / sizeof(DEMO_SCRIPT[0])))

static uint64_t touch_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

/*==============================================================================
 *                              对外接口
 *============================================================================*/

int uitouch_open(uitouch_t *t, const char *dev, int screen_w, int screen_h)
{
    int rc;

    if (t == NULL)
    {
        return -EINVAL;
    }
    memset(t, 0, sizeof(*t));
    t->fd = -1;
    t->screen_w = (screen_w > 0) ? screen_w : 480;
    t->screen_h = (screen_h > 0) ? screen_h : 272;
    t->last_activity_ms = touch_now_ms();

    /* ---- 演示模式：不打开任何设备节点 ---- */
    if (dev != NULL && strcmp(dev, UITOUCH_DEMO_DEV) == 0)
    {
        t->demo = 1;
        t->demo_step = 0;
        t->demo_next_ms = (int)(touch_now_ms() + (uint64_t)DEMO_SCRIPT[0].at_ms);
        snprintf(t->path, sizeof(t->path), "%s", "(演示模式)");
        snprintf(t->name, sizeof(t->name), "%s", "脚本化触摸事件");
        t->abs_min_x = 0; t->abs_max_x = t->screen_w;
        t->abs_min_y = 0; t->abs_max_y = t->screen_h;
        return 0;
    }

    if (dev != NULL)
    {
        snprintf(t->path, sizeof(t->path), "%s", dev);

        /* 【重要】即使是用户显式指定的设备，也必须去读它真实的 ABS 量程！
         *
         * 踩过的坑：这里原来直接写 t->abs_max_x = screen_w，等于假设
         * 「设备上报的原始值就是屏幕像素」。可实际上电容屏报的是自己的
         * 坐标系（例如野火板的 Goodix 是 X 0..34799 / Y 0..13064）。
         * 一旦假设错了，map_axis() 会把所有原始值都当成远超屏幕的越界值、
         * 统统 cramp 到右下角 —— 表现就是「怎么点都没反应」，
         * 而且因为是显式指定设备才触发，格外难查。
         */
        rc = probe_device(t->path, t->name, sizeof(t->name), &t->has_mt,
                          &t->abs_min_x, &t->abs_max_x,
                          &t->abs_min_y, &t->abs_max_y);
        if (rc < 0)
        {
            snprintf(t->name, sizeof(t->name), "(指定，量程读取失败)");
            t->abs_min_x = 0; t->abs_max_x = t->screen_w;
            t->abs_min_y = 0; t->abs_max_y = t->screen_h;
        }
    }
    else
    {
        rc = find_touch_device(t->path, sizeof(t->path), t->name, sizeof(t->name),
                               &t->has_mt, &t->abs_min_x, &t->abs_max_x,
                               &t->abs_min_y, &t->abs_max_y);
        if (rc != 0)
        {
            return -1;
        }
    }

    t->fd = open(t->path, O_RDONLY | O_NONBLOCK);
    if (t->fd < 0)
    {
        return -errno;
    }

    /* 把两套量程都读出来存着：多点设备的 ABS_MT_POSITION_* 与单点的
     * ABS_X/Y 有可能报的不一样，出问题时把两个都打出来就知道该信哪个。 */
    if (read_abs_range(t->fd, ABS_X, &t->ax_min_x, &t->ax_max_x) != 0)
    {
        t->ax_min_x = 0; t->ax_max_x = 0;
    }
    if (read_abs_range(t->fd, ABS_Y, &t->ax_min_y, &t->ax_max_y) != 0)
    {
        t->ax_min_y = 0; t->ax_max_y = 0;
    }
    if (read_abs_range(t->fd, ABS_MT_POSITION_X, &t->mt_min_x, &t->mt_max_x) != 0)
    {
        t->mt_min_x = 0; t->mt_max_x = 0;
    }
    if (read_abs_range(t->fd, ABS_MT_POSITION_Y, &t->mt_min_y, &t->mt_max_y) != 0)
    {
        t->mt_min_y = 0; t->mt_max_y = 0;
    }

    /* 量程兜底：内核没上报 max 时按屏幕分辨率处理 */
    if (t->abs_max_x <= t->abs_min_x)
    {
        t->abs_min_x = 0;
        t->abs_max_x = t->screen_w;
    }
    if (t->abs_max_y <= t->abs_min_y)
    {
        t->abs_min_y = 0;
        t->abs_max_y = t->screen_h;
    }
    return 0;
}

void uitouch_close(uitouch_t *t)
{
    if (t == NULL)
    {
        return;
    }

    /* 先解除独占抓取，再关设备 —— 顺序反了就解不掉了，
     * 桌面的触摸得重启 X 才能恢复。 */
    uitouch_ungrab(t);

    if (t->fd >= 0)
    {
        close(t->fd);
        t->fd = -1;
    }
}

int uitouch_grab(uitouch_t *t)
{
    int rc;

    if (t == NULL || t->fd < 0)
    {
        return -EINVAL;
    }
    if (t->grabbed)
    {
        return 0;
    }

    rc = ioctl(t->fd, EVIOCGRAB, (void *)1);
    if (rc != 0)
    {
        return -errno;
    }

    t->grabbed = 1;
    return 0;
}

void uitouch_ungrab(uitouch_t *t)
{
    if (t == NULL || t->fd < 0 || !t->grabbed)
    {
        return;
    }

    (void)ioctl(t->fd, EVIOCGRAB, (void *)0);
    t->grabbed = 0;
}

/** 把设备原始坐标线性映射到屏幕坐标（实现在下面） */
static int  map_axis(int raw, int amin, int amax, int smax);
/** 一批事件处理完后统一施加交换/镜像，算出最终屏幕坐标（实现在下面） */
static void finalize_coords(uitouch_t *t);

void uitouch_set_calib(uitouch_t *t, int min_x, int max_x, int min_y, int max_y)
{
    if (t == NULL)
    {
        return;
    }
    t->abs_min_x = min_x; t->abs_max_x = max_x;
    t->abs_min_y = min_y; t->abs_max_y = max_y;
    finalize_coords(t);
}

/** 把设备原始坐标线性映射到屏幕坐标 */
static int map_axis(int raw, int amin, int amax, int smax)
{
    double t;

    if (amax <= amin)
    {
        return raw;
    }
    t = (double)(raw - amin) / (double)(amax - amin);
    if (t < 0.0) { t = 0.0; }
    if (t > 1.0) { t = 1.0; }
    return (int)(t * (double)smax + 0.5);
}

/**
 * @brief  把原始坐标换算成最终屏幕坐标（统一在这里施加交换/镜像）
 *
 * 为什么不在收到 ABS_X 时就换算
 * ----------------------------
 *   因为「交换 X/Y」必须两个轴的值都到齐了才能做。事件是一条一条来的，
 *   收到 ABS_X 的时候新的 ABS_Y 可能还没来。所以这里把换算推迟到
 *   「一批事件处理完」的时刻，两个轴拿到的永远是同一批的值。
 *
 * 交换时量程也跟着换：面板若是竖屏、显示是横屏，那么原始的 X 轴对应的
 * 其实是屏幕的**高**，映射目标就应该是 screen_h 而不是 screen_w。
 */
static void finalize_coords(uitouch_t *t)
{
    int w = t->swap_xy ? t->screen_h : t->screen_w;
    int h = t->swap_xy ? t->screen_w : t->screen_h;
    int a = map_axis(t->raw_x, t->abs_min_x, t->abs_max_x, w - 1);
    int b = map_axis(t->raw_y, t->abs_min_y, t->abs_max_y, h - 1);

    if (t->swap_xy)
    {
        int tmp = a;
        a = b;
        b = tmp;
    }
    if (t->mirror_x) { a = (t->screen_w - 1) - a; }
    if (t->mirror_y) { b = (t->screen_h - 1) - b; }

    if (a < 0) { a = 0; }
    if (b < 0) { b = 0; }
    if (a > t->screen_w - 1) { a = t->screen_w - 1; }
    if (b > t->screen_h - 1) { b = t->screen_h - 1; }

    t->x = a;
    t->y = b;
}

void uitouch_set_transform(uitouch_t *t, int swap_xy, int mirror_x, int mirror_y)
{
    if (t == NULL) { return; }
    t->swap_xy  = swap_xy  ? 1 : 0;
    t->mirror_x = mirror_x ? 1 : 0;
    t->mirror_y = mirror_y ? 1 : 0;
    finalize_coords(t);
}

void uitouch_set_debug(uitouch_t *t, int on)
{
    if (t == NULL) { return; }
    t->dbg = on ? 1 : 0;
}

void uitouch_set_range(uitouch_t *t, int xmin, int xmax, int ymin, int ymax)
{
    if (t == NULL) { return; }
    if (xmax <= xmin || ymax <= ymin) { return; }   /* 参数不合法就忽略 */
    t->abs_min_x = xmin; t->abs_max_x = xmax;
    t->abs_min_y = ymin; t->abs_max_y = ymax;
    t->range_overridden = 1;
    t->range_hint_done = 1;     /* 已经手工指定了，不用再提示 */
    finalize_coords(t);
}

/**
 * @brief  一致性检查：内核上报的量程和实测到的原始值对不上时给出提示
 *
 * 判据：内核说最大值是 M，而实测点遍全屏之后最大也只到 m，
 *       并且 m 明显小于 M（不到一半）—— 说明设备实际不是按 M 这个量程上报的。
 *
 * 为什么要专门做这个检查：这种「量程配错」的故障现象非常隐蔽 ——
 * 触摸事件明明都收到了、日志里坐标也在变，只是全挤在屏幕左上角一小块，
 * 看起来像「程序没响应」。没有这个提示就得靠人一点点猜。
 */
static void range_sanity_hint(uitouch_t *t)
{
    int span_x = t->abs_max_x - t->abs_min_x;
    int span_y = t->abs_max_y - t->abs_min_y;

    if (t->range_overridden || t->range_hint_done) { return; }
    if (t->press_count < 2) { return; }             /* 至少按过两下再判断 */
    if (span_x <= 0 || span_y <= 0) { return; }

    /* 实测最大值够不够接近内核说的最大值？差得远就是量程配错了 */
    if ((t->obs_max_x * 2 > span_x) && (t->obs_max_y * 2 > span_y))
    {
        return;                                     /* 看着是正常的 */
    }

    t->range_hint_done = 1;

    printf("\n[TOUCH] ⚠ 量程可能配错了\n");
    printf("        内核上报的量程 : X[%d..%d] Y[%d..%d]\n",
           t->abs_min_x, t->abs_max_x, t->abs_min_y, t->abs_max_y);
    printf("        实测最大原始值 : X=%d Y=%d\n", t->obs_max_x, t->obs_max_y);
    printf("        也就是说设备并不是按上面这个量程上报的，坐标会被压缩到\n");
    printf("        屏幕左上角一小块，看起来就像「点了没反应」。\n");
    printf("        如果实测值大致等于「屏幕像素」，请这样启动：\n");
    printf("            --touch-calib 0,%d,0,%d\n", t->screen_w, t->screen_h);
    printf("        如果实测值对应别的量程，把 --touch-calib 换成实测的最大值。\n\n");
}

void uitouch_get_mapped(const uitouch_t *t, int *x, int *y)
{
    if (t == NULL) { return; }
    if (x != NULL) { *x = t->x; }
    if (y != NULL) { *y = t->y; }
}

int uitouch_is_down(const uitouch_t *t)
{
    return (t != NULL && t->down) ? 1 : 0;
}

int uitouch_poll(uitouch_t *t, int timeout_ms)
{
    struct input_event ev;
    ssize_t n;
    int updated = 0;
    int first = 1;

    if (t == NULL)
    {
        return -EINVAL;
    }

    /* 边沿标志每次调用开始时清掉，这样调用方拿到的永远是
     * 「本次 poll 期间发生的按下/抬起」，而不是历史累计值。
     * （之前忘了清，结果第一次触摸之后 pressed 一直是 1，
     *   每次坐标变化都会被当成一次新的按下 —— 表现为界面自己乱跳页。） */
    if (t->fd >= 0 && !t->demo)
    {
        t->pressed  = 0;
        t->released = 0;
    }

    if (t == NULL || t->fd < 0)
    {
        /* ---- 演示模式：按脚本在预定时刻产生一次「按下」 ---- */
        if (t != NULL && t->demo)
        {
            if (t->demo_step < DEMO_SCRIPT_N &&
                (int)touch_now_ms() >= t->demo_next_ms)
            {
                t->x = DEMO_SCRIPT[t->demo_step].x;
                t->y = DEMO_SCRIPT[t->demo_step].y;
                t->raw_x = t->x;
                t->raw_y = t->y;
                t->down = 1;
                t->pressed = 1;         /* 一次性边沿，调用方读完即清 */
                t->ev_count++;
                V("[TOUCH] 演示脚本第 %d 步：按下 (%d,%d)\n",
                       t->demo_step + 1, t->x, t->y);

                t->demo_step++;
                if (t->demo_step < DEMO_SCRIPT_N)
                {
                    t->demo_next_ms = (int)(touch_now_ms() +
                        (uint64_t)(DEMO_SCRIPT[t->demo_step].at_ms -
                                   DEMO_SCRIPT[t->demo_step - 1].at_ms));
                }
                return 1;
            }
            /* 演示模式下没有真实事件，别让调用方误以为设备坏了 */
            return 0;
        }
        return -EINVAL;
    }

    for (;;)
    {
        /* 第一次循环按 timeout 等待，之后立即返回（把缓冲区读空） */
        if (!first || timeout_ms > 0)
        {
            struct pollfd pfd;
            int pr;

            pfd.fd = t->fd;
            pfd.events = POLLIN;
            do
            {
                pr = poll(&pfd, 1, first ? timeout_ms : 0);
            } while (pr < 0 && errno == EINTR);

            if (pr <= 0)
            {
                break;
            }
        }
        first = 0;

        n = read(t->fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev))
        {
            break;
        }

        t->ev_count++;

        /* 调试：把原始事件打出来（只打前若干个，否则一直刷屏反而看不清） */
        if (t->dbg && t->dbg_events < 60)
        {
            t->dbg_events++;
            printf("[TOUCH-DBG] 事件 #%d: type=%u code=%u value=%d\n",
                   t->dbg_events, (unsigned)ev.type, (unsigned)ev.code,
                   (int)ev.value);
        }

        switch (ev.type)
        {
            case EV_ABS:
                switch (ev.code)
                {
                    case ABS_X:
                    case ABS_MT_POSITION_X:
                        t->raw_x = ev.value;
                        t->last_activity_ms = touch_now_ms();
                        updated = 1;
                        break;
                    case ABS_Y:
                    case ABS_MT_POSITION_Y:
                        t->raw_y = ev.value;
                        t->last_activity_ms = touch_now_ms();
                        updated = 1;
                        break;
                    case ABS_MT_TRACKING_ID:
                        /* Type B 协议：有 ID = 按下，-1 = 抬起 */
                        if (ev.value >= 0 && !t->down)
                        {
                            t->down = 1;
                            t->pressed = 1;
                            t->last_activity_ms = touch_now_ms();
                            updated = 1;
                        }
                        else if (ev.value < 0 && t->down)
                        {
                            t->down = 0;
                            t->released = 1;
                            updated = 1;
                        }
                        break;
                    default:
                        break;
                }
                break;

            case EV_KEY:
                if (ev.code == BTN_TOUCH)
                {
                    if (ev.value && !t->down)
                    {
                        t->down = 1;
                        t->pressed = 1;
                        t->last_activity_ms = touch_now_ms();
                        updated = 1;
                    }
                    else if (!ev.value && t->down)
                    {
                        t->down = 0;
                        t->released = 1;
                        updated = 1;
                    }
                }
                break;

            case EV_SYN:
                break;

            default:
                break;
        }
    }

    /* ---- 自动抬手兜底 ----
     * 有些多点设备既不上报 BTN_TOUCH、也不用 ABS_MT_TRACKING_ID
     * （Type A 的变体），那样就没有任何"抬起"事件可依赖。
     * 兜底策略：按着不动且 300 ms 内没有任何坐标更新，就当作已经抬手。
     * 只影响这类"不发抬起事件"的设备，正常 Type B 走的还是真实事件。 */
    if (t->down && (touch_now_ms() - t->last_activity_ms) > 300u)
    {
        t->down = 0;
        t->released = 1;
        updated = 1;
    }

    /* ---- 一批事件处理完，统一换算坐标 ---- */
    if (updated)
    {
        /* 记下实测见过的最大原始值：用来判断内核上报的量程是否可信 */
        if (t->raw_x > t->obs_max_x) { t->obs_max_x = t->raw_x; }
        if (t->raw_y > t->obs_max_y) { t->obs_max_y = t->raw_y; }

        finalize_coords(t);

        if (t->pressed)
        {
            t->press_count++;
            range_sanity_hint(t);
        }

        if (t->dbg)
        {
            printf("[TOUCH-DBG] 原始(%d,%d) → 屏幕(%d,%d)  down=%d pressed=%d released=%d\n",
                   t->raw_x, t->raw_y, t->x, t->y,
                   t->down, t->pressed, t->released);
        }
    }

    return updated ? 1 : 0;
}

void uitouch_clear_edges(uitouch_t *t)
{
    if (t != NULL)
    {
        t->pressed = 0;
        t->released = 0;
    }
}

void uitouch_dump_info(const uitouch_t *t)
{
    if (t == NULL)
    {
        return;
    }
    V("[TOUCH] 设备 %s  名称 \"%s\"\n", t->path, t->name);
    if (t->demo)
    {
        V("[TOUCH] 演示模式：按脚本产生触摸事件（2s/5s 点按钮，8s 点空白）\n");
        return;
    }
    V("[TOUCH] 原始量程 X[%d..%d] Y[%d..%d] -> 屏幕 %dx%d%s\n",
           t->abs_min_x, t->abs_max_x, t->abs_min_y, t->abs_max_y,
           t->screen_w, t->screen_h, t->has_mt ? "  (多点协议)" : "  (单点协议)");

    /* 两套量程不一致时额外打一行 —— 这正是「坐标被压缩到左上角」的元凶 */
    if ((t->ax_max_x != t->mt_max_x) || (t->ax_max_y != t->mt_max_y))
    {
        V("[TOUCH] 注意：内核给的两套量程不一样\n");
        printf("        ABS_X/Y           : X[%d..%d] Y[%d..%d]\n",
               t->ax_min_x, t->ax_max_x, t->ax_min_y, t->ax_max_y);
        printf("        ABS_MT_POSITION_X/Y: X[%d..%d] Y[%d..%d]\n",
               t->mt_min_x, t->mt_max_x, t->mt_min_y, t->mt_max_y);
        printf("        当前采用 ABS_X/Y 那套；若坐标不对请用 --touch-calib 手动指定\n");
    }
    if (t->range_overridden)
    {
        V("[TOUCH] 量程由 --touch-calib 指定\n");
    }

    if (t->swap_xy || t->mirror_x || t->mirror_y)
    {
        V("[TOUCH] 坐标修正: %s%s%s\n",
               t->swap_xy  ? "交换XY " : "",
               t->mirror_x ? "X镜像 "  : "",
               t->mirror_y ? "Y镜像"   : "");
    }
    if (t->dbg)
    {
        V("[TOUCH] 调试模式已开启：只打印原始事件与映射结果，\n"
               "        屏幕上不画任何调试图形（正式界面保持干净）\n");
    }
}

/******************* (C) COPYRIGHT 2025 CAN Monitor *****END OF FILE****/
