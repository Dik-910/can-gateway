/*
 * aggregator_test.c —— 聚合层
 *
 * 重点验证三件事：
 *   1. 同 (通道, ID) 才算同一条目 —— 不同通道上同一个 ID 是两回事
 *   2. 只在数据变化时导出，否则会变成「每秒重复发同样的值」
 *   3. 长期不变也要强制导出一次，否则云端看板上那个点会冻住
 */
#include <stdio.h>
#include <string.h>
#include "aggregator.h"

static int g_fail;
static int g_total;

static void check(int cond, const char *what)
{
    g_total++;
    if (!cond)
    {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

/* 收集导出的条目 */
static int g_exported;
static agg_entry_t g_last;
static int g_fail_next;   /* 让下一次回调失败，模拟队列满 */

static int collect(const agg_entry_t *e, void *user)
{
    (void)user;

    if (g_fail_next)
    {
        g_fail_next = 0;
        return -1;
    }

    g_exported++;
    g_last = *e;
    return 0;
}

static void reset(aggregator_t *a)
{
    agg_init(a);
    g_exported = 0;
    g_fail_next = 0;
    memset(&g_last, 0, sizeof(g_last));
}

static void test_first_seen(void)
{
    aggregator_t a;
    uint8_t d[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    printf("\nFirst seen is dirty\n");
    reset(&a);

    agg_update(&a, 0, 0x180, d, 8);

    check(agg_count(&a) == 1, "one entry registered");
    check(agg_export(&a, 1000, 30000, collect, 0) == 1, "exported immediately on first sight");
    check(g_last.id == 0x180 && g_last.channel == 0, "exported entry matches");
    check(g_last.dlc == 8 && memcmp(g_last.data, d, 8) == 0, "payload intact");
    check(g_last.count == 1, "count is 1");

    /* 再导一次：没有变化，不应导出 */
    check(agg_export(&a, 2000, 30000, collect, 0) == 0, "unchanged entry not re-exported");
}

static void test_unchanged_updates_count(void)
{
    aggregator_t a;
    uint8_t d[8] = {0};

    printf("\nRepeated same payload only bumps count\n");
    reset(&a);

    for (int i = 0; i < 50; i++)
        agg_update(&a, 0, 0x181, d, 8);

    check(a.entry[0].count == 50, "count accumulated to 50");
    check(agg_export(&a, 1000, 30000, collect, 0) == 1, "exported once");
    check(g_last.count == 50, "exported count is 50");

    /* 导出后 count 清零，下一窗口重新计 */
    agg_update(&a, 0, 0x181, d, 8);
    check(a.entry[0].count == 1, "count reset after export");
    check(agg_export(&a, 2000, 30000, collect, 0) == 0, "same payload not exported again");
}

static void test_change_detected(void)
{
    aggregator_t a;
    uint8_t d1[8] = {0};
    uint8_t d2[8] = {0};

    printf("\nPayload change sets dirty\n");
    reset(&a);

    agg_update(&a, 0, 0x200, d1, 8);
    agg_export(&a, 1000, 30000, collect, 0);

    d2[0] = 0xFF;
    agg_update(&a, 0, 0x200, d2, 8);

    check(agg_export(&a, 2000, 30000, collect, 0) == 1, "changed payload exported");
    check(g_last.data[0] == 0xFF, "new payload carried");

    /* DLC 变化也算变化 */
    agg_update(&a, 0, 0x200, d2, 4);
    check(agg_export(&a, 3000, 30000, collect, 0) == 1, "dlc change exported");
    check(g_last.dlc == 4, "new dlc carried");
}

static void test_channel_separation(void)
{
    aggregator_t a;
    uint8_t d[8] = {7};

    printf("\nSame id on different channels are separate entries\n");
    reset(&a);

    agg_update(&a, 0, 0x180, d, 8);
    agg_update(&a, 1, 0x180, d, 8);

    check(agg_count(&a) == 2, "two entries for two channels");
    check(agg_export(&a, 1000, 30000, collect, 0) == 2, "both exported");

    /* 两条独立计数 */
    agg_update(&a, 0, 0x180, d, 8);
    agg_update(&a, 0, 0x180, d, 8);
    for (int i = 0; i < AGG_MAX_ENTRIES; i++)
    {
        if (a.entry[i].used && a.entry[i].channel == 0)
            check(a.entry[i].count == 2, "channel 0 counted separately");
        if (a.entry[i].used && a.entry[i].channel == 1)
            check(a.entry[i].count == 0, "channel 1 untouched");
    }
}

static void test_force_export(void)
{
    aggregator_t a;
    uint8_t d[8] = {1};

    printf("\nForce export for unchanged entries\n");
    reset(&a);

    agg_update(&a, 0, 0x300, d, 8);
    agg_export(&a, 1000, 5000, collect, 0);
    g_exported = 0;

    check(agg_export(&a, 3000, 5000, collect, 0) == 0, "not exported before force interval");
    check(agg_export(&a, 6001, 5000, collect, 0) == 1, "forced out after interval");
    check(g_last.id == 0x300, "forced entry correct");
}

static void test_export_failure_keeps_dirty(void)
{
    aggregator_t a;
    uint8_t d[8] = {9};

    printf("\nFailed publish keeps entry dirty\n");
    reset(&a);

    agg_update(&a, 0, 0x400, d, 8);

    g_fail_next = 1;
    check(agg_export(&a, 1000, 30000, collect, 0) == 0, "failed export not counted");
    check(a.entry[0].dirty == 1, "entry stays dirty for retry");

    /* 下个窗口重试，这次成功 */
    check(agg_export(&a, 2000, 30000, collect, 0) == 1, "retried and exported");
    check(a.entry[0].dirty == 0, "dirty cleared after success");
}

static void test_overflow(void)
{
    aggregator_t a;
    uint8_t d[8] = {0};

    printf("\nTable full drops new ids\n");
    reset(&a);

    for (int i = 0; i < AGG_MAX_ENTRIES; i++)
        agg_update(&a, 0, (uint32_t)(0x100 + i), d, 8);

    check(agg_count(&a) == AGG_MAX_ENTRIES, "table filled");

    /* 新 ID 被丢弃，且计数 */
    agg_update(&a, 0, 0x7FF, d, 8);
    check(a.overflow == 1, "overflow counted");
    check(agg_count(&a) == AGG_MAX_ENTRIES, "table size unchanged");

    /* 已有 ID 仍能更新 —— 不因为表满就丢老数据 */
    agg_update(&a, 0, 0x100, d, 8);
    check(a.overflow == 1, "existing id does not overflow");
}

int main(void)
{
    printf("=== aggregator_test ===\n");

    test_first_seen();
    test_unchanged_updates_count();
    test_change_detected();
    test_channel_separation();
    test_force_export();
    test_export_failure_keeps_dirty();
    test_overflow();

    printf("\nResult: %d checks, %d failed\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
