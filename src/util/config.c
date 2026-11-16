#define _GNU_SOURCE

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "log/log.h"

#define LINE_MAX_LEN 256

/* 去掉首尾空白，返回指向串内的指针（原地修改） */
static char *trim(char *s)
{
    char *end;

    while (*s != '\0' && isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;

    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        end--;
    end[1] = '\0';
    return s;
}

/* 支持十进制与 0x 十六进制 */
static int parse_u32(const char *s, uint32_t *out)
{
    char *end;
    unsigned long v;

    if (s == 0 || out == 0 || *s == '\0')
        return -1;

    v = strtoul(s, &end, 0);
    if (*end != '\0')
        return -1;

    *out = (uint32_t)v;
    return 0;
}

static int parse_i32(const char *s, int32_t *out)
{
    char *end;
    long v;

    if (s == 0 || out == 0 || *s == '\0')
        return -1;

    v = strtol(s, &end, 0);
    if (*end != '\0')
        return -1;

    *out = (int32_t)v;
    return 0;
}

int config_find_channel(const gw_config_t *cfg, const char *name)
{
    if (cfg == 0 || name == 0)
        return -1;

    for (int i = 0; i < cfg->channel_count; i++)
    {
        if (strcmp(cfg->channels[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* 解析 routes 段的一行 CSV */
static int parse_route_line(gw_config_t *cfg, char *line, int lineno)
{
    cfg_route_t *r;
    char *fields[7];
    int n = 0;
    char *p = line;

    if (cfg->route_count >= CFG_MAX_ROUTES)
    {
        log_error("config line %d: too many routes (max %d)", lineno, CFG_MAX_ROUTES);
        return -1;
    }

    /* 按逗号切，最多 7 段（最后一段是可选掩码） */
    fields[n++] = p;
    while (n < 7 && (p = strchr(p, ',')) != 0)
    {
        *p = '\0';
        p++;
        fields[n++] = p;
    }

    if (n < 5)
    {
        log_error("config line %d: route needs at least 5 fields", lineno);
        return -1;
    }

    r = &cfg->routes[cfg->route_count];
    memset(r, 0, sizeof(*r));

    snprintf(r->src, sizeof(r->src), "%s", trim(fields[0]));
    if (parse_u32(trim(fields[1]), &r->id_min) != 0 ||
        parse_u32(trim(fields[2]), &r->id_max) != 0)
    {
        log_error("config line %d: invalid id range", lineno);
        return -1;
    }

    snprintf(r->dst, sizeof(r->dst), "%s", trim(fields[3]));

    if (parse_i32(trim(fields[4]), &r->offset) != 0)
    {
        log_error("config line %d: invalid offset", lineno);
        return -1;
    }

    if (n >= 6)
        snprintf(r->note, sizeof(r->note), "%s", trim(fields[5]));

    if (n >= 7 && parse_u32(trim(fields[6]), &r->id_mask) != 0)
    {
        log_error("config line %d: invalid id mask", lineno);
        return -1;
    }

    cfg->route_count++;
    return 0;
}

/* 解析 diag 段的一行 CSV */
static int parse_diag_line(gw_config_t *cfg, char *line, int lineno)
{
    cfg_diag_t *d;
    char *fields[6];
    int n = 0;
    char *p = line;

    if (cfg->diag_count >= CFG_MAX_DIAGS)
    {
        log_error("config line %d: too many diag maps (max %d)", lineno, CFG_MAX_DIAGS);
        return -1;
    }

    fields[n++] = p;
    while (n < 6 && (p = strchr(p, ',')) != 0)
    {
        *p = '\0';
        p++;
        fields[n++] = p;
    }

    if (n < 6)
    {
        log_error("config line %d: diag map needs 6 fields", lineno);
        return -1;
    }

    d = &cfg->diags[cfg->diag_count];
    memset(d, 0, sizeof(*d));

    snprintf(d->tester_ch, sizeof(d->tester_ch), "%s", trim(fields[0]));
    if (parse_u32(trim(fields[1]), &d->tester_req) != 0 ||
        parse_u32(trim(fields[2]), &d->tester_rsp) != 0)
    {
        log_error("config line %d: invalid tester id", lineno);
        return -1;
    }

    snprintf(d->target_ch, sizeof(d->target_ch), "%s", trim(fields[3]));
    if (parse_u32(trim(fields[4]), &d->target_req) != 0 ||
        parse_u32(trim(fields[5]), &d->target_rsp) != 0)
    {
        log_error("config line %d: invalid target id", lineno);
        return -1;
    }

    cfg->diag_count++;
    return 0;
}

/*
 * [limits] 段两种写法：
 *   a = 3000            通道总速率
 *   id a,0x181,60       单 ID 速率
 */
/*
 * [mqtt] 段：key = value
 *   addr / topic / client_id / window / channels
 */
static int parse_mqtt_line(gw_config_t *cfg, char *line, int lineno)
{
    char *eq = strchr(line, '=');
    char *key, *val;

    if (eq == 0)
    {
        log_error("config line %d: mqtt entry needs 'key = value'", lineno);
        return -1;
    }

    *eq = '\0';
    key = trim(line);
    val = trim(eq + 1);

    if (strcmp(key, "addr") == 0)
        snprintf(cfg->mqtt.addr, sizeof(cfg->mqtt.addr), "%s", val);
    else if (strcmp(key, "topic") == 0)
        snprintf(cfg->mqtt.topic, sizeof(cfg->mqtt.topic), "%s", val);
    else if (strcmp(key, "client_id") == 0)
        snprintf(cfg->mqtt.client_id, sizeof(cfg->mqtt.client_id), "%s", val);
    else if (strcmp(key, "channels") == 0)
        snprintf(cfg->mqtt.channels, sizeof(cfg->mqtt.channels), "%s", val);
    else if (strcmp(key, "window") == 0)
    {
        if (parse_u32(val, &cfg->mqtt.window_ms) != 0 || cfg->mqtt.window_ms < 100)
        {
            log_error("config line %d: window must be >= 100 ms", lineno);
            return -1;
        }
    }
    else if (strcmp(key, "enabled") == 0)
    {
        cfg->mqtt.enabled = (atoi(val) != 0);
    }
    else
    {
        log_error("config line %d: unknown mqtt key '%s'", lineno, key);
        return -1;
    }

    return 0;
}

static int parse_limit_line(gw_config_t *cfg, char *line, int lineno)
{
    char *p;

    if (strncmp(line, "id ", 3) == 0)
    {
        char *fields[3];
        int n = 0;
        char *cur = line + 3;
        cfg_id_limit_t *l;

        if (cfg->id_limit_count >= CFG_MAX_LIMITS)
        {
            log_error("config line %d: too many id limits (max %d)", lineno, CFG_MAX_LIMITS);
            return -1;
        }

        fields[n++] = cur;
        while (n < 3 && (cur = strchr(cur, ',')) != 0)
        {
            *cur = '\0';
            cur++;
            fields[n++] = cur;
        }

        if (n < 3)
        {
            log_error("config line %d: id limit needs 'id ch,id,limit'", lineno);
            return -1;
        }

        l = &cfg->id_limits[cfg->id_limit_count];
        memset(l, 0, sizeof(*l));
        snprintf(l->ch, sizeof(l->ch), "%s", trim(fields[0]));

        if (parse_u32(trim(fields[1]), &l->id) != 0 ||
            parse_u32(trim(fields[2]), &l->limit) != 0)
        {
            log_error("config line %d: invalid id limit numbers", lineno);
            return -1;
        }

        cfg->id_limit_count++;
        return 0;
    }

    /* 通道总速率：name = limit */
    p = strchr(line, '=');
    if (p == 0)
    {
        log_error("config line %d: limit needs 'channel = number' or 'id ...'", lineno);
        return -1;
    }

    if (cfg->chan_limit_count >= CFG_MAX_CHANNELS)
    {
        log_error("config line %d: too many channel limits", lineno);
        return -1;
    }

    *p = '\0';
    {
        cfg_chan_limit_t *l = &cfg->chan_limits[cfg->chan_limit_count];
        snprintf(l->ch, sizeof(l->ch), "%s", trim(line));

        if (parse_u32(trim(p + 1), &l->limit) != 0)
        {
            log_error("config line %d: invalid channel limit", lineno);
            return -1;
        }
        cfg->chan_limit_count++;
    }

    return 0;
}

int config_load(gw_config_t *cfg, const char *path)
{
    FILE *fp;
    char line[LINE_MAX_LEN];
    char section[32] = "";
    int lineno = 0;

    if (cfg == 0 || path == 0)
        return -1;

    memset(cfg, 0, sizeof(*cfg));

    /* MQTT 默认值：出现 addr 即视为启用 */
    snprintf(cfg->mqtt.addr, sizeof(cfg->mqtt.addr), "tcp://127.0.0.1:1883");
    snprintf(cfg->mqtt.topic, sizeof(cfg->mqtt.topic), "gw/can");
    snprintf(cfg->mqtt.client_id, sizeof(cfg->mqtt.client_id), "can-gateway");
    cfg->mqtt.window_ms = 1000;
    cfg->mqtt.enabled = 0;

    fp = fopen(path, "r");
    if (fp == 0)
    {
        log_error("cannot open config %s", path);
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != 0)
    {
        char *s;
        char *eq;

        lineno++;
        s = trim(line);

        if (*s == '\0' || *s == '#')
            continue;

        /* 段头 */
        if (*s == '[')
        {
            char *close = strchr(s, ']');
            if (close == 0)
            {
                log_error("config line %d: unterminated section header", lineno);
                fclose(fp);
                return -1;
            }
            *close = '\0';
            snprintf(section, sizeof(section), "%s", trim(s + 1));
            continue;
        }

        if (strcmp(section, "channels") == 0)
        {
            cfg_channel_t *ch;

            eq = strchr(s, '=');
            if (eq == 0)
            {
                log_error("config line %d: channel needs 'name = ifname'", lineno);
                fclose(fp);
                return -1;
            }
            if (cfg->channel_count >= CFG_MAX_CHANNELS)
            {
                log_error("config line %d: too many channels (max %d)", lineno, CFG_MAX_CHANNELS);
                fclose(fp);
                return -1;
            }

            *eq = '\0';
            ch = &cfg->channels[cfg->channel_count];
            snprintf(ch->name, sizeof(ch->name), "%s", trim(s));

            /* 支持 "a = vcan0, 500000" 指定波特率（用于负载率计算） */
            {
                char *comma = strchr(eq + 1, ',');
                ch->bitrate = 500000;

                if (comma != 0)
                {
                    *comma = '\0';
                    if (parse_u32(trim(comma + 1), &ch->bitrate) != 0 || ch->bitrate == 0)
                    {
                        log_error("config line %d: invalid bitrate", lineno);
                        fclose(fp);
                        return -1;
                    }
                }
                snprintf(ch->ifname, sizeof(ch->ifname), "%s", trim(eq + 1));
            }

            if (ch->name[0] == '\0' || ch->ifname[0] == '\0')
            {
                log_error("config line %d: empty channel name or ifname", lineno);
                fclose(fp);
                return -1;
            }

            cfg->channel_count++;
        }
        else if (strcmp(section, "routes") == 0)
        {
            if (parse_route_line(cfg, s, lineno) != 0)
            {
                fclose(fp);
                return -1;
            }
        }
        else if (strcmp(section, "diag") == 0)
        {
            if (parse_diag_line(cfg, s, lineno) != 0)
            {
                fclose(fp);
                return -1;
            }
        }
        else if (strcmp(section, "mqtt") == 0)
        {
            if (parse_mqtt_line(cfg, s, lineno) != 0)
            {
                fclose(fp);
                return -1;
            }
        }
        else if (strcmp(section, "limits") == 0)
        {
            if (parse_limit_line(cfg, s, lineno) != 0)
            {
                fclose(fp);
                return -1;
            }
        }
        else
        {
            log_warn("config line %d: key outside any known section, ignored", lineno);
        }
    }

    fclose(fp);

    if (cfg->channel_count == 0)
    {
        log_error("config %s defines no channel", path);
        return -1;
    }

    /*
     * 交叉校验：路由与诊断映射引用的通道必须存在。
     * 留到启动时再报错会让人以为配置写对了，实际运行时才发现规则不生效。
     */
    for (int i = 0; i < cfg->route_count; i++)
    {
        if (config_find_channel(cfg, cfg->routes[i].src) < 0 ||
            config_find_channel(cfg, cfg->routes[i].dst) < 0)
        {
            log_error("route %d references unknown channel (%s -> %s)",
                      i, cfg->routes[i].src, cfg->routes[i].dst);
            return -1;
        }
    }

    for (int i = 0; i < cfg->diag_count; i++)
    {
        if (config_find_channel(cfg, cfg->diags[i].tester_ch) < 0 ||
            config_find_channel(cfg, cfg->diags[i].target_ch) < 0)
        {
            log_error("diag map %d references unknown channel (%s -> %s)",
                      i, cfg->diags[i].tester_ch, cfg->diags[i].target_ch);
            return -1;
        }
    }

    for (int i = 0; i < cfg->chan_limit_count; i++)
    {
        if (config_find_channel(cfg, cfg->chan_limits[i].ch) < 0)
        {
            log_error("limit references unknown channel %s", cfg->chan_limits[i].ch);
            return -1;
        }
    }

    for (int i = 0; i < cfg->id_limit_count; i++)
    {
        if (config_find_channel(cfg, cfg->id_limits[i].ch) < 0)
        {
            log_error("id limit references unknown channel %s", cfg->id_limits[i].ch);
            return -1;
        }
    }

    return 0;
}

void config_dump(const gw_config_t *cfg)
{
    if (cfg == 0)
        return;

    if (!cfg->mqtt.enabled)
        log_info("  mqtt  disabled (set enabled = 1 in [mqtt] to turn on)");
    else
    {
        log_info("  mqtt  %s topic=%s window=%u ms channels=%s",
                 cfg->mqtt.addr, cfg->mqtt.topic, cfg->mqtt.window_ms,
                 cfg->mqtt.channels[0] ? cfg->mqtt.channels : "(all)");
    }

    log_info("config: %d channel(s), %d route(s), %d diag map(s), %d limit(s)",
             cfg->channel_count, cfg->route_count, cfg->diag_count,
             cfg->chan_limit_count + cfg->id_limit_count);

    for (int i = 0; i < cfg->channel_count; i++)
        log_info("  channel %s = %s", cfg->channels[i].name, cfg->channels[i].ifname);

    for (int i = 0; i < cfg->route_count; i++)
    {
        const cfg_route_t *r = &cfg->routes[i];
        if (r->id_mask != 0)
            log_info("  route %s (id & 0x%03X) == 0x%03X -> %s (offset %d) %s",
                     r->src, r->id_mask, r->id_min & r->id_mask, r->dst, r->offset, r->note);
        else
            log_info("  route %s 0x%03X-0x%03X -> %s (offset %d) %s",
                     r->src, r->id_min, r->id_max, r->dst, r->offset, r->note);
    }

    for (int i = 0; i < cfg->diag_count; i++)
    {
        const cfg_diag_t *d = &cfg->diags[i];
        log_info("  diag  %s 0x%03X/0x%03X <-> %s 0x%03X/0x%03X",
                 d->tester_ch, d->tester_req, d->tester_rsp,
                 d->target_ch, d->target_req, d->target_rsp);
    }

    for (int i = 0; i < cfg->chan_limit_count; i++)
        log_info("  limit channel %s = %u frame/s",
                 cfg->chan_limits[i].ch, cfg->chan_limits[i].limit);

    for (int i = 0; i < cfg->id_limit_count; i++)
        log_info("  limit id      %s 0x%03X = %u frame/s",
                 cfg->id_limits[i].ch, cfg->id_limits[i].id, cfg->id_limits[i].limit);
}
