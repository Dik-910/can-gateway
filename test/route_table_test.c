#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/route/route_table.h"

/*
 * 路由表测试。
 *
 * 重点验证三件事：
 *   1. 首次命中即返回（规则顺序有意义）
 *   2. 未命中要计数，这是排障的关键指标
 *   3. ID 改写在配置阶段就拒绝越界，不等运行期
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

static route_rule_t mk_rule(uint8_t src, uint32_t lo, uint32_t hi,
                           uint8_t dst, int32_t offset, const char *note)
{
    route_rule_t r;
    memset(&r, 0, sizeof(r));
    r.src_channel = src;
    r.id_min = lo;
    r.id_max = hi;
    r.dst_channel = dst;
    r.id_offset = offset;
    r.enabled = 1;
    if (note)
        snprintf(r.note, sizeof(r.note), "%s", note);
    return r;
}

/* 构造并添加，避免在调用点写复合字面量取址 */
static int add_rule(route_table_t *t, uint8_t src, uint32_t lo, uint32_t hi,
                    uint8_t dst, int32_t offset, const char *note)
{
    route_rule_t r = mk_rule(src, lo, hi, dst, offset, note);
    return route_table_add(t, &r);
}

static void test_add_and_match(void)
{
    route_table_t t;

    printf("Add and match\n");
    route_table_init(&t);

    check(t.count == 0, "starts empty");

    check(add_rule(&t, 0, 0x180, 0x1FF, 1, 0, "status") == 0,
          "add rule 1");
    check(add_rule(&t, 1, 0x200, 0x2FF, 0, 0, "data") == 0,
          "add rule 2");
    check(t.count == 2, "two rules");

    /* 通道 0 的 0x190 应转发到通道 1 */
    {
        const route_rule_t *r = route_table_match(&t, 0, 0x190);
        check(r != 0, "channel 0 / 0x190 matched");
        check(r && r->dst_channel == 1, "target channel is 1");
        check(t.matched == 1, "matched counter incremented");
    }

    /* 反向 */
    {
        const route_rule_t *r = route_table_match(&t, 1, 0x250);
        check(r != 0 && r->dst_channel == 0, "channel 1 / 0x250 forwards to channel 0");
    }

    /* 不匹配 */
    {
        uint32_t before = t.missed;
        const route_rule_t *r = route_table_match(&t, 0, 0x500);
        check(r == 0, "unmatched id returns NULL");
        check(t.missed == before + 1, "missed counter incremented");
    }

    /* 源通道不匹配 */
    {
        const route_rule_t *r = route_table_match(&t, 1, 0x190);
        check(r == 0, "wrong source channel does not match");
    }
}

static void test_wildcard_and_order(void)
{
    route_table_t t;

    printf("\nWildcard and rule order\n");
    route_table_init(&t);

    /* 先放一条特殊规则，再放一条兜底规则 */
    add_rule(&t, 0, 0x7E0, 0x7E7, 1, 0, "diag special");
    add_rule(&t, GW_CHANNEL_ANY, 0x000, 0x7FF, 1, 0, "catch all");

    {
        const route_rule_t *r = route_table_match(&t, 0, 0x7E0);
        check(r != 0 && strcmp(r->note, "diag special") == 0,
              "specific rule wins over catch-all (first match)");
    }

    {
        const route_rule_t *r = route_table_match(&t, 0, 0x123);
        check(r != 0 && strcmp(r->note, "catch all") == 0, "falls through to catch-all");
    }

    {
        /* ANY 源通道也要能匹配其他通道 */
        const route_rule_t *r = route_table_match(&t, 5, 0x321);
        check(r != 0, "wildcard source matches any channel");
    }
}

static void test_disabled(void)
{
    route_table_t t;
    route_rule_t r;

    printf("\nDisabled rule\n");
    route_table_init(&t);

    r = mk_rule(0, 0x100, 0x1FF, 1, 0, "off");
    r.enabled = 0;
    route_table_add(&t, &r);

    check(route_table_match(&t, 0, 0x150) == 0, "disabled rule is skipped");
}

static void test_id_offset(void)
{
    route_table_t t;
    const route_rule_t *r;
    uint32_t out;

    printf("\nID rewrite\n");
    route_table_init(&t);

    add_rule(&t, 0, 0x100, 0x17F, 1, 0x80, "shift +0x80");
    r = route_table_match(&t, 0, 0x110);
    check(r != 0, "rule with offset matched");
    check(route_target_id(r, 0x110, &out) == 0 && out == 0x190, "0x110 + 0x80 = 0x190");

    /* 越界改写必须在配置阶段被拒绝 */
    check(add_rule(&t, 0, 0x700, 0x7FF, 1, 0x100, "overflow") == -1,
          "offset that overflows 11-bit id is rejected at config time");

    /* 负向偏移也不能越界 */
    check(add_rule(&t, 0, 0x000, 0x010, 1, -0x20, "underflow") == -1,
          "negative offset that goes below 0 is rejected");
}

static void test_validation(void)
{
    route_table_t t;

    printf("\nValidation\n");
    route_table_init(&t);

    check(route_table_add(&t, 0) == -1, "null rule rejected");

    /* id_min > id_max */
    check(add_rule(&t, 0, 0x200, 0x100, 1, 0, "bad range") == -1,
          "inverted range rejected");

    /* 超出 11 位 */
    check(add_rule(&t, 0, 0x100, 0x800, 1, 0, "too big") == -1,
          "id above 0x7FF rejected");

    /* 边界值应通过 */
    check(add_rule(&t, 0, 0x000, 0x7FF, 1, 0, "full range") == 0,
          "0x000-0x7FF accepted");

    /* 表满 */
    {
        int rc = 0;
        for (int i = 0; i < GW_MAX_ROUTES + 2; i++)
            rc = add_rule(&t, 0, 0x100, 0x1FF, 1, 0, "fill");
        check(rc == -1 && t.count == GW_MAX_ROUTES, "table fills up and rejects extra rules");
    }
}

int main(void)
{
    printf("=== route_table test ===\n\n");
    test_add_and_match();
    test_wildcard_and_order();
    test_disabled();
    test_id_offset();
    test_validation();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
