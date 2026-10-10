/**
 * @file    can_layer.c
 * @brief   CAN 硬件抽象层实现（SocketCAN + rtnetlink，零第三方依赖）
 *
 * 关键实现说明
 * ------------
 * 1. 【硬件滤波】
 *    setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, n * sizeof(struct can_filter))
 *    内核在 raw 层对每帧做掩码比较，未命中的帧不会唤醒用户态进程。
 *    本工程使用「精确匹配 + EFF 标志」的方式，mask = CAN_EFF_FLAG | 0x1FFFFFFF，
 *    即只匹配 29 位 ID 完全相等且必须是扩展帧。
 *
 * 2. 【错误帧监听】
 *    setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof(err_mask))
 *    之后 recvmsg 会返回 can_id 带 CAN_ERR_FLAG 的帧，其 data[] 按 linux/can/error.h
 *    的约定编码：data[1] = 控制器状态位，data[2] = 协议错误类型位，
 *    data[3] = 错误位置，data[6]/data[7] = TX/RX 错误计数器。
 *
 * 3. 【时间戳】
 *    打开 SO_TIMESTAMPNS，用 recvmsg 的辅助数据取内核时间戳，
 *    比用户态 clock_gettime 更接近报文真正到达的时刻。
 *
 * 4. 【rtnetlink 配置】
 *    波特率配置走 RTM_NEWLINK + IFLA_LINKINFO/IFLA_INFO_KIND="can"
 *    /IFLA_INFO_DATA/IFLA_CAN_BITRATE，等价于
 *    `ip link set can0 type can bitrate 250000`，但无需 fork 进程。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "can_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <poll.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/error.h>
#include <linux/can/netlink.h>

extern int g_verbose;   /* 详细启动日志开关，定义在 main.c */

/*==============================================================================
 *  兼容性补丁
 *  ---------------------------------------------------------------------------
 *  错误帧相关的宏（CAN_ERR_CNT / CAN_ERR_PROT_LOC_* / CAN_ERR_CRTL_* …）
 *  已经移到 can_layer.h 里 —— 它们属于 can_err_frame_str() 这个公开接口的
 *  一部分，调用方也会用到。这里只留网络配置相关的两条。
 *============================================================================*/
#ifndef CAN_CTRLMODE_LOOPBACK
#define CAN_CTRLMODE_LOOPBACK   0x1
#endif
#ifndef IFLA_CAN_RESTART_MS
#define IFLA_CAN_RESTART_MS     6   /* Bus-Off 自动重启周期 */
#endif

/*==============================================================================
 *                      错误帧翻译
 *
 *  内核给的是一条「按位编码」的错误帧：can_id 里是错误类别标志，
 *  data[1] 是控制器状态位，data[2] 是协议错误类型，data[3] 是出错位置。
 *  直接在日志里打 0x00000100 之类没人看得懂，这里翻成中文。
 *
 *  实测过的例子（I.MX6ULL 上总线只有自己一个节点时）：
 *      can_id=0x00000004 data[1]=0x20  → 发送错误被动(TEC>127)
 *      can_id=0x00000040               → 总线关闭
 *      can_id=0x00000100               → 控制器已重启
 *  这三个连在一起出现，就是典型的「发出去没人应答 → TEC 涨到 128 →
 *  再涨到 256 触发 Bus-Off → 自动重启 → 再重复」的循环。
 *============================================================================*/

/** 往缓冲里追加一段文字（带分隔符），返回新的写入长度 */
static int errstr_add(char *out, size_t cap, int len, const char *text)
{
    int n;

    if (out == NULL || cap == 0u || len < 0) { return 0; }
    if ((size_t)len >= cap - 1u) { return len; }   /* 缓冲已满，别再写了 */

    n = snprintf(out + len, cap - (size_t)len, "%s%s",
                 (len > 0) ? "; " : "", text);
    if (n < 0) { return len; }
    if ((size_t)(len + n) >= cap) { return (int)cap - 1; }
    return len + n;
}

int can_err_frame_str(uint32_t can_id, const uint8_t *data, char *out, size_t cap)
{
    int len = 0;

    if (out == NULL || cap == 0u) { return 0; }
    out[0] = '\0';

    /* ---------------- 错误类别（can_id 里的标志位） ---------------- */
    if ((can_id & CAN_ERR_TX_TIMEOUT) != 0u)
    {
        len = errstr_add(out, cap, len, "发送超时");
    }
    if ((can_id & CAN_ERR_LOSTARB) != 0u)
    {
        len = errstr_add(out, cap, len, "仲裁丢失");
    }
    if ((can_id & CAN_ERR_CRTL) != 0u)
    {
        /* data[1] = 控制器状态位图 */
        uint8_t st = (data != NULL) ? data[1] : 0u;

        if ((st & CAN_ERR_CRTL_TX_WARNING) != 0u)
        {
            len = errstr_add(out, cap, len, "发送错误警告(TEC>96)");
        }
        if ((st & CAN_ERR_CRTL_RX_WARNING) != 0u)
        {
            len = errstr_add(out, cap, len, "接收错误警告(REC>96)");
        }
        if ((st & CAN_ERR_CRTL_TX_PASSIVE) != 0u)
        {
            len = errstr_add(out, cap, len, "发送错误被动(TEC>127)");
        }
        if ((st & CAN_ERR_CRTL_RX_PASSIVE) != 0u)
        {
            len = errstr_add(out, cap, len, "接收错误被动(REC>127)");
        }
        if ((st & CAN_ERR_CRTL_TX_OVERFLOW) != 0u)
        {
            len = errstr_add(out, cap, len, "发送缓冲溢出");
        }
        if ((st & CAN_ERR_CRTL_RX_OVERFLOW) != 0u)
        {
            len = errstr_add(out, cap, len, "接收缓冲溢出");
        }
        if ((st & CAN_ERR_CRTL_ACTIVE) != 0u)
        {
            len = errstr_add(out, cap, len, "控制器已回到错误主动(恢复正常)");
        }
        if (st == 0u)
        {
            len = errstr_add(out, cap, len, "控制器状态变化");
        }
    }
    if ((can_id & CAN_ERR_PROT) != 0u)
    {
        /* data[2] = 协议错误类型 */
        uint8_t ty = (data != NULL) ? data[2] : 0u;

        if ((ty & CAN_ERR_PROT_BIT) != 0u)
        {
            len = errstr_add(out, cap, len, "位错误");
        }
        if ((ty & CAN_ERR_PROT_FORM) != 0u)
        {
            len = errstr_add(out, cap, len, "格式错误");
        }
        if ((ty & CAN_ERR_PROT_STUFF) != 0u)
        {
            len = errstr_add(out, cap, len, "位填充错误");
        }
        if ((ty & CAN_ERR_PROT_BIT0) != 0u)
        {
            len = errstr_add(out, cap, len, "显性位错误");
        }
        if ((ty & CAN_ERR_PROT_BIT1) != 0u)
        {
            len = errstr_add(out, cap, len, "隐性位错误");
        }
        if ((ty & CAN_ERR_PROT_OVERLOAD) != 0u)
        {
            len = errstr_add(out, cap, len, "过载帧");
        }
        if ((ty & CAN_ERR_PROT_ACTIVE) != 0u)
        {
            len = errstr_add(out, cap, len, "错误主动错误标志");
        }
        if ((ty & CAN_ERR_PROT_TX) != 0u)
        {
            len = errstr_add(out, cap, len, "（发送方向）");
        }

        /* data[3] = 出错位置：能精确定位到 ACK 位基本就说明「没人应答」 */
        if (data != NULL)
        {
            switch (data[3])
            {
                case CAN_ERR_PROT_LOC_ACK:
                    len = errstr_add(out, cap, len, "★ACK 位无应答（总线上没有其它节点在收）");
                    break;
                case CAN_ERR_PROT_LOC_ACK_DEL:
                    len = errstr_add(out, cap, len, "★ACK 界定符错误（同上，无人应答）");
                    break;
                case CAN_ERR_PROT_LOC_CRC_SEQ:
                    len = errstr_add(out, cap, len, "CRC 序列错误（多半是波特率不一致）");
                    break;
                case CAN_ERR_PROT_LOC_CRC_DEL:
                    len = errstr_add(out, cap, len, "CRC 界定符错误");
                    break;
                case CAN_ERR_PROT_LOC_SOF:
                    len = errstr_add(out, cap, len, "帧起始位错误");
                    break;
                default:
                    break;
            }
        }
    }
    if ((can_id & CAN_ERR_TRX) != 0u)
    {
        len = errstr_add(out, cap, len, "收发器故障（检查 CANH/CANL 接线与终端电阻）");
    }
    if ((can_id & CAN_ERR_ACK) != 0u)
    {
        len = errstr_add(out, cap, len, "★收到 ACK 错误（本机发出去了但没人应答）");
    }
    if ((can_id & CAN_ERR_BUSOFF) != 0u)
    {
        len = errstr_add(out, cap, len, "总线关闭(Bus-Off)");
    }
    if ((can_id & CAN_ERR_BUSERROR) != 0u)
    {
        len = errstr_add(out, cap, len, "总线错误");
    }
    if ((can_id & CAN_ERR_RESTARTED) != 0u)
    {
        len = errstr_add(out, cap, len, "控制器已自动重启");
    }

    /* ---------------- 错误计数器 ---------------- */
    if ((can_id & CAN_ERR_CNT) != 0u && data != NULL)
    {
        char tmp[40];
        snprintf(tmp, sizeof(tmp), "TEC=%u REC=%u",
                 (unsigned)data[6], (unsigned)data[7]);
        len = errstr_add(out, cap, len, tmp);
    }

    if (len == 0)
    {
        len = errstr_add(out, cap, 0, "（未识别的错误类型）");
    }

    return len;
}

int can_err_is_bus_off(uint32_t can_id)
{
    return ((can_id & (CAN_ERR_BUSOFF | CAN_ERR_BUSERROR |
                       CAN_ERR_RESTARTED)) != 0u) ? 1 : 0;
}

/*==============================================================================
 *                       rtnetlink 辅助（移植自 iproute2 的极简版本）
 *============================================================================*/

/** 追加一个普通 rta 属性 */
static int rta_add_l(struct nlmsghdr *n, size_t maxlen, int type,
                     const void *data, size_t alen)
{
    size_t len = RTA_LENGTH(alen);
    struct rtattr *rta;

    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen)
    {
        return -1;
    }

    rta = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len  = (unsigned short)len;
    if (alen)
    {
        memcpy(RTA_DATA(rta), data, alen);
    }
    n->nlmsg_len = (unsigned int)(NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len));
    return 0;
}

/** 开始一个嵌套属性，返回占位指针 */
static struct rtattr *rta_nest_start(struct nlmsghdr *n, size_t maxlen, int type)
{
    struct rtattr *nest = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
    size_t len = RTA_LENGTH(0);

    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > maxlen)
    {
        return NULL;
    }
    nest->rta_type = (unsigned short)type;
    nest->rta_len  = (unsigned short)len;
    n->nlmsg_len   = (unsigned int)(NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len));
    return nest;
}

/** 结束嵌套属性，回填长度 */
static void rta_nest_end(struct nlmsghdr *n, struct rtattr *nest)
{
    nest->rta_len = (unsigned short)((char *)n + n->nlmsg_len - (char *)nest);
}

/**
 * @brief  发送 rtnetlink 请求并等待 ACK
 * @return 0 成功；负值为 errno
 */
static int rtnl_talk(struct nlmsghdr *n)
{
    struct sockaddr_nl nladdr;
    struct iovec iov;
    struct msghdr msg;
    char buf[4096];
    struct nlmsghdr *h;
    int fd, ret = 0;
    ssize_t len;

    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        return -errno;
    }

    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;

    n->nlmsg_seq  = 1;
    n->nlmsg_flags |= NLM_F_ACK;
    n->nlmsg_pid  = (unsigned int)getpid();

    iov.iov_base = n;
    iov.iov_len  = n->nlmsg_len;

    memset(&msg, 0, sizeof(msg));
    msg.msg_name    = &nladdr;
    msg.msg_namelen = sizeof(nladdr);
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;

    if (sendmsg(fd, &msg, 0) < 0)
    {
        ret = -errno;
        close(fd);
        return ret;
    }

    for (;;)
    {
        iov.iov_base = buf;
        iov.iov_len  = sizeof(buf);
        len = recvmsg(fd, &msg, 0);
        if (len < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            ret = -errno;
            break;
        }

        for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned int)len); h = NLMSG_NEXT(h, len))
        {
            if (h->nlmsg_type == NLMSG_ERROR)
            {
                struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(h);
                ret = (err->error == 0) ? 0 : err->error;   /* 内核返回负 errno */
                goto done;
            }
        }
    }

done:
    close(fd);
    return ret;
}

/*==============================================================================
 *                       rtnetlink 接口配置
 *============================================================================*/

int can_set_bitrate(const char *ifname, uint32_t bitrate)
{
    struct
    {
        struct nlmsghdr  n;
        struct ifinfomsg ifi;
        char             buf[512];
    } req;
    struct rtattr *linkinfo, *infodata;
    struct can_bittiming bt;
    unsigned int ifindex;
    int ret;

    if (ifname == NULL || bitrate == 0)
    {
        return -EINVAL;
    }

    ifindex = if_nametoindex(ifname);
    if (ifindex == 0)
    {
        return -ENODEV;
    }

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_type  = RTM_NEWLINK;
    req.n.nlmsg_flags = NLM_F_REQUEST;
    req.ifi.ifi_family = AF_UNSPEC;
    req.ifi.ifi_index  = (int)ifindex;

    /* IFLA_LINKINFO { IFLA_INFO_KIND="can", IFLA_INFO_DATA { IFLA_CAN_BITTIMING } }
     *
     * 注意：内核 UAPI 中**没有** IFLA_CAN_BITRATE 这个属性，
     *      波特率是通过 IFLA_CAN_BITTIMING 下发 struct can_bittiming 的
     *      bitrate 字段完成的（等价于 `ip link set can0 type can bitrate N`）。
     *      其余字段（tq/phase_seg1/...）填 0 表示由内核自行计算。 */
    linkinfo = rta_nest_start(&req.n, sizeof(req), IFLA_LINKINFO);
    if (linkinfo == NULL)
    {
        return -EMSGSIZE;
    }

    rta_add_l(&req.n, sizeof(req), IFLA_INFO_KIND, "can", sizeof("can"));

    infodata = rta_nest_start(&req.n, sizeof(req), IFLA_INFO_DATA);
    if (infodata == NULL)
    {
        return -EMSGSIZE;
    }

    memset(&bt, 0, sizeof(bt));
    bt.bitrate = bitrate;
    rta_add_l(&req.n, sizeof(req), IFLA_CAN_BITTIMING, &bt, sizeof(bt));

    /* 同时打开「Bus-Off 自动恢复」：内核会在总线关闭后每 restart_ms
     * 毫秒自动重启控制器。等价于
     *     ip link set can0 type can restart-ms 100
     * 联调时非常有用 —— 否则一旦因为没有 ACK 进入 Bus-Off，
     * 接口就会一直停在 BUS_OFF，必须手工 down/up 才能恢复，
     * 现场看到的就是「程序在发但总线上什么都没有」。 */
    {
        __u32 restart_ms = 100u;
        rta_add_l(&req.n, sizeof(req), IFLA_CAN_RESTART_MS,
                  &restart_ms, sizeof(restart_ms));
    }

    rta_nest_end(&req.n, infodata);
    rta_nest_end(&req.n, linkinfo);

    ret = rtnl_talk(&req.n);

    /* EBUSY：接口处于 UP 状态时，内核/FlexCAN 驱动不允许修改位定时。
     * 这正是 `ip link set can0 type can bitrate N` 在接口已经启动时
     * 会报 "Device or resource busy" 的原因。这里自动完成
     * 「down -> 配置 -> up」，等价于用户手工执行的三条命令，
     * 避免出现「程序启动时接口已经是 up，于是波特率根本没被改到」这种坑。 */
    if (ret == -EBUSY)
    {
        fprintf(stdout, "[CAN] %s 当前处于 UP 状态，先 down 再配置位定时 ...\n", ifname);
        (void)can_set_link_up(ifname, 0);
        usleep(200000);              /* 给驱动一点时间真正停下来 */
        ret = rtnl_talk(&req.n);     /* 同一个请求重发即可 */
        (void)can_set_link_up(ifname, 1);
        if (ret == 0)
        {
            fprintf(stdout, "[CAN] 位定时配置成功: bitrate=%u, restart-ms=100, 接口已重新 UP\n",
                    bitrate);
            return 0;
        }
    }

    if (ret < 0)
    {
        fprintf(stderr,
                "[CAN] 在线配置 %s 波特率 %u 失败(errno=%d: %s)。\n"
                "      请手工执行: sudo ip link set %s down\n"
                "                  sudo ip link set %s type can bitrate %u restart-ms 100\n"
                "                  sudo ip link set %s up\n",
                ifname, bitrate, -ret, strerror(-ret),
                ifname, ifname, bitrate, ifname);
    }
    return ret;
}

int can_set_link_up(const char *ifname, int up)
{
    struct
    {
        struct nlmsghdr  n;
        struct ifinfomsg ifi;
    } req;
    unsigned int ifindex;
    int ret;

    if (ifname == NULL)
    {
        return -EINVAL;
    }

    ifindex = if_nametoindex(ifname);
    if (ifindex == 0)
    {
        return -ENODEV;
    }

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_type  = RTM_NEWLINK;
    req.n.nlmsg_flags = NLM_F_REQUEST;
    req.ifi.ifi_family = AF_UNSPEC;
    req.ifi.ifi_index  = (int)ifindex;
    req.ifi.ifi_change = IFF_UP;
    req.ifi.ifi_flags  = up ? IFF_UP : 0;

    ret = rtnl_talk(&req.n);
    if (ret < 0)
    {
        fprintf(stderr, "[CAN] %s 接口 %s 失败(errno=%d: %s)\n",
                up ? "启动" : "关闭", ifname, -ret, strerror(-ret));
    }
    return ret;
}

int can_set_loopback(const char *ifname, int on)
{
    struct
    {
        struct nlmsghdr  n;
        struct ifinfomsg ifi;
        char             buf[512];
    } req;
    struct rtattr *linkinfo, *infodata;
    struct can_ctrlmode cm;
    unsigned int ifindex;
    int ret;

    if (ifname == NULL)
    {
        return -EINVAL;
    }
    ifindex = if_nametoindex(ifname);
    if (ifindex == 0)
    {
        return -ENODEV;
    }

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len    = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_type   = RTM_NEWLINK;
    req.n.nlmsg_flags  = NLM_F_REQUEST;
    req.ifi.ifi_family = AF_UNSPEC;
    req.ifi.ifi_index  = (int)ifindex;

    linkinfo = rta_nest_start(&req.n, sizeof(req), IFLA_LINKINFO);
    if (linkinfo == NULL)
    {
        return -EMSGSIZE;
    }
    rta_add_l(&req.n, sizeof(req), IFLA_INFO_KIND, "can", sizeof("can"));

    infodata = rta_nest_start(&req.n, sizeof(req), IFLA_INFO_DATA);
    if (infodata == NULL)
    {
        return -EMSGSIZE;
    }

    /* 回环不是独立属性，而是控制模式 CAN_CTRLMODE_LOOPBACK 的一个位：
     *   struct can_ctrlmode { __u32 mask; __u32 flags; }
     *   mask   = 本次要修改哪些位
     *   flags  = 这些位的新取值
     * 等价于 `ip link set can0 type can loopback on|off`。 */
    memset(&cm, 0, sizeof(cm));
    cm.mask  = CAN_CTRLMODE_LOOPBACK;
    cm.flags = on ? CAN_CTRLMODE_LOOPBACK : 0u;
    rta_add_l(&req.n, sizeof(req), IFLA_CAN_CTRLMODE, &cm, sizeof(cm));

    rta_nest_end(&req.n, infodata);
    rta_nest_end(&req.n, linkinfo);

    ret = rtnl_talk(&req.n);
    return ret;
}

int can_query_link(const char *ifname, char *info, size_t info_len)
{
    char path[128];
    char buf[64];
    FILE *fp;
    unsigned int ifindex;
    int flags;

    if (ifname == NULL || info == NULL || info_len == 0)
    {
        return -EINVAL;
    }

    ifindex = if_nametoindex(ifname);
    if (ifindex == 0)
    {
        return -ENODEV;
    }

    /* 读取 sysfs 中的 flags 判断 UP/DOWN（无需 ioctl，避免额外头文件依赖） */
    snprintf(path, sizeof(path), "/sys/class/net/%s/flags", ifname);
    flags = 0;
    fp = fopen(path, "r");
    if (fp != NULL)
    {
        if (fgets(buf, sizeof(buf), fp) != NULL)
        {
            flags = (int)strtol(buf, NULL, 16);
        }
        fclose(fp);
    }

    snprintf(info, info_len, "%s: ifindex=%u state=%s",
             ifname, ifindex, (flags & IFF_UP) ? "UP" : "DOWN");
    return 0;
}

/*==============================================================================
 *                       sysfs 统计
 *============================================================================*/

/** 读取 sysfs 中的单个计数文件 */
static uint64_t read_sysfs_counter(const char *ifname, const char *name)
{
    char path[160];
    char buf[64];
    FILE *fp;
    uint64_t v = 0;

    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", ifname, name);
    fp = fopen(path, "r");
    if (fp == NULL)
    {
        return 0;
    }
    if (fgets(buf, sizeof(buf), fp) != NULL)
    {
        v = strtoull(buf, NULL, 10);
    }
    fclose(fp);
    return v;
}

volatile int g_can_stat_enable = 1;     /* 1 = 统计收发帧数（见 can_layer.h 说明） */

void can_layer_refresh_sysfs_stats(can_layer_t *cl)
{
    if (cl == NULL || !cl->opened)
    {
        return;
    }

    cl->stats.ctrl_restarts  = read_sysfs_counter(cl->ifname, "restarts");
    cl->stats.bus_errors_sys = read_sysfs_counter(cl->ifname, "bus_error");
}

const char *can_state_str(can_bus_state_t st)
{
    switch (st)
    {
        case CANBUS_STATE_ERROR_ACTIVE:  return "ERROR_ACTIVE";
        case CANBUS_STATE_ERROR_WARNING: return "ERROR_WARNING";
        case CANBUS_STATE_ERROR_PASSIVE: return "ERROR_PASSIVE";
        case CANBUS_STATE_BUS_OFF:       return "BUS_OFF";
        case CANBUS_STATE_STOPPED:       return "STOPPED";
        case CANBUS_STATE_SLEEPING:      return "SLEEPING";
        default:                      return "UNKNOWN";
    }
}

/*==============================================================================
 *                       生命周期
 *============================================================================*/

int can_layer_init(can_layer_t *cl, const char *ifname)
{
    struct sockaddr_can addr;
    int sock;
    int ts_on = 1;

    if (cl == NULL)
    {
        return -EINVAL;
    }
    memset(cl, 0, sizeof(*cl));
    cl->fd = -1;

    snprintf(cl->ifname, sizeof(cl->ifname), "%s",
             (ifname != NULL) ? ifname : CAN_DEFAULT_IFNAME);

    /* 1. 检查接口是否存在 */
    cl->ifindex = (int)if_nametoindex(cl->ifname);
    if (cl->ifindex == 0)
    {
        fprintf(stderr, "[CAN] 接口 %s 不存在。请先执行:\n"
                        "      sudo ip link set can0 type can bitrate 250000\n"
                        "      sudo ip link set can0 up\n", cl->ifname);
        return -1001;
    }

    /* 2. 创建 SocketCAN 原始套接字 */
    sock = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    if (sock < 0)
    {
        fprintf(stderr, "[CAN] socket(PF_CAN) 失败: %s\n", strerror(errno));
        return -errno;
    }

    /* 3. 关闭「回环自发自收」，避免与 STM32 联调时收到自己发的报文
     *    （如需单板自测，可用 --loopback 参数打开） */
    {
        int loop = 0;
        (void)setsockopt(sock, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &loop, sizeof(loop));
    }

    /* 4. 打开纳秒级接收时间戳 */
    if (setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPNS, &ts_on, sizeof(ts_on)) < 0)
    {
        fprintf(stderr, "[CAN] 警告: SO_TIMESTAMPNS 设置失败，将退化为用户态计时 (%s)\n",
                strerror(errno));
    }

    /* 5. bind 到 canX */
    memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = cl->ifindex;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "[CAN] bind(%s) 失败: %s\n", cl->ifname, strerror(errno));
        close(sock);
        return -errno;
    }

    /* 6. 记录并校验接口基本信息 */
    cl->fd      = sock;
    cl->bitrate = CAN_BITRATE_GB27930;
    cl->opened  = 1;
    cl->stats.state = CANBUS_STATE_ERROR_ACTIVE;

    return 0;
}

void can_layer_deinit(can_layer_t *cl)
{
    if (cl == NULL)
    {
        return;
    }
    if (cl->fd >= 0)
    {
        close(cl->fd);
        cl->fd = -1;
    }
    cl->opened = 0;
}

/*==============================================================================
 *                       滤波与错误帧
 *============================================================================*/

int can_apply_filter_ids(can_layer_t *cl, const uint32_t *ids, size_t num)
{
    struct can_filter *filters;
    size_t i;

    if (cl == NULL || !cl->opened || ids == NULL || num == 0)
    {
        return -EINVAL;
    }
    if (num > 512)
    {
        num = 512;   /* SocketCAN 原始套接字滤波器上限 */
    }

    filters = (struct can_filter *)calloc(num, sizeof(struct can_filter));
    if (filters == NULL)
    {
        return -ENOMEM;
    }

    for (i = 0; i < num; i++)
    {
        /* 精确匹配：can_id 必须完全相同，且必须是扩展帧（CAN_EFF_FLAG） */
        filters[i].can_id   = (ids[i] & CAN_EFF_MASK) | CAN_EFF_FLAG;
        filters[i].can_mask = CAN_EFF_FLAG | CAN_EFF_MASK;   /* 0x9FFFFFFF */
    }

    if (setsockopt(cl->fd, SOL_CAN_RAW, CAN_RAW_FILTER,
                   filters, (socklen_t)(num * sizeof(struct can_filter))) < 0)
    {
        int e = errno;
        fprintf(stderr, "[CAN] 下发滤波器失败: %s\n", strerror(e));
        free(filters);
        return -e;
    }

    fprintf(stdout, "[CAN] 硬件滤波已启用，放行 %zu 条扩展帧 ID\n", num);
    free(filters);
    return 0;
}

int can_apply_filter_all(can_layer_t *cl)
{
    struct can_filter f;

    if (cl == NULL || !cl->opened)
    {
        return -EINVAL;
    }

    /* can_id = 0, can_mask = 0 -> 内核认为不过滤任何帧 */
    f.can_id   = 0;
    f.can_mask = 0;

    if (setsockopt(cl->fd, SOL_CAN_RAW, CAN_RAW_FILTER, &f, sizeof(f)) < 0)
    {
        return -errno;
    }
    fprintf(stdout, "[CAN] 已关闭硬件滤波，接收全部报文（调试模式）\n");
    return 0;
}

int can_enable_error_frames(can_layer_t *cl)
{
    can_err_mask_t err_mask = CAN_ERR_MASK;   /* 订阅全部错误类型 */

    if (cl == NULL || !cl->opened)
    {
        return -EINVAL;
    }

    if (setsockopt(cl->fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
                   &err_mask, sizeof(err_mask)) < 0)
    {
        fprintf(stderr, "[CAN] 无法订阅错误帧: %s\n", strerror(errno));
        return -errno;
    }

    if (g_verbose) fprintf(stdout, "[CAN] 错误帧监听已开启（位错误/填充错误/格式错误/ACK/CRC）\n");
    return 0;
}

/*==============================================================================
 *                       收 / 发
 *============================================================================*/

int can_layer_send(can_layer_t *cl, uint32_t ext_id, const uint8_t *data, uint8_t dlc)
{
    struct can_frame frame;
    ssize_t n;

    if (cl == NULL || !cl->opened)
    {
        return -EINVAL;
    }
    if (dlc > 8 || (dlc > 0 && data == NULL))
    {
        return -1;
    }
    if (ext_id > CAN_EFF_MASK)
    {
        return -1;
    }

    memset(&frame, 0, sizeof(frame));
    frame.can_id  = (ext_id & CAN_EFF_MASK) | CAN_EFF_FLAG;   /* 扩展帧标志 */
    frame.can_dlc = dlc;
    if (dlc > 0)
    {
        memcpy(frame.data, data, dlc);
    }

    n = write(cl->fd, &frame, sizeof(frame));
    if (n != (ssize_t)sizeof(frame))
    {
        cl->stats.tx_errors++;
        if (errno == EAGAIN || errno == ENOBUFS)
        {
            return -2;   /* 发送队列满，调用者可按需重试 */
        }
        return -errno;
    }

    if (g_can_stat_enable)
    {
        cl->stats.tx_frames++;
    }
    return 0;
}

int can_layer_recv(can_layer_t *cl, can_item_t *item, int timeout_ms)
{
    struct can_frame frame;
    struct iovec iov;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    char ctrl[CMSG_SPACE(sizeof(struct timespec))];
    struct timespec tv_now;
    ssize_t n;

    if (cl == NULL || !cl->opened || item == NULL)
    {
        return -EINVAL;
    }

    /* 1. 超时控制：使用 poll 实现「非阻塞 / 有限等待 / 永久阻塞」三种语义 */
    if (timeout_ms >= 0)
    {
        struct pollfd pfd;
        int pr;

        pfd.fd     = cl->fd;
        pfd.events = POLLIN;
        do
        {
            pr = poll(&pfd, 1, timeout_ms);
        } while (pr < 0 && errno == EINTR);

        if (pr == 0)
        {
            return 0;   /* 超时 */
        }
        if (pr < 0)
        {
            return -errno;
        }
    }

    /* 2. recvmsg 以便拿到辅助数据（内核时间戳） */
    memset(&frame, 0, sizeof(frame));
    memset(&msg, 0, sizeof(msg));
    memset(ctrl, 0, sizeof(ctrl));

    iov.iov_base = &frame;
    iov.iov_len  = sizeof(frame);
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctrl;
    msg.msg_controllen = sizeof(ctrl);

    do
    {
        n = recvmsg(cl->fd, &msg, 0);
    } while (n < 0 && errno == EINTR);

    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 0;
        }
        return -errno;
    }
    if (n < (ssize_t)sizeof(struct can_frame))
    {
        return 0;   /* 短读，忽略 */
    }

    /* 3. 解析时间戳 */
    clock_gettime(CLOCK_REALTIME, &tv_now);
    item->ts = tv_now;
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg))
    {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPNS)
        {
            memcpy(&item->ts, CMSG_DATA(cmsg), sizeof(struct timespec));
            break;
        }
    }

    memset(item->data, 0, sizeof(item->data));

    /* 4. 错误帧分支：分类统计后仍返回给上层（is_error = 1） */
    if ((frame.can_id & CAN_ERR_FLAG) != 0)
    {
        cl->stats.err_frames++;

        /* data[1] = 控制器状态位 */
        if (frame.data[1] & CAN_ERR_CRTL_TX_WARNING)  { cl->stats.err_warning++; }
        if (frame.data[1] & CAN_ERR_CRTL_RX_WARNING)  { cl->stats.err_warning++; }
        if ((frame.data[1] & (CAN_ERR_CRTL_TX_PASSIVE | CAN_ERR_CRTL_RX_PASSIVE)) != 0)
        {
            cl->stats.err_passive++;
        }
        if ((frame.can_id & CAN_ERR_BUSOFF) != 0)
        {
            cl->stats.bus_off++;
            cl->stats.state = CANBUS_STATE_BUS_OFF;
        }
        else if ((frame.can_id & CAN_ERR_RESTARTED) != 0)
        {
            cl->stats.state = CANBUS_STATE_ERROR_ACTIVE;
        }

        /* data[2] = 协议错误类型（PROT） */
        if (frame.data[2] & CAN_ERR_PROT_BIT)   { cl->stats.err_bit++;   }
        if (frame.data[2] & CAN_ERR_PROT_FORM)  { cl->stats.err_form++;  }
        if (frame.data[2] & CAN_ERR_PROT_STUFF) { cl->stats.err_stuff++; }

        /* data[3] = 错误位置：CRC 序列 / ACK 槽 */
        if (frame.data[3] == CAN_ERR_PROT_LOC_CRC_SEQ ||
            frame.data[3] == CAN_ERR_PROT_LOC_CRC_DEL)
        {
            cl->stats.err_crc++;
        }
        if (frame.data[3] == CAN_ERR_PROT_LOC_ACK ||
            frame.data[3] == CAN_ERR_PROT_LOC_ACK_DEL)
        {
            cl->stats.err_ack++;
        }

        /* 未归类的其它错误 */
        if (((frame.data[2] & (CAN_ERR_PROT_BIT | CAN_ERR_PROT_FORM | CAN_ERR_PROT_STUFF)) == 0) &&
            (frame.data[3] != CAN_ERR_PROT_LOC_CRC_SEQ) &&
            (frame.data[3] != CAN_ERR_PROT_LOC_CRC_DEL) &&
            (frame.data[3] != CAN_ERR_PROT_LOC_ACK) &&
            (frame.data[3] != CAN_ERR_PROT_LOC_ACK_DEL))
        {
            cl->stats.err_other++;
        }

        /* data[6]/data[7] = TX/RX 错误计数器 */
        if ((frame.can_id & CAN_ERR_CNT) != 0)
        {
            cl->stats.txerr = frame.data[6];
            cl->stats.rxerr = frame.data[7];
        }

        item->can_id      = frame.can_id & ~CAN_ERR_FLAG;
        item->dlc         = 8;
        memcpy(item->data, frame.data, 8);
        item->is_error    = 1;
        item->is_extended = 1;
        return -1;
    }

    /* 5. 正常数据帧 */
    item->can_id      = frame.can_id & CAN_EFF_MASK;
    item->dlc         = frame.can_dlc & 0x0F;
    if (item->dlc > 8)
    {
        item->dlc = 8;
    }
    memcpy(item->data, frame.data, item->dlc);
    item->is_error    = 0;
    item->is_extended = ((frame.can_id & CAN_EFF_FLAG) != 0) ? 1 : 0;

    if (g_can_stat_enable)
    {
        cl->stats.rx_frames++;
    }
    return 1;
}

/******************* (C) COPYRIGHT 2025 CAN Monitor *****END OF FILE****/
