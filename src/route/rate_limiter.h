#ifndef __RATE_LIMITER_H
#define __RATE_LIMITER_H

#include <stdint.h>

#include "can_channel.h"

/*
 * 速率限制（洪水防护）。
 *
 * 为什么要做：
 *   一条 CAN 总线的带宽是固定的。某个节点故障或被攻陷后疯狂发报文，
 *   会占满总线，让其他节点的关键报文发不出去——这就是报文洪水（flooding）
 *   导致的总线级拒绝服务。网关如果只转发不限速，等于把这个故障扩散到整网。
 *
 * 两级限制：
 *   1. 每通道总量 —— 兜底，防未知 ID 的洪水
 *   2. 每 ID 单独限速 —— 精确保护已知的关键报文
 *
 * 窗口算法用固定窗口（每 WINDOW_MS 清零），不是滑动窗口。
 * 固定窗口在窗口边界可能有 2 倍突发，但对洪水防护够用，且实现简单、内存恒定。
 * 真要做平滑限速才需要令牌桶。
 */

#define GW_RATE_MAX_IDS      64   /* 单独配置限速的 ID 数上限 */
#define GW_RATE_WINDOW_MS    1000 /* 统计窗口 */
#define GW_RATE_UNLIMITED    0    /* limit 为 0 表示不限 */

typedef struct
{
    uint32_t id;
    uint8_t channel;
    uint32_t limit;        /* 每窗口允许的帧数 */
    uint32_t count;        /* 当前窗口已通过帧数 */
    uint64_t window_start; /* 窗口起点 */
    uint32_t dropped;      /* 该 ID 累计被限 */
} rate_id_entry_t;

typedef struct
{
    uint32_t limit;
    uint32_t count;
    uint64_t window_start;
    uint32_t dropped;
} rate_channel_t;

typedef struct
{
    rate_id_entry_t ids[GW_RATE_MAX_IDS];
    uint16_t id_count;

    rate_channel_t channels[GW_CHANNEL_MAX];

    uint32_t passed;
    uint32_t total_dropped;
} rate_limiter_t;

void rate_limiter_init(rate_limiter_t *r);

/* 设置某通道的总速率上限（帧/秒）。0 表示不限 */
void rate_limiter_set_channel(rate_limiter_t *r, uint8_t channel, uint32_t limit);

/* 为某个 ID 设置单独上限。同一 (channel, id) 重复设置会覆盖 */
int rate_limiter_set_id(rate_limiter_t *r, uint8_t channel, uint32_t id, uint32_t limit);

/*
 * 判断是否允许通过。返回 1 允许，0 拒绝（已计入 dropped）。
 * 检查顺序：先通道总量，再单 ID —— 通道级是兜底，先挡掉整体洪水的绝大部分。
 */
int rate_limiter_allow(rate_limiter_t *r, uint8_t channel, uint32_t id, uint64_t now_ms);

#endif
