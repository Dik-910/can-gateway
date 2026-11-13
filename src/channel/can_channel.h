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

typedef struct
{
    char name[GW_CHANNEL_NAME_MAX];
    int fd;
    int opened;

    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t rx_errors;
    uint32_t tx_errors;  /* 发送失败，通常是 socket 缓冲满 */
} can_channel_t;

/* 打开通道。ifname 如 "can0" / "vcan0"。成功返回 0 */
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

#endif
