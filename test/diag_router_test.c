#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/diag/diag_router.h"

/*
 * 诊断会话映射测试。
 *
 * 核心场景：两个 tester 分别诊断两个节点，节点都回各自的响应 ID，
 * 网关要把每一帧送回正确的 tester。
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

static diag_map_t mk_map(uint8_t t_ch, uint32_t t_req, uint32_t t_rsp,
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
    return m;
}

static int add_map(diag_router_t *d, uint8_t t_ch, uint32_t t_req, uint32_t t_rsp,
                   uint8_t n_ch, uint32_t n_req, uint32_t n_rsp)
{
    diag_map_t m = mk_map(t_ch, t_req, t_rsp, n_ch, n_req, n_rsp);
    return diag_router_add(d, &m);
}

/* 双 tester / 双节点的完整往返 */
static void test_two_testers(void)
{
    diag_router_t d;
    uint8_t dst_ch;
    uint32_t dst_id;

    printf("Two testers, two nodes\n");
    diag_router_init(&d);

    /* tester1 在通道 0 用 0x7E0，节点1 在通道 1 用 0x7E0/0x7E8 */
    check(add_map(&d, 0, 0x7E0, 0x7E8, 1, 0x7E0, 0x7E8) == 0, "map tester1 -> node1");
    /* tester2 在通道 0 用 0x7E1，节点2 在通道 1 用 0x7E0/0x7EA */
    check(add_map(&d, 0, 0x7E1, 0x7E9, 1, 0x7E0, 0x7EA) == 0, "map tester2 -> node2");

    /* tester1 发请求 */
    check(diag_router_request(&d, 1000, 0, 0x7E0, &dst_ch, &dst_id) == 0, "tester1 request matched");
    check(dst_ch == 1 && dst_id == 0x7E0, "tester1 request -> channel 1 / 0x7E0");

    /* tester2 发请求：同为通道 1，但 ID 不同 */
    check(diag_router_request(&d, 1000, 0, 0x7E1, &dst_ch, &dst_id) == 0, "tester2 request matched");
    check(dst_ch == 1 && dst_id == 0x7E0, "tester2 request -> channel 1 / 0x7E0 (rewritten)");

    /* 节点1 回响应 0x7E8 -> 回给 tester1 的 0x7E8 */
    check(diag_router_response(&d, 1001, 1, 0x7E8, &dst_ch, &dst_id) == 0, "node1 response matched");
    check(dst_ch == 0 && dst_id == 0x7E8, "node1 response -> tester1 on 0x7E8");

    /* 节点2 回响应 0x7EA -> 回给 tester2 的 0x7E9 */
    check(diag_router_response(&d, 1001, 1, 0x7EA, &dst_ch, &dst_id) == 0, "node2 response matched");
    check(dst_ch == 0 && dst_id == 0x7E9, "node2 response -> tester2 on 0x7E9");

    check(d.requests == 2 && d.responses == 2, "counters correct");
}

/* 未命中 */
static void test_unmatched(void)
{
    diag_router_t d;
    uint8_t dst_ch;
    uint32_t dst_id;

    printf("\nUnmatched frames\n");
    diag_router_init(&d);
    add_map(&d, 0, 0x7E0, 0x7E8, 1, 0x7E0, 0x7E8);

    check(diag_router_request(&d, 1000, 0, 0x7E5, &dst_ch, &dst_id) == -1,
          "unknown tester request id rejected");
    check(diag_router_response(&d, 1000, 1, 0x7EF, &dst_ch, &dst_id) == -1,
          "unknown node response id rejected");
    check(diag_router_request(&d, 1000, 2, 0x7E0, &dst_ch, &dst_id) == -1,
          "wrong source channel rejected");
    check(d.unmatched == 3, "unmatched counter tracks misses");
}

/* 冲突检测：同一节点通道上两个映射用同一个响应 ID，网关无法区分 */
static void test_conflict(void)
{
    diag_router_t d;

    printf("\nConflict detection\n");
    diag_router_init(&d);

    check(add_map(&d, 0, 0x7E0, 0x7E8, 1, 0x7E0, 0x7E8) == 0, "first map accepted");

    /* 同一个 target_rsp_id 再来一次 —— 必须拒绝 */
    check(add_map(&d, 0, 0x7E1, 0x7E9, 1, 0x7E0, 0x7E8) == -1,
          "duplicate target response id rejected");

    /* tester 侧请求 ID 也不能重复 */
    check(add_map(&d, 0, 0x7E0, 0x7EA, 1, 0x7E2, 0x7EA) == -1,
          "duplicate tester request id rejected");

    check(d.count == 1, "only the valid map is stored");
}

/* 参数校验 */
static void test_validation(void)
{
    diag_router_t d;

    printf("\nValidation\n");
    diag_router_init(&d);

    check(diag_router_add(&d, 0) == -1, "null map rejected");
    check(add_map(&d, 0, 0x800, 0x801, 1, 0x7E0, 0x7E8) == -1, "id above 0x7FF rejected");

    /* 表满 */
    {
        int rc = 0;
        for (int i = 0; i < GW_DIAG_MAX_MAPS + 2; i++)
            rc = add_map(&d, 0, 0x7E0 + (uint32_t)i, 0x7E8 + (uint32_t)i,
                         1, 0x7E0, 0x7E8 + (uint32_t)i);
        check(rc == -1 && d.count == GW_DIAG_MAX_MAPS, "table fills up");
    }
}

/* 活跃统计与超时 */
static void test_activity(void)
{
    diag_router_t d;
    uint8_t dst_ch;
    uint32_t dst_id;

    printf("\nActivity and timeout\n");
    diag_router_init(&d);
    add_map(&d, 0, 0x7E0, 0x7E8, 1, 0x7E0, 0x7E8);
    add_map(&d, 0, 0x7E1, 0x7E9, 1, 0x7E2, 0x7EA);

    check(diag_router_active(&d, 1000) == 0, "no active session before any traffic");

    diag_router_request(&d, 1000, 0, 0x7E0, &dst_ch, &dst_id);
    check(diag_router_active(&d, 1500) == 1, "one active session after request");

    diag_router_request(&d, 2000, 0, 0x7E1, &dst_ch, &dst_id);
    check(diag_router_active(&d, 2500) == 2, "two active sessions");

    /* 时间推进到超过超时窗口，两个都应变空闲 */
    check(diag_router_active(&d, 2000 + GW_DIAG_TIMEOUT_MS + 1000) == 0,
          "sessions go idle after timeout");

    /* 映射本身仍然保留，再来一帧还能命中 */
    check(diag_router_request(&d, 20000, 0, 0x7E0, &dst_ch, &dst_id) == 0,
          "map still usable after idle");
}

int main(void)
{
    printf("=== diag_router test ===\n\n");
    test_two_testers();
    test_unmatched();
    test_conflict();
    test_validation();
    test_activity();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
