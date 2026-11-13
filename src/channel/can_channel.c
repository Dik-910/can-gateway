/*
 * 必须在所有 include 之前：-std=c99 严格 ANSI 模式不暴露 struct ifreq / IFNAMSIZ。
 * 放在源文件第一行而非编译选项，换构建系统也不会失效。
 */
#define _GNU_SOURCE

#include "can_channel.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#include "log/log.h"

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

int can_channel_open(can_channel_t *ch, const char *ifname)
{
    struct sockaddr_can addr;
    struct ifreq ifr;
    int fd;

    if (ch == 0 || ifname == 0)
        return -1;

    memset(ch, 0, sizeof(*ch));
    ch->fd = -1;
    strncpy(ch->name, ifname, sizeof(ch->name) - 1);

    fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0)
    {
        log_error("socket(PF_CAN) failed for %s: %s", ifname, strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
    {
        log_error("interface %s not found: %s", ifname, strerror(errno));
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        log_error("bind %s failed: %s", ifname, strerror(errno));
        close(fd);
        return -1;
    }

    /* 非阻塞：epoll 循环里不能有阻塞调用 */
    {
        int flags = 1;
        ioctl(fd, FIONBIO, &flags);
    }

    ch->fd = fd;
    ch->opened = 1;
    log_info("channel %s opened (fd=%d)", ifname, fd);
    return 0;
}

void can_channel_close(can_channel_t *ch)
{
    if (ch == 0)
        return;

    if (ch->opened && ch->fd >= 0)
    {
        close(ch->fd);
        log_info("channel %s closed", ch->name);
    }

    ch->fd = -1;
    ch->opened = 0;
}

int can_channel_send(can_channel_t *ch, const gw_frame_t *f)
{
    struct can_frame cf;
    ssize_t n;

    if (ch == 0 || f == 0 || !ch->opened)
        return -1;

    memset(&cf, 0, sizeof(cf));
    cf.can_id = f->id & CAN_SFF_MASK;
    cf.can_dlc = (f->dlc > GW_FRAME_MAX_DLC) ? GW_FRAME_MAX_DLC : f->dlc;
    memcpy(cf.data, f->data, cf.can_dlc);

    n = write(ch->fd, &cf, sizeof(cf));
    if (n != (ssize_t)sizeof(cf))
    {
        /*
         * 发送失败不重试。CAN 是广播型总线，socket 缓冲满说明下游处理不过来，
         * 这时候重试只会把延迟越堆越高，丢掉并计数才是正确做法。
         */
        ch->tx_errors++;
        return -1;
    }

    ch->tx_frames++;
    return 0;
}

int can_channel_recv(can_channel_t *ch, gw_frame_t *f, uint8_t channel_index)
{
    struct can_frame cf;
    ssize_t n;

    if (ch == 0 || f == 0 || !ch->opened)
        return -2;

    n = read(ch->fd, &cf, sizeof(cf));
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -1; /* 暂无数据，不是错误 */
        ch->rx_errors++;
        return -2;
    }
    if (n != (ssize_t)sizeof(cf))
        return -1;

    memset(f, 0, sizeof(*f));
    f->id = cf.can_id & CAN_SFF_MASK;
    f->dlc = cf.can_dlc;
    memcpy(f->data, cf.data, (cf.can_dlc > GW_FRAME_MAX_DLC) ? GW_FRAME_MAX_DLC : cf.can_dlc);
    f->timestamp_us = now_us();
    f->src_channel = channel_index;

    ch->rx_frames++;
    return 0;
}

int can_channel_fd(const can_channel_t *ch)
{
    return (ch != 0 && ch->opened) ? ch->fd : -1;
}
