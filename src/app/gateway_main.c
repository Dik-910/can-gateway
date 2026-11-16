#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include "config.h"
#include "route_engine.h"
#include "log/log.h"

/*
 * 网关主程序。
 *
 * 通道、路由、诊断映射全部来自配置文件，改规则不需要重新编译。
 * 启动时把读到的配置打印一遍——配置写错却静默忽略，是网关最难查的故障之一。
 *
 * 用法：
 *   ./gateway                              # 读 examples/gateway.conf
 *   ./gateway --config /etc/gw.conf        # 指定配置
 *   ./gateway --stats 10                   # 每 10 秒打印一次统计
 */

static route_engine_t *g_engine = 0;

static void on_signal(int sig)
{
    (void)sig;
    if (g_engine != 0)
        route_engine_stop(g_engine);
}

/* 配置里的通道名 -> 引擎里的通道索引 */
static int resolve_channel(const gw_config_t *cfg, const int *ch_idx, const char *name)
{
    int i = config_find_channel(cfg, name);

    if (i < 0)
        return -1;

    return ch_idx[i];
}

/*
 * 把 "a,b" 这样的通道短名列表转成位掩码。
 * 空字符串返回 0（表示全部通道）。
 * 遇到未知名字返回 -1 —— 配置写错必须启动即失败，
 * 静默忽略会出现「明明配了采集却一条数据都没有」。
 */
static int resolve_channel_mask(const gw_config_t *cfg, const int *ch_idx,
                                const char *list, uint8_t *out_mask)
{
    char buf[64];
    char *p, *tok;
    uint8_t mask = 0;

    *out_mask = 0;

    if (list == 0 || list[0] == '\0')
        return 0;

    snprintf(buf, sizeof(buf), "%s", list);

    p = buf;
    while ((tok = strtok(p, ",")) != 0)
    {
        int idx;

        p = 0;

        /* 去掉前后空格 */
        while (*tok == ' ')
            tok++;
        {
            size_t n = strlen(tok);
            while (n > 0 && (tok[n - 1] == ' ' || tok[n - 1] == '\t'))
                tok[--n] = '\0';
        }

        if (*tok == '\0')
            continue;

        idx = resolve_channel(cfg, ch_idx, tok);
        if (idx < 0)
        {
            log_error("mqtt.channels references unknown channel '%s'", tok);
            return -1;
        }

        mask = (uint8_t)(mask | (1u << idx));
    }

    *out_mask = mask;
    return 0;
}

int main(int argc, char **argv)
{
    const char *conf_path = "examples/gateway.conf";
    int stats_interval_s = 0;
    gw_config_t cfg;
    route_engine_t engine;
    int ch_idx[CFG_MAX_CHANNELS];
    int rc = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            conf_path = argv[++i];
        else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc)
            stats_interval_s = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0)
        {
            printf("usage: %s [--config path] [--stats seconds]\n", argv[0]);
            printf("  default config: examples/gateway.conf\n");
            return 0;
        }
    }

    log_set_level(LOG_INFO);

    if (config_load(&cfg, conf_path) != 0)
        return 1;

    config_dump(&cfg);

    if (route_engine_init(&engine) != 0)
        return 1;

    g_engine = &engine;

    /* 按配置顺序建通道，记录配置序号到引擎索引的对应关系 */
    for (int i = 0; i < cfg.channel_count; i++)
    {
        int idx = route_engine_add_channel_ex(&engine, cfg.channels[i].ifname,
                                              cfg.channels[i].bitrate);

        if (idx < 0)
        {
            log_error("failed to open channel %s (%s)",
                      cfg.channels[i].name, cfg.channels[i].ifname);
            route_engine_close(&engine);
            return 1;
        }

        ch_idx[i] = idx;
    }

    /* 路由规则 */
    for (int i = 0; i < cfg.route_count; i++)
    {
        const cfg_route_t *cr = &cfg.routes[i];
        route_rule_t rr;

        memset(&rr, 0, sizeof(rr));
        rr.src_channel = (uint8_t)resolve_channel(&cfg, ch_idx, cr->src);
        rr.dst_channel = (uint8_t)resolve_channel(&cfg, ch_idx, cr->dst);
        rr.id_min = cr->id_min;
        rr.id_max = cr->id_max;
        rr.id_mask = cr->id_mask;
        rr.id_offset = cr->offset;
        rr.enabled = 1;
        snprintf(rr.note, sizeof(rr.note), "%s", cr->note);

        if (route_engine_add_route(&engine, &rr) != 0)
        {
            log_error("route %d rejected: %s 0x%03X-0x%03X -> %s",
                      i, cr->src, cr->id_min, cr->id_max, cr->dst);
            rc = 1;
            goto out;
        }
    }

    /* 诊断映射 */
    for (int i = 0; i < cfg.diag_count; i++)
    {
        const cfg_diag_t *cd = &cfg.diags[i];
        diag_map_t dm;

        memset(&dm, 0, sizeof(dm));
        dm.tester_channel = (uint8_t)resolve_channel(&cfg, ch_idx, cd->tester_ch);
        dm.tester_req_id = cd->tester_req;
        dm.tester_rsp_id = cd->tester_rsp;
        dm.target_channel = (uint8_t)resolve_channel(&cfg, ch_idx, cd->target_ch);
        dm.target_req_id = cd->target_req;
        dm.target_rsp_id = cd->target_rsp;

        if (route_engine_add_diag_map(&engine, &dm) != 0)
        {
            log_error("diag map %d rejected: %s 0x%03X <-> %s 0x%03X",
                      i, cd->tester_ch, cd->tester_req, cd->target_ch, cd->target_req);
            rc = 1;
            goto out;
        }
    }

    /* 速率限制：通道上限防洪水，单 ID 上限精确保护关键报文 */
    for (int i = 0; i < cfg.chan_limit_count; i++)
    {
        int idx = resolve_channel(&cfg, ch_idx, cfg.chan_limits[i].ch);

        if (idx >= 0)
        {
            route_engine_set_channel_limit(&engine, (uint8_t)idx, cfg.chan_limits[i].limit);
            log_info("rate limit: channel %s <= %u frame/s",
                     cfg.chan_limits[i].ch, cfg.chan_limits[i].limit);
        }
    }

    for (int i = 0; i < cfg.id_limit_count; i++)
    {
        int idx = resolve_channel(&cfg, ch_idx, cfg.id_limits[i].ch);

        if (idx >= 0 &&
            route_engine_set_id_limit(&engine, (uint8_t)idx, cfg.id_limits[i].id,
                                      cfg.id_limits[i].limit) == 0)
        {
            log_info("rate limit: %s 0x%03X <= %u frame/s",
                     cfg.id_limits[i].ch, cfg.id_limits[i].id, cfg.id_limits[i].limit);
        }
    }

    /* ---- MQTT 上行 ---- */
    if (cfg.mqtt.enabled)
    {
        uint8_t ch_mask = 0;

        if (resolve_channel_mask(&cfg, ch_idx, cfg.mqtt.channels, &ch_mask) != 0)
        {
            rc = 1;
            goto out;
        }

        if (!uplink_available())
        {
            log_warn("mqtt configured but built without Paho, uplink disabled");
        }
        else
        {
            uplink_t *up = uplink_create(cfg.mqtt.addr, cfg.mqtt.topic,
                                         cfg.mqtt.client_id);

            if (up == 0)
            {
                log_error("uplink create failed");
                rc = 1;
                goto out;
            }

            if (uplink_start(up) != 0)
            {
                log_error("uplink start failed");
                uplink_destroy(up);
                rc = 1;
                goto out;
            }

            if (route_engine_enable_uplink(&engine, up, cfg.mqtt.window_ms, ch_mask) != 0)
            {
                log_error("enable uplink failed");
                uplink_destroy(up);
                rc = 1;
                goto out;
            }

            log_info("mqtt uplink: %s topic %s, window %u ms",
                     cfg.mqtt.addr, cfg.mqtt.topic, cfg.mqtt.window_ms);
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    log_info("gateway starting");

    route_engine_run(&engine, (uint32_t)stats_interval_s * 1000u);

    log_info("gateway stopping");
    route_engine_print_stats(&engine);

    if (engine.uplink != 0)
    {
        uplink_t *up = engine.uplink;

        /* 先把队列里剩的尽量发完，再释放 */
        log_info("draining uplink queue...");
        engine.uplink = 0;
        uplink_destroy(up);
    }

out:
    route_engine_close(&engine);
    return rc;
}
