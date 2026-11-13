#define _GNU_SOURCE

#include "route_engine.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>

#include "log/log.h"

int route_engine_init(route_engine_t *e)
{
    int fd;

    if (e == 0)
        return -1;

    memset(e, 0, sizeof(*e));
    route_table_init(&e->routes);

    fd = epoll_create1(0);
    if (fd < 0)
    {
        log_error("epoll_create1 failed: %s", strerror(errno));
        return -1;
    }

    e->epoll_fd = fd;
    e->running = 1;
    return 0;
}

void route_engine_close(route_engine_t *e)
{
    if (e == 0)
        return;

    for (uint8_t i = 0; i < e->channel_count; i++)
        can_channel_close(&e->channels[i]);

    if (e->epoll_fd > 0)
        close(e->epoll_fd);

    e->epoll_fd = -1;
    e->channel_count = 0;
}

int route_engine_add_channel(route_engine_t *e, const char *ifname)
{
    struct epoll_event ev;
    uint8_t idx;

    if (e == 0 || ifname == 0)
        return -1;
    if (e->channel_count >= GW_CHANNEL_MAX)
    {
        log_error("too many channels (max %d)", GW_CHANNEL_MAX);
        return -1;
    }

    idx = e->channel_count;

    if (can_channel_open(&e->channels[idx], ifname) != 0)
        return -1;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    /*
     * epoll 的 data 存通道索引，事件到来时就知道是哪路通道，不必遍历比对 fd。
     * 用 fd 反查在通道数多时是 O(n)，而这里直接 O(1)。
     */
    ev.data.u32 = idx;

    if (epoll_ctl(e->epoll_fd, EPOLL_CTL_ADD, can_channel_fd(&e->channels[idx]), &ev) < 0)
    {
        log_error("epoll_ctl add %s failed: %s", ifname, strerror(errno));
        can_channel_close(&e->channels[idx]);
        return -1;
    }

    e->channel_count++;
    log_info("channel[%u] = %s registered", idx, ifname);
    return idx;
}

int route_engine_add_route(route_engine_t *e, const route_rule_t *rule)
{
    if (e == 0 || rule == 0)
        return -1;

    if (rule->dst_channel >= e->channel_count)
    {
        log_error("route target channel %u not registered", rule->dst_channel);
        return -1;
    }

    return route_table_add(&e->routes, rule);
}

void route_engine_stop(route_engine_t *e)
{
    if (e != 0)
        e->running = 0;
}

/* 处理某一路通道上当前可读的所有帧，最多 GW_RX_BATCH_MAX 帧 */
static void drain_channel(route_engine_t *e, uint8_t idx)
{
    gw_frame_t f;

    for (int n = 0; n < GW_RX_BATCH_MAX; n++)
    {
        const route_rule_t *rule;
        uint32_t target_id;

        if (can_channel_recv(&e->channels[idx], &f, idx) != 0)
            break; /* 读空了，或出错 */

        rule = route_table_match(&e->routes, idx, f.id);
        if (rule == 0)
        {
            e->no_route++;
            continue;
        }

        /*
         * 改写后仍要校验；配置阶段已经拦过一遍，这里是运行期兜底。
         * 越界 ID 发出去会被内核拒绝，但那时已经浪费一次系统调用。
         */
        if (route_target_id(rule, f.id, &target_id) != 0)
        {
            e->rewrite_failed++;
            continue;
        }

        f.id = target_id;

        if (can_channel_send(&e->channels[rule->dst_channel], &f) != 0)
            e->send_failed++;
        else
            e->forwarded++;
    }
}

int route_engine_run(route_engine_t *e)
{
    struct epoll_event events[GW_EPOLL_MAX_EVENTS];

    if (e == 0 || e->epoll_fd < 0)
        return -1;

    log_info("engine running: %u channel(s), %u rule(s)",
             e->channel_count, e->routes.count);

    while (e->running)
    {
        int n = epoll_wait(e->epoll_fd, events, GW_EPOLL_MAX_EVENTS, GW_EPOLL_TIMEOUT_MS);

        if (n < 0)
        {
            if (errno == EINTR)
                continue; /* 被信号打断是正常的 */
            log_error("epoll_wait failed: %s", strerror(errno));
            return -1;
        }

        for (int i = 0; i < n; i++)
        {
            uint8_t idx = (uint8_t)events[i].data.u32;

            if (idx >= e->channel_count)
                continue;

            if (events[i].events & EPOLLIN)
                drain_channel(e, idx);

            if (events[i].events & EPOLLERR)
                log_warn("channel %s reported EPOLLERR", e->channels[idx].name);
        }
    }

    return 0;
}

void route_engine_print_stats(const route_engine_t *e)
{
    if (e == 0)
        return;

    log_info("stats: forwarded=%llu no_route=%llu send_failed=%llu rewrite_failed=%llu",
             (unsigned long long)e->forwarded,
             (unsigned long long)e->no_route,
             (unsigned long long)e->send_failed,
             (unsigned long long)e->rewrite_failed);

    for (uint8_t i = 0; i < e->channel_count; i++)
    {
        const can_channel_t *ch = &e->channels[i];
        log_info("  channel[%u] %-8s rx=%u tx=%u rx_err=%u tx_err=%u",
                 i, ch->name, ch->rx_frames, ch->tx_frames, ch->rx_errors, ch->tx_errors);
    }
}
