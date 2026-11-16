#define _GNU_SOURCE

#include "route_engine.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>

#include "log/log.h"

int route_engine_init(route_engine_t *e)
{
    int fd;

    if (e == 0)
        return -1;

    memset(e, 0, sizeof(*e));
    route_table_init(&e->routes);
    diag_router_init(&e->diag);
    rate_limiter_init(&e->rate);
    agg_init(&e->agg);

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

int route_engine_add_channel_ex(route_engine_t *e, const char *ifname, uint32_t bitrate)
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

    if (can_channel_open_ex(&e->channels[idx], ifname, bitrate) != 0)
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

int route_engine_add_channel(route_engine_t *e, const char *ifname)
{
    return route_engine_add_channel_ex(e, ifname, 0);
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

int route_engine_add_diag_map(route_engine_t *e, const diag_map_t *m)
{
    if (e == 0 || m == 0)
        return -1;

    if (m->tester_channel >= e->channel_count || m->target_channel >= e->channel_count)
    {
        log_error("diag map references unregistered channel (%u, %u)",
                  m->tester_channel, m->target_channel);
        return -1;
    }

    if (diag_router_add(&e->diag, m) != 0)
        return -1;

    e->diag_enabled = 1;
    return 0;
}

void route_engine_set_channel_limit(route_engine_t *e, uint8_t channel, uint32_t limit)
{
    if (e == 0 || limit == 0)
        return;

    rate_limiter_set_channel(&e->rate, channel, limit);
    e->rate_enabled = 1;
}

int route_engine_set_id_limit(route_engine_t *e, uint8_t channel, uint32_t id, uint32_t limit)
{
    if (e == 0 || limit == 0)
        return -1;

    if (rate_limiter_set_id(&e->rate, channel, id, limit) != 0)
        return -1;

    e->rate_enabled = 1;
    return 0;
}

int route_engine_enable_uplink(route_engine_t *e, uplink_t *u,
                               uint32_t window_ms, uint32_t force_ms,
                               uint8_t ch_mask)
{
    if (e == 0 || u == 0)
        return -1;

    e->uplink = u;
    e->uplink_window_ms = (window_ms != 0) ? window_ms : AGG_DEFAULT_WINDOW;
    e->uplink_force_ms = (force_ms != 0) ? force_ms : AGG_DEFAULT_FORCE;
    e->uplink_ch_mask = ch_mask;
    /* 0 表示「窗口还没开始」，run 循环第一次进来时才取时间 */
    e->uplink_last_ms = 0;
    e->uplink_enabled = 1;

    log_info("uplink enabled: window %u ms, force %u ms, channel mask 0x%02X",
             e->uplink_window_ms, e->uplink_force_ms, ch_mask);
    return 0;
}

void route_engine_stop(route_engine_t *e)
{
    if (e != 0)
        e->running = 0;
}

/* 处理某一路通道上当前可读的所有帧，最多 GW_RX_BATCH_MAX 帧 */
/*
 * 聚合导出回调：组一条 JSON 投进上行队列。
 *
 * 手写 JSON 而不用 cJSON：
 * 格式固定（就这几个字段），一次 snprintf 就够，不必为此引入依赖、
 * 也不必每帧 malloc/free —— 在转发主路径上，分配是最不该出现的东西。
 *
 * 注意返回非 0 时 aggregator 会保留 dirty，下个窗口重试。
 * 队列满导致的丢弃是可控的；丢数据但保留「有新值」的标记才是对的。
 */
static int on_agg_export(const agg_entry_t *en, void *user)
{
    route_engine_t *e = (route_engine_t *)user;
    char hex[2 * GW_FRAME_MAX_DLC + 1];
    char buf[GW_UPLINK_MSG_MAX];
    int n = 0;

    for (uint8_t i = 0; i < en->dlc && i < GW_FRAME_MAX_DLC; i++)
        n += snprintf(hex + n, sizeof(hex) - (size_t)n, "%02X", en->data[i]);
    hex[n] = '\0';

    snprintf(buf, sizeof(buf),
             "{\"gw\":\"can-gateway\",\"ch\":\"%s\",\"id\":\"0x%03X\","
             "\"n\":%u,\"dlc\":%u,\"d\":\"%s\",\"ts\":%llu}",
             e->channels[en->channel].name,
             en->id,
             (unsigned)en->count,
             (unsigned)en->dlc,
             hex,
             (unsigned long long)en->last_ms);

    if (e->uplink == 0 || uplink_publish(e->uplink, buf) != 0)
        return -1;

    e->uplink_exported++;
    return 0;
}

/* 处理某一路通道上当前可读的所有帧，最多 GW_RX_BATCH_MAX 帧 */
static void drain_channel(route_engine_t *e, uint8_t idx)
{
    gw_frame_t f;

    for (int n = 0; n < GW_RX_BATCH_MAX; n++)
    {
        const route_rule_t *rule;
        uint32_t target_id;
        uint8_t dst_ch;
        uint64_t now_ms;

        if (can_channel_recv(&e->channels[idx], &f, idx) != 0)
            break; /* 读空了，或出错 */

        now_ms = f.timestamp_us / 1000ull;

        /*
         * 采集放在路由之前：没有路由规则的帧（no_route）也要上报。
         * 网关的旁路监测价值不该被路由表决定 —— 总线上有些帧本来就
         * 只给远端 ECU 看的，不需要转发，但云端仍然想知道它还在发。
         */
        if (e->uplink_enabled &&
            (e->uplink_ch_mask == 0 ||
             (e->uplink_ch_mask & (uint8_t)(1u << idx)) != 0))
        {
            agg_update(&e->agg, idx, f.id, f.data, f.dlc);
        }

        /*
         * 诊断映射优先于静态路由表。
         * 顺序不能反：一旦先落到静态规则，tester 侧的 ID 改写就没机会生效，
         * 多个 tester 并发时响应会全部涌向同一个 ID。
         */
        if (e->diag_enabled)
        {
            int hit = diag_router_request(&e->diag, now_ms, idx, f.id, &dst_ch, &target_id);

            if (hit != 0)
                hit = diag_router_response(&e->diag, now_ms, idx, f.id, &dst_ch, &target_id);

            if (hit == 0)
            {
                if (dst_ch >= e->channel_count)
                {
                    e->rewrite_failed++;
                    continue;
                }

                f.id = target_id;

                if (can_channel_send(&e->channels[dst_ch], &f) != 0)
                    e->send_failed++;
                else
                {
                    e->forwarded++;
                    e->diag_forwarded++;
                }
                continue;
            }
        }

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

        /* 限速只作用于普通业务帧；诊断帧走上面的分支，不受影响 */
        if (e->rate_enabled && !rate_limiter_allow(&e->rate, idx, f.id, now_ms))
        {
            e->rate_limited++;
            continue;
        }

        f.id = target_id;

        if (can_channel_send(&e->channels[rule->dst_channel], &f) != 0)
            e->send_failed++;
        else
            e->forwarded++;
    }
}

/* 单调时钟毫秒，用于统计间隔计时 */
static uint64_t engine_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

int route_engine_run(route_engine_t *e, uint32_t stats_interval_ms)
{
    struct epoll_event events[GW_EPOLL_MAX_EVENTS];
    uint64_t last_stats = 0;

    if (e == 0 || e->epoll_fd < 0)
        return -1;

    log_info("engine running: %u channel(s), %u rule(s)",
             e->channel_count, e->routes.count);

    if (stats_interval_ms > 0)
        last_stats = engine_now_ms();

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

        /* 聚合导出：按窗口把本周期有变化的条目推给上行 */
        if (e->uplink_enabled)
        {
            uint64_t now = engine_now_ms();

            if (e->uplink_last_ms == 0)
            {
                /*
                 * 第一次进来只记窗口起点，不导出。
                 *
                 * 之前是「首次出现立刻导出」，结果是上报里的 n（帧数）
                 * 只是刚启动那一瞬间的零头（实测 7，而一个窗口应有 10）。
                 * 上游看到这个数字会以为丢帧了。等满一个窗口再导，
                 * n 才是真实的窗口帧数——代价是新 ID 最多晚一个窗口出现。
                 */
                e->uplink_last_ms = now;
            }
            else if (now - e->uplink_last_ms >= e->uplink_window_ms)
            {
                agg_export(&e->agg, now, e->uplink_force_ms, on_agg_export, e);
                e->uplink_last_ms = now;
            }
        }

        /* 周期统计：放在循环末尾，避免影响本轮的转发处理 */
        if (stats_interval_ms > 0)
        {
            uint64_t now = engine_now_ms();
            if (now - last_stats >= stats_interval_ms)
            {
                route_engine_print_stats(e);
                last_stats = now;
            }
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

void route_engine_print_stats(route_engine_t *e)
{
    if (e == 0)
        return;

    log_info("stats: forwarded=%llu (diag %llu) no_route=%llu rate_limited=%llu send_failed=%llu rewrite_failed=%llu",
             (unsigned long long)e->forwarded,
             (unsigned long long)e->diag_forwarded,
             (unsigned long long)e->no_route,
             (unsigned long long)e->rate_limited,
             (unsigned long long)e->send_failed,
             (unsigned long long)e->rewrite_failed);

    {
        uint64_t now = engine_now_ms();

        if (e->uplink_enabled && e->uplink != 0)
        {
            log_info("  uplink: agg_entries=%d exported=%llu overflow=%llu",
                     agg_count(&e->agg),
                     (unsigned long long)e->uplink_exported,
                     (unsigned long long)e->agg.overflow);
            uplink_print_stats(e->uplink);
        }

        for (uint8_t i = 0; i < e->channel_count; i++)
        {
            can_channel_t *ch = &e->channels[i];
            uint32_t load = can_channel_update_load(ch, now);

            log_info("  channel[%u] %-8s rx=%u tx=%u load=%u.%u%% err=%u (bus %u ctrl %u)",
                     i, ch->name, ch->rx_frames, ch->tx_frames,
                     load / 10u, load % 10u,
                     ch->err_frames, ch->err_bus, ch->err_ctrl);
        }
    }
}
