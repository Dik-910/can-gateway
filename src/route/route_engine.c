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

/*
 * 链路维护：给需要重开的通道重新 bind socket，并挂回 epoll。
 *
 * 为什么不能「读到错误就 close 再 open」：
 *   接口被 down 掉之后，open 会一直失败（SIOCGIFINDEX 找不到设备），
 *   每帧重试一次等于把主循环拖进 syscall 风暴。所以失败要退避。
 *
 * 为什么要重新 EPOLL_CTL_ADD：
 *   fd 是新的。旧 fd 在 epoll 里的注册随 close 一起消失，
 *   内核不会替新 fd 保留注册 —— 忘了这步的表现是「重开成功、统计不动」。
 */
static void service_reconnect(route_engine_t *e, uint64_t now_ms)
{
    for (uint8_t i = 0; i < e->channel_count; i++)
    {
        can_channel_t *ch = &e->channels[i];
        struct epoll_event ev;

        if (!can_channel_needs_reopen(ch))
            continue;
        if (now_ms < e->reconnect_at[i])
            continue;

        /* 旧 fd 可能已经无效，DEL 失败不算问题 */
        epoll_ctl(e->epoll_fd, EPOLL_CTL_DEL, ch->fd, 0);

        if (can_channel_reopen(ch) != 0)
        {
            e->reconnect_at[i] = now_ms + GW_CHANNEL_RECONNECT_MS;
            continue;
        }

        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.u32 = i;

        if (epoll_ctl(e->epoll_fd, EPOLL_CTL_ADD, can_channel_fd(ch), &ev) < 0)
        {
            log_error("epoll_ctl add %s after reopen failed: %s",
                      ch->name, strerror(errno));
            ch->need_reopen = 1;
            e->reconnect_at[i] = now_ms + GW_CHANNEL_RECONNECT_MS;
            continue;
        }

        e->reconnect_at[i] = 0;
        e->reconnects++;
        log_info("channel %s back in service (reopen #%u)", ch->name, ch->reopen_count);
    }
}

/*
 * 响应结算。节点回 0x7F <sid> 78 表示仍在处理，此时延长窗口而不是判超时；
 * 其余响应视为请求已应答。
 *
 * 只认「单帧且声明长度恰好 3 字节」的形态，避免把业务数据里恰好出现的
 * 0x7F / 0x78 误判成响应挂起。
 */
static void diag_note_response(route_engine_t *e, uint8_t node_ch, uint32_t rsp_id,
                               const uint8_t *data, uint8_t dlc, uint64_t now_ms)
{
    if (dlc >= 4 && (data[0] & 0xF0u) == 0x00u && (data[0] & 0x0Fu) == 0x03u &&
        data[1] == 0x7Fu && data[3] == 0x78u)
    {
        diag_router_extend(&e->diag, node_ch, rsp_id, now_ms);
        return;
    }

    diag_router_answered(&e->diag, node_ch, rsp_id);
}

/* 诊断请求的超时扫描与重传。重传的帧与首次转发的内容完全一致 */
static void service_diag_timeouts(route_engine_t *e, uint64_t now_ms)
{
    diag_retry_t retry[GW_DIAG_MAX_MAPS];
    uint8_t n;

    if (!e->diag_enabled)
        return;

    n = diag_router_service(&e->diag, now_ms, retry, GW_DIAG_MAX_MAPS);

    for (uint8_t i = 0; i < n; i++)
    {
        gw_frame_t f;

        if (retry[i].node_ch >= e->channel_count)
            continue;

        memset(&f, 0, sizeof(f));
        f.id = retry[i].req_id;
        f.dlc = retry[i].dlc;
        f.src_channel = retry[i].node_ch;
        memcpy(f.data, retry[i].data, retry[i].dlc);

        if (can_channel_send(&e->channels[retry[i].node_ch], &f) != 0)
            e->send_failed++;
        else
        {
            e->forwarded++;
            e->diag_forwarded++;
        }
    }
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
            int is_request = (hit == 0);

            if (hit != 0)
                hit = diag_router_response(&e->diag, now_ms, idx, f.id, &dst_ch, &target_id);

            if (hit == 0)
            {
                /* 响应方向：在 ID 被改写之前用节点侧的 ID 结算待响应项 */
                if (!is_request)
                    diag_note_response(e, idx, f.id, f.data, f.dlc, now_ms);

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

                    /* 请求方向：转发成功后登记一条待响应项 */
                    if (is_request)
                        diag_router_track(&e->diag, dst_ch, f.id, f.data, f.dlc, now_ms);
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

        /* 链路维护：处理上一轮标记出来的断链通道 */
        service_reconnect(e, engine_now_ms());

        /* 诊断会话监督：到期的请求重传，重传用尽仍无响应的计为超时 */
        service_diag_timeouts(e, engine_now_ms());

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

            /*
             * EPOLLERR：接口出错（down、被删、驱动卸载）。
             * 只打日志不够 —— 之后 read 会一直失败，这路通道等于静默死亡。
             * 标成需要重开，交给 service_reconnect 处理。
             */
            if (events[i].events & (EPOLLERR | EPOLLHUP))
            {
                can_channel_note_error(&e->channels[idx], ENETDOWN);
                log_warn("channel %s reported %s", e->channels[idx].name,
                         (events[i].events & EPOLLERR) ? "EPOLLERR" : "EPOLLHUP");
            }
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

    if (e->diag_enabled)
    {
        log_info("  diag: tracked=%u answered=%u timeout=%u retry=%u pending_ext=%u",
                 e->diag.tracked, e->diag.answered, e->diag.timeouts,
                 e->diag.retries, e->diag.pending_ext);
    }

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

            log_info("  channel[%u] %-8s rx=%u tx=%u load=%u.%u%% err=%u (bus %u ctrl %u) "
                     "reopen=%u fail=%u%s",
                     i, ch->name, ch->rx_frames, ch->tx_frames,
                     load / 10u, load % 10u,
                     ch->err_frames, ch->err_bus, ch->err_ctrl,
                     ch->reopen_count, ch->reopen_fail,
                     ch->need_reopen ? " DOWN" : "");
        }
    }
}
