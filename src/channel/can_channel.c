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
#include <linux/can/error.h>

#include "log/log.h"

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

/*
 * 一帧占用的总线位数估算。
 *
 * 标准帧基础位数 = 47 + 8*dlc（含 SOF、仲裁场、CRC、EOF 与 3 位帧间隔）。
 * 位填充按 1/6 估算（最坏情况约 1/5，取 1/6 偏保守）。
 * 这是估算值，用于负载率趋势判断，不是精确测量。
 */
static uint32_t frame_bits(uint8_t dlc)
{
    uint32_t base = 47 + 8u * dlc;

    return base + base / 6u;
}

static void account_bits(can_channel_t *ch, uint8_t dlc)
{
    ch->bits_accum += frame_bits(dlc);
}

int can_channel_open_ex(can_channel_t *ch, const char *ifname, uint32_t bitrate)
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

    /*
     * 订阅错误帧。不订阅的话，总线出问题网关只会「感觉报文变少了」，
     * 完全看不到原因。错误帧里带着错误计数，是排障的第一手信息。
     */
    {
        can_err_mask_t err_mask = CAN_ERR_MASK;
        setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof(err_mask));
    }

    ch->fd = fd;
    ch->opened = 1;
    ch->bitrate = (bitrate != 0) ? bitrate : 500000u;
    ch->load_window_ms = now_us() / 1000ull;
    log_info("channel %s opened (fd=%d, %u bps)", ifname, fd, ch->bitrate);
    return 0;
}

int can_channel_open(can_channel_t *ch, const char *ifname)
{
    return can_channel_open_ex(ch, ifname, 0);
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

/*
 * 哪些 errno 意味着 socket 已经废了。
 *
 *   ENETDOWN  接口被 down 掉（或网线拔了）—— socket 还活着但收不到东西
 *   ENODEV    接口被删掉、驱动卸载 —— socket 对应不到设备了
 *   EBADF     fd 已经失效（被别处关掉）
 *   EPERM     权限被回收（例如容器里改了设备策略）
 *
 * 反例要特别留意 ENOBUFS：那是发送侧缓冲满，属于「太快了」，
 * 不是「链路断了」。把它当致命错误会触发一次毫无必要的重开，
 * 而重开期间这路通道是真的收不到帧。
 */
int can_channel_error_fatal(int err)
{
    switch (err)
    {
    case ENETDOWN:
    case ENODEV:
    case EBADF:
    case EPERM:
        return 1;
    default:
        return 0;
    }
}

void can_channel_note_error(can_channel_t *ch, int err)
{
    if (ch == 0)
        return;

    ch->last_errno = err;
    if (can_channel_error_fatal(err))
        ch->need_reopen = 1;
}

int can_channel_needs_reopen(const can_channel_t *ch)
{
    return (ch != 0 && ch->need_reopen) ? 1 : 0;
}

int can_channel_reopen(can_channel_t *ch)
{
    can_channel_t saved;
    int rc;

    if (ch == 0)
        return -1;

    /*
     * 只在内存里保留需要延续的字段。can_channel_open_ex 会清零整个结构，
     * 直接调用会把累计统计一起抹掉 —— 而重开恰恰发生在故障之后，
     * 那正是最不该丢统计的时刻。
     */
    saved = *ch;
    can_channel_close(ch);

    rc = can_channel_open_ex(ch, saved.name, saved.bitrate);

    /* 名字与统计恢复（open 成功时它只设了 name/fd/opened/bitrate） */
    strncpy(ch->name, saved.name, sizeof(ch->name) - 1);
    ch->rx_frames = saved.rx_frames;
    ch->tx_frames = saved.tx_frames;
    ch->rx_errors = saved.rx_errors;
    ch->tx_errors = saved.tx_errors;
    ch->err_frames = saved.err_frames;
    ch->err_bus = saved.err_bus;
    ch->err_ctrl = saved.err_ctrl;
    ch->bits_accum = saved.bits_accum;
    ch->load_window_ms = saved.load_window_ms;
    ch->load_permille = saved.load_permille;

    if (rc == 0)
    {
        ch->reopen_count = saved.reopen_count + 1;
        ch->reopen_fail = saved.reopen_fail;
        ch->need_reopen = 0;
        ch->last_errno = 0;
        log_info("channel %s reopened (total %u)", ch->name, ch->reopen_count);
        return 0;
    }

    /* 失败：保持 need_reopen，让调用方按退避继续重试 */
    ch->fd = -1;
    ch->opened = 0;
    ch->reopen_count = saved.reopen_count;
    ch->reopen_fail = saved.reopen_fail + 1;
    ch->need_reopen = 1;
    ch->last_errno = saved.last_errno;
    return -1;
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
         *
         * 但要把 errno 分类：ENOBUFS 是「太快了」，ENETDOWN/ENODEV 是
         * 「链路没了」，后者必须让引擎去重开。
         */
        ch->tx_errors++;
        can_channel_note_error(ch, errno);
        return -1;
    }

    ch->tx_frames++;
    account_bits(ch, cf.can_dlc);
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
        can_channel_note_error(ch, errno);
        return -2;
    }
    if (n != (ssize_t)sizeof(cf))
        return -1;

    /* 错误帧：不当作数据帧向上传，只统计 */
    if (cf.can_id & CAN_ERR_FLAG)
    {
        ch->err_frames++;
        if (cf.can_id & CAN_ERR_BUSERROR)
            ch->err_bus++;
        if (cf.can_id & CAN_ERR_CRTL)
            ch->err_ctrl++;
        return -1;
    }

    memset(f, 0, sizeof(*f));
    f->id = cf.can_id & CAN_SFF_MASK;
    f->dlc = cf.can_dlc;
    memcpy(f->data, cf.data, (cf.can_dlc > GW_FRAME_MAX_DLC) ? GW_FRAME_MAX_DLC : cf.can_dlc);
    f->timestamp_us = now_us();
    f->src_channel = channel_index;

    ch->rx_frames++;
    account_bits(ch, cf.can_dlc);
    return 0;
}

uint32_t can_channel_update_load(can_channel_t *ch, uint64_t now_ms)
{
    uint64_t elapsed;
    uint64_t capacity_bits;

    if (ch == 0 || !ch->opened || ch->bitrate == 0)
        return 0;

    if (ch->load_window_ms == 0)
    {
        ch->load_window_ms = now_ms;
        return 0;
    }

    elapsed = now_ms - ch->load_window_ms;
    if (elapsed == 0)
        return ch->load_permille;

    /* 窗口内总线理论容量 = 波特率 × 秒数 */
    capacity_bits = (uint64_t)ch->bitrate * elapsed / 1000ull;
    if (capacity_bits > 0)
        ch->load_permille = (uint32_t)(ch->bits_accum * 1000ull / capacity_bits);

    ch->bits_accum = 0;
    ch->load_window_ms = now_ms;
    return ch->load_permille;
}

int can_channel_fd(const can_channel_t *ch)
{
    return (ch != 0 && ch->opened) ? ch->fd : -1;
}
