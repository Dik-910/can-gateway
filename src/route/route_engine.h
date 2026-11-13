#ifndef __ROUTE_ENGINE_H
#define __ROUTE_ENGINE_H

#include <stdint.h>

#include "can_channel.h"
#include "route_table.h"

/*
 * 转发引擎：epoll 监听多路 CAN，收到帧就查路由表转发出去。
 *
 * 为什么是单线程：
 *   车载网关要的是确定性延迟。线程池里一个诊断报文可能排在几十个周期报文后面，
 *   延迟从 1ms 变成 20ms 且不可预测。单线程循环里，最坏延迟约等于
 *   （一次 batch 的处理时间 + 一次 epoll_wait 超时），可以估算。
 *
 * 为什么每次事件要循环读空：
 *   每帧调一次 epoll_wait 会多一次系统调用。CAN 满载时这成为主要开销。
 *   一次读完再返回，但设上限——否则一个高速通道会把其他通道饿死。
 */

#define GW_EPOLL_MAX_EVENTS 8
#define GW_RX_BATCH_MAX     64   /* 单次事件最多处理帧数，防止饿死其他通道 */
#define GW_EPOLL_TIMEOUT_MS 100  /* 超时用于检查退出标志与打印统计 */

typedef struct
{
    can_channel_t channels[GW_CHANNEL_MAX];
    uint8_t channel_count;

    route_table_t routes;

    int epoll_fd;
    int running;

    /* 监控指标 */
    uint64_t forwarded;       /* 成功转发的帧数 */
    uint64_t no_route;        /* 没有规则匹配而丢弃 */
    uint64_t send_failed;     /* 转发时发送失败 */
    uint64_t rewrite_failed;  /* ID 改写越界（理论上配置阶段已拦） */
} route_engine_t;

int route_engine_init(route_engine_t *e);
void route_engine_close(route_engine_t *e);

/* 打开一路通道并加入 epoll。返回通道索引，失败返回 -1 */
int route_engine_add_channel(route_engine_t *e, const char *ifname);

/* 添加路由规则，转发到已注册的通道索引 */
int route_engine_add_route(route_engine_t *e, const route_rule_t *rule);

/* 阻塞运行，直到 route_engine_stop 被调用 */
int route_engine_run(route_engine_t *e);

void route_engine_stop(route_engine_t *e);

/* 打印一行统计（由 run 循环周期调用或外部调用） */
void route_engine_print_stats(const route_engine_t *e);

#endif
