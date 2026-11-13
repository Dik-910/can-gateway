#ifndef __FRAME_QUEUE_H
#define __FRAME_QUEUE_H

#include <stdint.h>

/*
 * 定长 CAN 帧队列（环形）。
 *
 * 为什么不用原 IoT 网关那套双缓冲：
 *   双缓冲适合「长度不定的数据流」，每条消息带 1 字节长度前缀，读取时要 memmove 搬移剩余数据。
 *   但 CAN 帧是定长结构，环形队列能做到 O(1) 入队出队、零搬移，且天然支持按帧计数与丢弃统计。
 *
 * 为什么无锁：
 *   网关采用单线程 epoll 模型（读、路由、写都在一个线程里），不存在并发访问。
 *   这不是省事——线程池会给转发引入不可控的排队延迟，而车载网关要的是确定性。
 *   如果将来改成多线程，这里的注释位置就是加锁点。
 */

#define GW_FRAME_MAX_DLC 8
#define GW_CHANNEL_NAME_MAX 16

typedef struct
{
    uint32_t id;
    uint8_t dlc;
    uint8_t data[GW_FRAME_MAX_DLC];

    uint64_t timestamp_us; /* 收到时刻，微秒，用于延迟统计 */
    uint8_t src_channel;   /* 来自哪条通道 */
} gw_frame_t;

typedef struct
{
    gw_frame_t *slots;
    uint32_t capacity;
    uint32_t head;    /* 出队位置 */
    uint32_t count;   /* 当前元素数 */

    uint32_t dropped; /* 队满丢弃的帧数，用于监控 */
} frame_queue_t;

int frame_queue_init(frame_queue_t *q, uint32_t capacity);
void frame_queue_free(frame_queue_t *q);

/* 入队。队满返回 -1 并累加 dropped，调用方不应阻塞等待 */
int frame_queue_push(frame_queue_t *q, const gw_frame_t *f);

/* 出队。空返回 -1 */
int frame_queue_pop(frame_queue_t *q, gw_frame_t *f);

uint32_t frame_queue_count(const frame_queue_t *q);
uint32_t frame_queue_dropped(const frame_queue_t *q);
uint32_t frame_queue_capacity(const frame_queue_t *q);

#endif
