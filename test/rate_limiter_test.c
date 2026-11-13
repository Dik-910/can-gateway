#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/route/rate_limiter.h"

/*
 * 限速器测试。
 *
 * 重点是两级限制的独立性：单个 ID 被限不应该影响同通道的其他 ID，
 * 通道总量被限则应该挡住所有 ID。
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

/* 在 now 时刻连续放行 n 次，返回通过数 */
static int burst(rate_limiter_t *r, uint8_t ch, uint32_t id, int n, uint64_t now)
{
    int ok = 0;
    for (int i = 0; i < n; i++)
        ok += rate_limiter_allow(r, ch, id, now);
    return ok;
}

static void test_unlimited(void)
{
    rate_limiter_t r;

    printf("No limit configured\n");
    rate_limiter_init(&r);

    check(burst(&r, 0, 0x180, 1000, 1000) == 1000, "all frames pass when unlimited");
    check(r.total_dropped == 0, "nothing dropped");
}

static void test_channel_limit(void)
{
    rate_limiter_t r;

    printf("\nChannel limit\n");
    rate_limiter_init(&r);
    rate_limiter_set_channel(&r, 0, 10);

    check(burst(&r, 0, 0x180, 15, 1000) == 10, "channel limit stops at 10");
    check(r.total_dropped == 5, "5 dropped");
    check(r.channels[0].dropped == 5, "per-channel drop counter");

    /* 另一路通道不受影响 */
    check(burst(&r, 1, 0x180, 5, 1000) == 5, "other channel unaffected");

    /* 窗口滚动后恢复额度 */
    check(burst(&r, 0, 0x180, 5, 1000 + GW_RATE_WINDOW_MS) == 5,
          "quota restored in next window");
}

static void test_id_limit(void)
{
    rate_limiter_t r;

    printf("\nPer-ID limit\n");
    rate_limiter_init(&r);
    rate_limiter_set_id(&r, 0, 0x180, 3);

    check(burst(&r, 0, 0x180, 5, 1000) == 3, "id limited to 3");
    check(r.total_dropped == 2, "2 dropped");

    /* 同通道其他 ID 不受影响 */
    check(burst(&r, 0, 0x181, 50, 1000) == 50, "other id on same channel unaffected");

    /* 另一通道的同名 ID 也不受影响 */
    check(burst(&r, 1, 0x180, 5, 1000) == 5, "same id on other channel unaffected");

    /* 窗口滚动恢复 */
    check(burst(&r, 0, 0x180, 3, 1000 + GW_RATE_WINDOW_MS) == 3,
          "id quota restored in next window");
}

static void test_both_limits(void)
{
    rate_limiter_t r;

    printf("\nChannel and ID limits together\n");
    rate_limiter_init(&r);
    rate_limiter_set_channel(&r, 0, 10);
    rate_limiter_set_id(&r, 0, 0x200, 4);

    /* 先打满 ID 限额 */
    check(burst(&r, 0, 0x200, 4, 1000) == 4, "id quota consumed");

    /*
     * 被 ID 限速的帧不应占用通道额度：
     * 接着发 6 帧同 ID（会被 ID 限挡掉），再发 10 帧别的 ID，
     * 通道额度仍应剩 6。
     */
    check(burst(&r, 0, 0x200, 6, 1000) == 0, "id-limited frames rejected");
    check(burst(&r, 0, 0x201, 6, 1000) == 6, "channel quota not consumed by rejected frames");
    check(burst(&r, 0, 0x202, 6, 1000) == 4, "channel quota exhausts correctly");
}

static void test_override(void)
{
    rate_limiter_t r;

    printf("\nOverride\n");
    rate_limiter_init(&r);

    rate_limiter_set_id(&r, 0, 0x180, 2);
    check(burst(&r, 0, 0x180, 3, 1000) == 2, "initial limit applies");

    /* 覆盖为更大值，窗口内应立即可用新额度 */
    rate_limiter_set_id(&r, 0, 0x180, 100);
    check(burst(&r, 0, 0x180, 50, 1000) == 50, "overridden limit takes effect (98 left after 2 used)");
    check(r.id_count == 1, "no duplicate entry created");
}

static void test_params(void)
{
    rate_limiter_t r;

    printf("\nParameter checks\n");
    rate_limiter_init(0); /* 不应崩溃 */

    rate_limiter_init(&r);
    check(rate_limiter_allow(0, 0, 0x180, 1000) == 0, "null limiter rejects");
    check(rate_limiter_allow(&r, GW_CHANNEL_MAX, 0x180, 1000) == 0, "invalid channel rejects");
    check(rate_limiter_set_id(&r, GW_CHANNEL_MAX, 0x180, 10) == -1, "invalid channel rejected on set");
}

int main(void)
{
    printf("=== rate_limiter test ===\n\n");
    test_unlimited();
    test_channel_limit();
    test_id_limit();
    test_both_limits();
    test_override();
    test_params();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
