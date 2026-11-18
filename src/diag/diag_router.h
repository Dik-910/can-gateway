#ifndef __DIAG_ROUTER_H
#define __DIAG_ROUTER_H

#include <stdint.h>

/*
 * 诊断路由：处理多个 tester 同时诊断多个节点时的 ID 冲突。
 *
 * 问题：
 *   按 ISO 14229，节点的诊断请求 ID 是 0x7E0-0x7E7，响应固定 0x7E8-0x7EF。
 *   当两个 tester 分别诊断两个节点时，两个节点都会回 0x7E8，
 *   网关无法判断这个 0x7E8 该交给谁。
 *
 * 解法：
 *   网关维护映射表，把 tester 侧的 ID 与节点侧的 ID 解耦：
 *
 *     tester1  0x7E0  ──┐                  ┌── 节点1  0x7E0 / 回 0x7E8
 *                        ├── 网关映射表 ────┤
 *     tester2  0x7E1  ──┘                  └── 节点2  0x7E2 / 回 0x7EA
 *
 *   请求方向：查 tester 侧 → 改写成节点侧 ID
 *   响应方向：查节点侧 → 改写成 tester 侧 ID
 *
 * 约束（重要）：
 *   target_rsp_id 必须在表内唯一。因为响应帧里只有 ID 能用来定位来源，
 *   如果两个节点都回 0x7E8，网关无从区分——这是协议决定的，不是实现缺陷。
 *   真实项目里要求每个节点的诊断 ID 段错开，或由 tester 侧保证不并发。
 */

#define GW_DIAG_MAX_MAPS   16
#define GW_DIAG_TIMEOUT_MS 5000 /* 超过该时间无活动视为会话空闲 */

/*
 * 会话监督的时间参数。
 *
 * 网关不是 tester，不能用 UDS 的 P2_server = 50 ms 作为判超时的依据：
 * 那只是节点自己的响应时间指标，没有算上报文往返和节点侧排队。
 * 窗口取 150 ms，比 P2 宽一档；再多就会晚于 tester 自己的超时，
 * 重传也就失去意义。
 *
 * 节点回 0x7F xx 78（responsePending）表示仍在处理，此时按 P2* 延长，
 * 不重传、不判超时。
 */
#define GW_DIAG_P2_MS      150u
#define GW_DIAG_P2EXT_MS   2000u
#define GW_DIAG_RETRY_MAX  1u

typedef struct
{
    uint8_t in_use;
    uint8_t retries;     /* 已重传次数 */
    uint8_t extended;    /* 收到过 0x78，此后不再重传 */
    uint8_t node_ch;     /* 重传时的目标通道 */
    uint8_t dlc;
    uint32_t req_id;     /* 已改写的节点侧请求 ID */
    uint8_t data[8];
    uint64_t deadline_ms;
} diag_pending_t;

/* 需要重传的帧，交给调用方重新发送 */
typedef struct
{
    uint8_t node_ch;
    uint8_t dlc;
    uint32_t req_id;
    uint8_t data[8];
} diag_retry_t;

typedef struct
{
    uint8_t in_use;

    /* tester 侧 */
    uint8_t tester_channel;
    uint32_t tester_req_id; /* tester 发请求用的 ID，如 0x7E1 */
    uint32_t tester_rsp_id; /* tester 期望收到的响应 ID，如 0x7E9 */

    /* 节点侧 */
    uint8_t target_channel;
    uint32_t target_req_id; /* 转发给节点的请求 ID，如 0x7E0 */
    uint32_t target_rsp_id; /* 节点回响应用的 ID，如 0x7E8 */

    /* 运行期统计 */
    uint64_t last_active_ms;
    uint32_t frames;
    uint32_t idle_events; /* 被判为空闲的次数 */

    /*
     * 每个映射同一时刻只有一条未回应的请求 —— UDS 的会话本身就是串行的，
     * 节点在处理完当前请求前不会接受下一条（0x78 除外，见 pending_ext）。
     */
    diag_pending_t pending;
} diag_map_t;

typedef struct
{
    diag_map_t maps[GW_DIAG_MAX_MAPS];
    uint16_t count;

    uint32_t requests;   /* 成功路由的请求数 */
    uint32_t responses;  /* 成功路由的响应数 */
    uint32_t unmatched;  /* 落在诊断 ID 段但找不到映射的帧 */

    /* 会话监督 */
    uint32_t tracked;     /* 纳入监督的请求数 */
    uint32_t answered;    /* 在窗口内收到响应的次数 */
    uint32_t timeouts;    /* 重传次数用尽仍无响应 */
    uint32_t retries;     /* 实际发出的重传次数 */
    uint32_t pending_ext; /* 收到 0x78 而延长的次数 */
    uint32_t p2_ms;
    uint32_t p2ext_ms;
    uint8_t retry_max;
} diag_router_t;

void diag_router_init(diag_router_t *d);

/*
 * 调整监督参数。p2_ms / p2ext_ms 传 0 表示保持默认，retry_max 传 0 表示
 * 只监督不重传。上电时由 diag_router_init 填入默认值。
 */
void diag_router_set_timing(diag_router_t *d, uint32_t p2_ms, uint32_t p2ext_ms, uint8_t retry_max);

/* 添加映射。ID 非法、target_rsp_id 重复、表满都会返回 -1 */
int diag_router_add(diag_router_t *d, const diag_map_t *m);

/*
 * 请求方向：tester 发的请求 → 节点的请求 ID。
 * 命中返回 0 并通过 dst_ch / dst_id 输出目标；未命中返回 -1。
 */
int diag_router_request(diag_router_t *d, uint64_t now_ms, uint8_t src_ch, uint32_t req_id,
                        uint8_t *dst_ch, uint32_t *dst_id);

/*
 * 响应方向：节点回的响应 → tester 的响应 ID。
 * 命中返回 0；未命中返回 -1。
 */
int diag_router_response(diag_router_t *d, uint64_t now_ms, uint8_t src_ch, uint32_t rsp_id,
                         uint8_t *dst_ch, uint32_t *dst_id);

/*
 * 请求转发成功后登记一条待响应项。data/dlc 是改写后的完整帧，用于重传。
 * 返回 0 表示已登记。
 */
int diag_router_track(diag_router_t *d, uint8_t node_ch, uint32_t req_id, const uint8_t *data,
                      uint8_t dlc, uint64_t now_ms);

/* 响应到达，清除待响应项。node_ch + rsp_id 用于定位映射。清掉返回 0 */
int diag_router_answered(diag_router_t *d, uint8_t node_ch, uint32_t rsp_id);

/* 响应为 0x7F xx 78（responsePending）：按 P2* 延长截止时刻，并停用重传 */
int diag_router_extend(diag_router_t *d, uint8_t node_ch, uint32_t rsp_id, uint64_t now_ms);

/*
 * 超时扫描。需要重传的条目写入 out[0..max)，返回写入条数。
 * 重传次数用尽且仍无响应的条目计为 timeout 并释放。
 */
uint8_t diag_router_service(diag_router_t *d, uint64_t now_ms, diag_retry_t *out, uint8_t max);

/* 当前活跃会话数（用于监控），同时把超时的映射标记为空闲 */
uint16_t diag_router_active(diag_router_t *d, uint64_t now_ms);

#endif
