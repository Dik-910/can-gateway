#include "diag_router.h"

#include <string.h>

#include "log/log.h"

#define GW_STD_ID_MAX 0x7FFu

void diag_router_init(diag_router_t *d)
{
    if (d == 0)
        return;
    memset(d, 0, sizeof(*d));
    d->p2_ms = GW_DIAG_P2_MS;
    d->p2ext_ms = GW_DIAG_P2EXT_MS;
    d->retry_max = GW_DIAG_RETRY_MAX;
}

void diag_router_set_timing(diag_router_t *d, uint32_t p2_ms, uint32_t p2ext_ms, uint8_t retry_max)
{
    if (d == 0)
        return;

    if (p2_ms != 0)
        d->p2_ms = p2_ms;
    if (p2ext_ms != 0)
        d->p2ext_ms = p2ext_ms;

    d->retry_max = retry_max;
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

/* 按节点侧 (通道, 响应 ID) 定位映射；响应方向唯一 */
static diag_map_t *map_by_response(diag_router_t *d, uint8_t node_ch, uint32_t rsp_id)
{
    for (uint16_t i = 0; i < d->count; i++)
    {
        diag_map_t *m = &d->maps[i];

        if (m->in_use && m->target_channel == node_ch && m->target_rsp_id == rsp_id)
            return m;
    }

    return 0;
}

int diag_router_track(diag_router_t *d, uint8_t node_ch, uint32_t req_id, const uint8_t *data,
                      uint8_t dlc, uint64_t now_ms)
{
    if (d == 0 || data == 0 || dlc == 0 || dlc > 8)
        return -1;

    for (uint16_t i = 0; i < d->count; i++)
    {
        diag_map_t *m = &d->maps[i];

        if (!m->in_use || m->target_channel != node_ch || m->target_req_id != req_id)
            continue;

        /*
         * 上一条还没回应就又来了新请求：tester 自己超时重发。
         * 旧的那条按超时计，避免长期静默丢失被计入成功。
         */
        if (m->pending.in_use)
            d->timeouts++;

        memset(&m->pending, 0, sizeof(m->pending));
        m->pending.in_use = 1;
        m->pending.node_ch = node_ch;
        m->pending.req_id = req_id;
        m->pending.dlc = dlc;
        m->pending.deadline_ms = now_ms + d->p2_ms;
        memcpy(m->pending.data, data, dlc);
        d->tracked++;
        return 0;
    }

    return -1;
}

int diag_router_answered(diag_router_t *d, uint8_t node_ch, uint32_t rsp_id)
{
    diag_map_t *m;

    if (d == 0)
        return -1;

    m = map_by_response(d, node_ch, rsp_id);
    if (m == 0 || !m->pending.in_use)
        return -1;

    m->pending.in_use = 0;
    d->answered++;
    return 0;
}

int diag_router_extend(diag_router_t *d, uint8_t node_ch, uint32_t rsp_id, uint64_t now_ms)
{
    diag_map_t *m;

    if (d == 0)
        return -1;

    m = map_by_response(d, node_ch, rsp_id);
    if (m == 0 || !m->pending.in_use)
        return -1;

    m->pending.deadline_ms = now_ms + d->p2ext_ms;
    m->pending.extended = 1;
    d->pending_ext++;
    return 0;
}

uint8_t diag_router_service(diag_router_t *d, uint64_t now_ms, diag_retry_t *out, uint8_t max)
{
    uint8_t n = 0;

    if (d == 0 || out == 0)
        return 0;

    for (uint16_t i = 0; i < d->count; i++)
    {
        diag_map_t *m = &d->maps[i];
        diag_pending_t *p = &m->pending;

        if (!p->in_use || now_ms < p->deadline_ms)
            continue;

        /*
         * 只有单帧请求可以重传。多帧请求的后续帧依赖接收方的流控和序号状态，
         * 单独重发某一帧会破坏 ISO-TP 的会话，宁可判超时交给 tester 重来。
         */
        int single_frame = (p->data[0] & 0xF0u) == 0x00u;

        if (single_frame && !p->extended && p->retries < d->retry_max)
        {
            /* 输出缓冲已满：本条留到下一轮，不能因为写不下就当成超时 */
            if (n >= max)
                continue;

            p->retries++;
            p->deadline_ms = now_ms + d->p2_ms;
            d->retries++;

            out[n].node_ch = p->node_ch;
            out[n].dlc = p->dlc;
            out[n].req_id = p->req_id;
            memcpy(out[n].data, p->data, p->dlc);
            n++;
            continue;
        }

        p->in_use = 0;
        d->timeouts++;
    }

    return n;
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
