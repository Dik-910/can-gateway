#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/diag/diag_router.h"

/*
 * 诊断请求的会话监督测试。
 *
 * 关注三件事：
 *   1. 转发之后有登记，响应到达能结算，超时能发现
 *   2. 重传只做一次，且只对单帧请求做
 *   3. 节点回 0x7F xx 78 时延长窗口而不是判超时
 */

static int passed = 0;
static int failed = 0;

static void check(int cond, const char *what)
{
    if (cond)
    {
        printf("  [PASS] %s\n", what);
        passed++;
    }
    else
    {
        printf("  [FAIL] %s\n", what);
        failed++;
    }
}

static int add_map(diag_router_t *d, uint8_t t_ch, uint32_t t_req, uint32_t t_rsp,
                   uint8_t n_ch, uint32_t n_req, uint32_t n_rsp)
{
    diag_map_t m;

    memset(&m, 0, sizeof(m));
    m.tester_channel = t_ch;
    m.tester_req_id = t_req;
    m.tester_rsp_id = t_rsp;
    m.target_channel = n_ch;
    m.target_req_id = n_req;
    m.target_rsp_id = n_rsp;
    return diag_router_add(d, &m);
}

/* 单帧请求：0x02 10 03 —— 进入扩展会话 */
static const uint8_t REQ_SF[8] = { 0x02, 0x10, 0x03, 0, 0, 0, 0, 0 };

static void test_defaults(void)
{
    diag_router_t d;

    printf("Default timing\n");
    diag_router_init(&d);

    check(d.p2_ms == GW_DIAG_P2_MS, "p2 default");
    check(d.p2ext_ms == GW_DIAG_P2EXT_MS, "p2ext default");
    check(d.retry_max == GW_DIAG_RETRY_MAX, "retry_max default");
    check(d.retry_max == 1, "default retransmits exactly once");
    check(d.timeouts == 0 && d.retries == 0 && d.tracked == 0, "counters start at zero");

    /* 0 表示保持原值 */
    diag_router_set_timing(&d, 0, 0, 0);
    check(d.p2_ms == GW_DIAG_P2_MS, "zero leaves p2 unchanged");
    check(d.p2ext_ms == GW_DIAG_P2EXT_MS, "zero leaves p2ext unchanged");
    check(d.retry_max == 0, "zero disables retransmission");

    diag_router_set_timing(&d, 40, 900, 2);
    check(d.p2_ms == 40 && d.p2ext_ms == 900 && d.retry_max == 2, "override accepted");
}

static void test_track_and_answer(void)
{
    diag_router_t d;

    printf("Track and answer\n");
    diag_router_init(&d);
    check(add_map(&d, 0, 0x7E1, 0x7E9, 1, 0x7E0, 0x7E8) == 0, "map added");

    check(diag_router_track(&d, 1, 0x7E0, REQ_SF, 8, 1000) == 0, "track accepted");
    check(d.tracked == 1, "tracked counted");
    check(d.maps[0].pending.in_use == 1, "pending slot in use");
    check(d.maps[0].pending.deadline_ms == 1000 + GW_DIAG_P2_MS, "deadline is now + p2");
    check(d.maps[0].pending.dlc == 8, "dlc stored for retransmission");
    check(memcmp(d.maps[0].pending.data, REQ_SF, 8) == 0, "payload stored verbatim");

    /* 窗口内不判超时、不重传 */
    diag_retry_t out[4];
    check(diag_router_service(&d, 1000 + GW_DIAG_P2_MS - 1, out, 4) == 0, "no action before deadline");
    check(d.timeouts == 0 && d.retries == 0, "counters untouched before deadline");

    /* 响应到达，结算成功 */
    check(diag_router_answered(&d, 1, 0x7E8) == 0, "answer settles the request");
    check(d.answered == 1, "answered counted");
    check(d.maps[0].pending.in_use == 0, "pending slot released");

    /* 再结算一次没有待响应项 */
    check(diag_router_answered(&d, 1, 0x7E8) != 0, "second answer is not counted");
    check(d.answered == 1, "answered not incremented twice");

    /* 未知映射的响应不结算 */
    check(diag_router_answered(&d, 1, 0x7E7) != 0, "unknown response id ignored");

    /* 参数校验 */
    check(diag_router_track(&d, 1, 0x7E0, 0, 8, 1000) != 0, "null payload rejected");
    check(diag_router_track(&d, 1, 0x7E0, REQ_SF, 0, 1000) != 0, "zero dlc rejected");
    check(diag_router_track(&d, 1, 0x7E0, REQ_SF, 9, 1000) != 0, "dlc above 8 rejected");
    check(diag_router_track(&d, 1, 0x7E5, REQ_SF, 8, 1000) != 0, "unknown request id rejected");
}

static void test_retransmit_once(void)
{
    diag_router_t d;
    diag_retry_t out[4];

    printf("Retransmit once, then give up\n");
    diag_router_init(&d);
    add_map(&d, 0, 0x7E1, 0x7E9, 1, 0x7E0, 0x7E8);
    diag_router_track(&d, 1, 0x7E0, REQ_SF, 8, 1000);

    uint64_t t = 1000 + GW_DIAG_P2_MS;
    check(diag_router_service(&d, t, out, 4) == 1, "deadline produces one retry");
    check(d.retries == 1, "retry counted");
    check(out[0].node_ch == 1, "retry keeps target channel");
    check(out[0].req_id == 0x7E0, "retry keeps node-side request id");
    check(out[0].dlc == 8 && memcmp(out[0].data, REQ_SF, 8) == 0, "retry payload identical");
    check(d.maps[0].pending.in_use == 1, "still pending after retry");

    /* 重传后窗口重新计算，不应立刻再触发 */
    check(diag_router_service(&d, t + 10, out, 4) == 0, "no immediate second retry");

    /* 第二次到期：重传次数用尽，判超时 */
    check(diag_router_service(&d, t + GW_DIAG_P2_MS, out, 4) == 0, "second deadline retries nothing");
    check(d.timeouts == 1, "timeout counted after retries exhausted");
    check(d.maps[0].pending.in_use == 0, "pending slot released on timeout");

    /* 超时之后迟到的响应不再算 answered */
    check(diag_router_answered(&d, 1, 0x7E8) != 0, "late response is not counted");
    check(d.answered == 0, "answered stays zero");
}

static void test_retry_disabled(void)
{
    diag_router_t d;
    diag_retry_t out[4];

    printf("Supervise without retransmission\n");
    diag_router_init(&d);
    diag_router_set_timing(&d, 0, 0, 0);
    add_map(&d, 0, 0x7E1, 0x7E9, 1, 0x7E0, 0x7E8);
    diag_router_track(&d, 1, 0x7E0, REQ_SF, 8, 1000);

    check(diag_router_service(&d, 1000 + GW_DIAG_P2_MS, out, 4) == 0, "no retry when disabled");
    check(d.timeouts == 1, "timeout still counted");
    check(d.retries == 0, "retry counter untouched");
}

static void test_multi_frame_not_retried(void)
{
    diag_router_t d;
    diag_retry_t out[4];
    /* 首帧：0x10 0x0E ... 表示 14 字节后续 */
    static const uint8_t req_ff[8] = { 0x10, 0x0E, 0x2E, 0xF1, 0x90, 1, 2, 3 };

    printf("Multi-frame request is supervised but not retried\n");
    diag_router_init(&d);
    add_map(&d, 0, 0x7E1, 0x7E9, 1, 0x7E0, 0x7E8);
    diag_router_track(&d, 1, 0x7E0, req_ff, 8, 1000);

    check(diag_router_service(&d, 1000 + GW_DIAG_P2_MS, out, 4) == 0, "first frame not retransmitted");
    check(d.timeouts == 1, "counted as timeout instead");
    check(d.retries == 0, "retry counter untouched");
}

static void test_response_pending(void)
{
    diag_router_t d;
    diag_retry_t out[4];

    printf("0x78 extends the window and stops retransmission\n");
    diag_router_init(&d);
    add_map(&d, 0, 0x7E1, 0x7E9, 1, 0x7E0, 0x7E8);
    diag_router_track(&d, 1, 0x7E0, REQ_SF, 8, 1000);

    check(diag_router_extend(&d, 1, 0x7E8, 1010) == 0, "extend accepted");
    check(d.pending_ext == 1, "extension counted");
    check(d.maps[0].pending.deadline_ms == 1010 + GW_DIAG_P2EXT_MS, "deadline pushed to p2ext");
    check(d.maps[0].pending.extended == 1, "marked as extended");

    /* 原本的 p2 早已过去，但延长后不应重传 */
    check(diag_router_service(&d, 1000 + GW_DIAG_P2_MS + 50, out, 4) == 0,
          "no retry while node reports pending");
    check(d.retries == 0 && d.timeouts == 0, "neither retry nor timeout");

    /* 延长窗口也过了：判超时，但仍不重传 */
    check(diag_router_service(&d, 1010 + GW_DIAG_P2EXT_MS, out, 4) == 0,
          "extended request is not retransmitted");
    check(d.timeouts == 1, "eventually counted as timeout");
    check(d.retries == 0, "still no retry");

    /* 未知映射上的 0x78 不改变任何状态 */
    check(diag_router_extend(&d, 1, 0x7E7, 2000) != 0, "unknown extend rejected");
    check(d.pending_ext == 1, "extension counter unchanged");
}

static void test_response_pending_then_answer(void)
{
    diag_router_t d;

    printf("Answer after extension\n");
    diag_router_init(&d);
    add_map(&d, 0, 0x7E1, 0x7E9, 1, 0x7E0, 0x7E8);
    diag_router_track(&d, 1, 0x7E0, REQ_SF, 8, 1000);

    diag_router_extend(&d, 1, 0x7E8, 1010);
    check(diag_router_answered(&d, 1, 0x7E8) == 0, "answer after extension settles");
    check(d.answered == 1, "answered counted");
    check(d.timeouts == 0, "no timeout recorded");
}

static void test_superseded_request(void)
{
    diag_router_t d;
    diag_retry_t out[4];

    printf("New request supersedes an unanswered one\n");
    diag_router_init(&d);
    add_map(&d, 0, 0x7E1, 0x7E9, 1, 0x7E0, 0x7E8);

    diag_router_track(&d, 1, 0x7E0, REQ_SF, 8, 1000);
    check(diag_router_track(&d, 1, 0x7E0, REQ_SF, 8, 1200) == 0, "second request accepted");
    check(d.timeouts == 1, "superseded request counted as timeout");
    check(d.maps[0].pending.deadline_ms == 1200 + GW_DIAG_P2_MS, "deadline follows new request");
    check(d.maps[0].pending.retries == 0, "retry count reset for the new request");

    check(diag_router_service(&d, 1200 + GW_DIAG_P2_MS, out, 4) == 1, "new request retried on its own schedule");
}

static void test_two_maps_independent(void)
{
    diag_router_t d;
    diag_retry_t out[4];

    printf("Two sessions supervised independently\n");
    diag_router_init(&d);
    add_map(&d, 0, 0x7E1, 0x7E9, 1, 0x7E0, 0x7E8);
    add_map(&d, 1, 0x7E2, 0x7EA, 0, 0x7E3, 0x7EB);

    diag_router_track(&d, 1, 0x7E0, REQ_SF, 8, 1000);
    diag_router_track(&d, 0, 0x7E3, REQ_SF, 8, 1100);

    /* 只有第一条到期 */
    check(diag_router_service(&d, 1000 + GW_DIAG_P2_MS, out, 4) == 1, "only the due session retried");
    check(out[0].req_id == 0x7E0, "the due session is the first one");
    check(d.maps[1].pending.retries == 0, "second session untouched");

    /* 第一条结算，第二条不受影响 */
    check(diag_router_answered(&d, 1, 0x7E8) == 0, "first session settles");
    check(d.maps[1].pending.in_use == 1, "second session still pending");
    check(d.answered == 1, "answered counted once");
}

static void test_output_capacity(void)
{
    diag_router_t d;
    diag_retry_t out[1];

    printf("Output buffer limit\n");
    diag_router_init(&d);
    add_map(&d, 0, 0x7E1, 0x7E9, 1, 0x7E0, 0x7E8);
    add_map(&d, 1, 0x7E2, 0x7EA, 0, 0x7E3, 0x7EB);

    diag_router_track(&d, 1, 0x7E0, REQ_SF, 8, 1000);
    diag_router_track(&d, 0, 0x7E3, REQ_SF, 8, 1000);

    /* 只有一个输出槽：另一条本轮不动，也不应被误判超时 */
    check(diag_router_service(&d, 1000 + GW_DIAG_P2_MS, out, 1) == 1, "one retry fits the buffer");
    check(d.retries == 1, "only one retransmission counted");
    check(d.timeouts == 0, "the deferred session is not timed out");

    /* 下一轮把剩下的取走 */
    check(diag_router_service(&d, 1000 + GW_DIAG_P2_MS, out, 1) == 1, "remaining session retried next round");
    check(d.retries == 2, "both retransmissions counted");
}

static void test_null_args(void)
{
    diag_router_t d;
    diag_retry_t out[2];
    uint64_t now = 1000;

    printf("Null and empty handling\n");
    diag_router_init(&d);

    check(diag_router_track(0, 1, 0x7E0, REQ_SF, 8, now) != 0, "track on null router rejected");
    check(diag_router_answered(0, 1, 0x7E8) != 0, "answer on null router rejected");
    check(diag_router_extend(0, 1, 0x7E8, now) != 0, "extend on null router rejected");
    check(diag_router_service(0, now, out, 2) == 0, "service on null router returns zero");
    check(diag_router_service(&d, now, 0, 2) == 0, "service without output buffer returns zero");

    /* 表为空时一切为 0，不崩溃 */
    check(diag_router_service(&d, now, out, 2) == 0, "empty table has nothing to do");
    check(diag_router_answered(&d, 1, 0x7E8) != 0, "answer on empty table rejected");
}

int main(void)
{
    printf("=== diag_timeout test ===\n\n");
    test_defaults();
    test_track_and_answer();
    test_retransmit_once();
    test_retry_disabled();
    test_multi_frame_not_retried();
    test_response_pending();
    test_response_pending_then_answer();
    test_superseded_request();
    test_two_maps_independent();
    test_output_capacity();
    test_null_args();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
