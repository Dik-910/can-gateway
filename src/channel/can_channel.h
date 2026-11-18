#ifndef __CAN_CHANNEL_H
#define __CAN_CHANNEL_H

#include <stdint.h>

#include "frame_queue.h"

/*
 * CAN 通道：一路 SocketCAN 接口的封装。
 *
 * 与原 IoT 网关的 Device 相比，两点不同：
 *   1. 多实例。原来 Device 是全局单例（`static Device *device`），
 *      车载网关至少要两路 CAN，必须能同时存在多个通道。
 *   2. 暴露 fd。转发引擎用 epoll 同时监听多路，需要拿到文件描述符。
 *
 * 每通道带统计，因为网关最常被问的两个数字是「转了多少」和「丢了多少」。
 */

#define GW_CHANNEL_MAX 8

/* 重开失败后的重试间隔：接口可能还没被重新拉起来 */
#define GW_CHANNEL_RECONNECT_MS 1000

typedef struct
{
    char name[GW_CHANNEL_NAME_MAX];
    int fd;
    int opened;

    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t rx_errors;
    uint32_t tx_errors;  /* 发送失败，通常是 socket 缓冲满 */

    /* 总线错误帧统计（来自 CAN_RAW_ERR_FILTER） */
    uint32_t err_frames; /* 错误帧总数 */
    uint32_t err_bus;    /* 总线错误（位错误、帧格式错等） */
    uint32_t err_ctrl;   /* 控制器错误（含 Bus Off） */

    /*
     * 总线负载率。
     *
     * 按位时间累加计算，不是按帧数估——不同 DLC 的帧占用差异很大，
     * 用帧数估会得到完全失真的数字。
     */
    uint32_t bitrate;        /* 通道波特率，默认 500000 */
    uint64_t bits_accum;     /* 当前窗口累计位数 */
    uint64_t load_window_ms; /* 窗口起点 */
    uint32_t load_permille;  /* 最近一次算出的负载率（千分比） */

    /*
     * 链路健康。
     *
     * 接口被 down 掉、被 delete 掉，或者驱动卸载过，socket 就再也不会
     * 出新帧了 —— read 只会返回错误。这时候进程还在跑、统计也不动，
     * 唯一能看出来的迹象就是「转发数不涨了」。所以要把这类错误单独记下来，
     * 交给引擎去重开，而不是当成一次普通的读失败跳过。
     */
    uint32_t reopen_count;   /* 成功重开次数 */
    uint32_t reopen_fail;    /* 重开失败次数 */
    int last_errno;          /* 最近一次致命错误的 errno，供日志排查 */
    uint8_t need_reopen;     /* 1 表示该通道需要重开 */
} can_channel_t;

/* 打开通道。ifname 如 "can0" / "vcan0"，bitrate 用于负载率计算（0 表示用默认 500000） */
int can_channel_open_ex(can_channel_t *ch, const char *ifname, uint32_t bitrate);
int can_channel_open(can_channel_t *ch, const char *ifname);
void can_channel_close(can_channel_t *ch);

/* 发送一帧。失败返回 -1，tx_errors 递增 */
int can_channel_send(can_channel_t *ch, const gw_frame_t *f);

/*
 * 接收一帧（非阻塞）。读到数据返回 0 并填好 src_channel 与 timestamp_us；
 * 无数据返回 -1（不是错误）；底层出错返回 -2。
 */
int can_channel_recv(can_channel_t *ch, gw_frame_t *f, uint8_t channel_index);

/* 供 epoll 使用 */
int can_channel_fd(const can_channel_t *ch);

/*
 * 错误分类：这个 errno 是否意味着「socket 已经废了，必须重开」。
 *
 * 分开成独立函数是为了能单测 —— 判定写错的表现是「偶发 ENOBUFS 触发重开」，
 * 那会在总线上制造一段没人需要的空窗。
 */
int can_channel_error_fatal(int err);

/* 记录一次错误并更新 need_reopen。send/recv 内部调用，测试也可直接调用 */
void can_channel_note_error(can_channel_t *ch, int err);

int can_channel_needs_reopen(const can_channel_t *ch);

/*
 * 重开。保留名字、波特率与全部统计 —— 重开是因为链路断了，
 * 不是因为这路通道被重新定义，统计归零会让「断之前转了多少」无从查起。
 * 失败时返回 -1，need_reopen 保持为 1，调用方按退避重试。
 */
int can_channel_reopen(can_channel_t *ch);

/*
 * 计算最近 window_ms 内的总线负载率（千分比），并重置窗口。
 * 由统计打印周期调用即可，不必每帧调用。
 */
uint32_t can_channel_update_load(can_channel_t *ch, uint64_t now_ms);

#endif
