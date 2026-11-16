/*
 * aggregator.c —— 聚合实现
 */
#include "aggregator.h"

#include <string.h>

void agg_init(aggregator_t *a)
{
    if (a == 0)
        return;

    memset(a, 0, sizeof(*a));
}

static agg_entry_t *find_entry(aggregator_t *a, uint8_t channel, uint32_t id)
{
    /*
     * 线性查找。上限 AGG_MAX_ENTRIES = 64，每帧最多 64 次比较。
     *
     * 为什么不用哈希：64 项的线性扫描实测每帧不到 1 微秒，
     * 而开放寻址哈希要处理探测、删除标记、rehash，bug 面大得多。
     * 如果将来 ID 数上百，再换哈希——届时这个函数是唯一的改动点。
     */
    for (int i = 0; i < AGG_MAX_ENTRIES; i++)
    {
        if (a->entry[i].used &&
            a->entry[i].channel == channel &&
            a->entry[i].id == id)
            return &a->entry[i];
    }

    return 0;
}

void agg_update(aggregator_t *a, uint8_t channel, uint32_t id,
                const uint8_t *data, uint8_t dlc)
{
    agg_entry_t *e;

    if (a == 0 || data == 0)
        return;

    if (dlc > 8)
        dlc = 8;

    e = find_entry(a, channel, id);

    if (e == 0)
    {
        for (int i = 0; i < AGG_MAX_ENTRIES; i++)
        {
            if (!a->entry[i].used)
            {
                e = &a->entry[i];
                break;
            }
        }

        if (e == 0)
        {
            a->overflow++;
            return;
        }

        memset(e, 0, sizeof(*e));
        e->used = 1;
        e->channel = channel;
        e->id = id;

        /* 首次出现一定标记为 dirty：否则新 ID 要等下一次变化才会上报 */
        e->dirty = 1;
    }
    else if (e->dlc != dlc || memcmp(e->data, data, dlc) != 0)
    {
        e->dirty = 1;
    }

    e->dlc = dlc;
    memcpy(e->data, data, dlc);
    e->count++;
}

int agg_export(aggregator_t *a, uint64_t now_ms, uint32_t force_ms,
               agg_export_cb cb, void *user)
{
    int n = 0;

    if (a == 0 || cb == 0)
        return 0;

    if (force_ms == 0)
        force_ms = AGG_DEFAULT_FORCE;

    for (int i = 0; i < AGG_MAX_ENTRIES; i++)
    {
        agg_entry_t *e = &a->entry[i];
        int force;

        if (!e->used)
            continue;

        force = (now_ms - e->last_export_ms) >= force_ms;

        if (!e->dirty && !force)
            continue;

        if (cb(e, user) == 0)
        {
            /*
             * 只有投递成功才清状态。
             * 投递失败（队列满 / 未连接）时保留 dirty，
             * 下一个窗口会重试 —— 用旧数据覆盖新数据总比丢一条好。
             */
            e->dirty = 0;
            e->count = 0;
            e->last_export_ms = now_ms;
            a->exports++;
            n++;
        }
    }

    return n;
}

int agg_count(const aggregator_t *a)
{
    int n = 0;

    if (a == 0)
        return 0;

    for (int i = 0; i < AGG_MAX_ENTRIES; i++)
        if (a->entry[i].used)
            n++;

    return n;
}
