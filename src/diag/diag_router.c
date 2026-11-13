#include "diag_router.h"

#include <string.h>

#include "log/log.h"

#define GW_STD_ID_MAX 0x7FFu

void diag_router_init(diag_router_t *d)
{
    if (d == 0)
        return;
    memset(d, 0, sizeof(*d));
}

int diag_router_add(diag_router_t *d, const diag_map_t *m)
{
    if (d == 0 || m == 0)
        return -1;

    if (d->count >= GW_DIAG_MAX_MAPS)
    {
        log_error("diag map table full (%d)", GW_DIAG_MAX_MAPS);
        return -1;
    }

    if (m->tester_req_id > GW_STD_ID_MAX || m->tester_rsp_id > GW_STD_ID_MAX ||
        m->target_req_id > GW_STD_ID_MAX || m->target_rsp_id > GW_STD_ID_MAX)
    {
        log_error("diag map contains id above 0x7FF");
        return -1;
    }

    /*
     * target_rsp_id 必须唯一。
     * 响应帧里只有 CAN ID 可用于定位来源，若两个节点回同一个 ID，
     * 网关无法判断该交给哪个 tester。这是协议层面的约束。
     */
    for (uint16_t i = 0; i < d->count; i++)
    {
        if (d->maps[i].target_channel == m->target_channel &&
            d->maps[i].target_rsp_id == m->target_rsp_id)
        {
            log_error("duplicate target response id 0x%03X on channel %u",
                      m->target_rsp_id, m->target_channel);
            return -1;
        }

        if (d->maps[i].tester_channel == m->tester_channel &&
            d->maps[i].tester_req_id == m->tester_req_id)
        {
            log_error("duplicate tester request id 0x%03X on channel %u",
                      m->tester_req_id, m->tester_channel);
            return -1;
        }
    }

    d->maps[d->count] = *m;
    d->maps[d->count].in_use = 1;
    d->count++;
    return 0;
}

int diag_router_request(diag_router_t *d, uint64_t now_ms, uint8_t src_ch, uint32_t req_id,
                        uint8_t *dst_ch, uint32_t *dst_id)
{
    if (d == 0 || dst_ch == 0 || dst_id == 0)
        return -1;

    for (uint16_t i = 0; i < d->count; i++)
    {
        diag_map_t *m = &d->maps[i];

        if (!m->in_use)
            continue;
        if (m->tester_channel != src_ch || m->tester_req_id != req_id)
            continue;

        m->last_active_ms = now_ms;
        m->frames++;
        d->requests++;

        *dst_ch = m->target_channel;
        *dst_id = m->target_req_id;
        return 0;
    }

    d->unmatched++;
    return -1;
}

int diag_router_response(diag_router_t *d, uint64_t now_ms, uint8_t src_ch, uint32_t rsp_id,
                         uint8_t *dst_ch, uint32_t *dst_id)
{
    if (d == 0 || dst_ch == 0 || dst_id == 0)
        return -1;

    for (uint16_t i = 0; i < d->count; i++)
    {
        diag_map_t *m = &d->maps[i];

        if (!m->in_use)
            continue;
        if (m->target_channel != src_ch || m->target_rsp_id != rsp_id)
            continue;

        m->last_active_ms = now_ms;
        m->frames++;
        d->responses++;

        *dst_ch = m->tester_channel;
        *dst_id = m->tester_rsp_id;
        return 0;
    }

    d->unmatched++;
    return -1;
}

uint16_t diag_router_active(diag_router_t *d, uint64_t now_ms)
{
    uint16_t active = 0;

    if (d == 0)
        return 0;

    for (uint16_t i = 0; i < d->count; i++)
    {
        diag_map_t *m = &d->maps[i];

        if (!m->in_use)
            continue;

        /*
         * 超时只影响「活跃」统计，不删除映射。
         * 映射是配置，删了就得靠 tester 重连重建，反而更麻烦。
         */
        if (now_ms > m->last_active_ms + GW_DIAG_TIMEOUT_MS)
        {
            m->idle_events++;
            continue;
        }

        if (m->last_active_ms != 0)
            active++;
    }

    return active;
}
