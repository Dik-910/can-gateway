#include "rate_limiter.h"

#include <string.h>

#include "log/log.h"

/* 窗口是否已经过去，是则重置计数 */
static void roll_window(uint32_t *count, uint64_t *start, uint64_t now_ms)
{
    if (*start == 0 || now_ms < *start || now_ms - *start >= GW_RATE_WINDOW_MS)
    {
        *count = 0;
        *start = now_ms;
    }
}

void rate_limiter_init(rate_limiter_t *r)
{
    if (r == 0)
        return;
    memset(r, 0, sizeof(*r));
}

void rate_limiter_set_channel(rate_limiter_t *r, uint8_t channel, uint32_t limit)
{
    if (r == 0 || channel >= GW_CHANNEL_MAX)
        return;

    r->channels[channel].limit = limit;
}

int rate_limiter_set_id(rate_limiter_t *r, uint8_t channel, uint32_t id, uint32_t limit)
{
    if (r == 0 || channel >= GW_CHANNEL_MAX)
        return -1;

    /* 已存在就覆盖，便于配置热更新 */
    for (uint16_t i = 0; i < r->id_count; i++)
    {
        if (r->ids[i].channel == channel && r->ids[i].id == id)
        {
            r->ids[i].limit = limit;
            return 0;
        }
    }

    if (r->id_count >= GW_RATE_MAX_IDS)
    {
        log_error("rate table full (%d ids)", GW_RATE_MAX_IDS);
        return -1;
    }

    r->ids[r->id_count].channel = channel;
    r->ids[r->id_count].id = id;
    r->ids[r->id_count].limit = limit;
    r->id_count++;
    return 0;
}

static rate_id_entry_t *find_id_entry(rate_limiter_t *r, uint8_t channel, uint32_t id)
{
    for (uint16_t i = 0; i < r->id_count; i++)
    {
        if (r->ids[i].channel == channel && r->ids[i].id == id)
            return &r->ids[i];
    }
    return 0;
}

int rate_limiter_allow(rate_limiter_t *r, uint8_t channel, uint32_t id, uint64_t now_ms)
{
    rate_channel_t *ch;
    rate_id_entry_t *e;

    if (r == 0 || channel >= GW_CHANNEL_MAX)
        return 0;

    ch = &r->channels[channel];

    /* 第一级：通道总量（防未知 ID 的洪水） */
    if (ch->limit != GW_RATE_UNLIMITED)
    {
        roll_window(&ch->count, &ch->window_start, now_ms);

        if (ch->count >= ch->limit)
        {
            ch->dropped++;
            r->total_dropped++;
            return 0;
        }
    }

    /* 第二级：单 ID（精确保护关键报文） */
    e = find_id_entry(r, channel, id);
    if (e != 0 && e->limit != GW_RATE_UNLIMITED)
    {
        roll_window(&e->count, &e->window_start, now_ms);

        if (e->count >= e->limit)
        {
            e->dropped++;
            r->total_dropped++;
            /* 该 ID 被限，但通道计数已经加过——回退，避免通道额度被无效帧吃掉 */
            if (ch->limit != GW_RATE_UNLIMITED && ch->count > 0)
                ch->count--;
            return 0;
        }

        e->count++;
    }

    if (ch->limit != GW_RATE_UNLIMITED)
        ch->count++;

    r->passed++;
    return 1;
}
