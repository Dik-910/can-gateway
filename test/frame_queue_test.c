#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/util/frame_queue.h"

/*
 * 环形帧队列测试。
 *
 * 除了常规的入队出队，重点验证两个边界：
 *   1. 环形回绕后顺序仍然正确（head 回到 0 之后的数据不能错位）
 *   2. 队满时丢弃并计数，而不是覆盖旧数据或阻塞
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

static gw_frame_t mk_frame(uint32_t id, uint8_t dlc, uint8_t seed)
{
    gw_frame_t f;
    memset(&f, 0, sizeof(f));
    f.id = id;
    f.dlc = dlc;
    for (uint8_t i = 0; i < dlc; i++)
        f.data[i] = (uint8_t)(seed + i);
    f.timestamp_us = 1000u + seed;
    return f;
}

/* 构造并入队，避免在调用点写复合字面量取址 */
static int push_one(frame_queue_t *q, uint32_t id, uint8_t dlc, uint8_t seed)
{
    gw_frame_t f = mk_frame(id, dlc, seed);
    return frame_queue_push(q, &f);
}

static void test_basic(void)
{
    frame_queue_t q;
    gw_frame_t out;

    printf("Basic push / pop\n");

    check(frame_queue_init(&q, 4) == 0, "init succeeds");
    check(frame_queue_count(&q) == 0, "starts empty");
    check(frame_queue_capacity(&q) == 4, "capacity recorded");

    check(frame_queue_pop(&q, &out) == -1, "pop on empty returns -1");

    check(push_one(&q, 0x180, 8, 1) == 0, "push 1");
    check(push_one(&q, 0x181, 4, 2) == 0, "push 2");
    check(frame_queue_count(&q) == 2, "count is 2");

    check(frame_queue_pop(&q, &out) == 0, "pop 1");
    check(out.id == 0x180 && out.dlc == 8, "FIFO order: first in first out");
    check(out.data[0] == 1 && out.data[7] == 8, "payload preserved");

    check(frame_queue_pop(&q, &out) == 0, "pop 2");
    check(out.id == 0x181 && out.dlc == 4, "second frame matches");

    frame_queue_free(&q);
}

/* 回绕：head/tail 绕回数组起点后数据顺序仍要正确 */
static void test_wrap_around(void)
{
    frame_queue_t q;
    gw_frame_t out;

    printf("\nWrap around\n");

    frame_queue_init(&q, 3);

    for (uint32_t i = 0; i < 3; i++)
        push_one(&q, 0x100 + i, 1, (uint8_t)i);

    for (uint32_t i = 0; i < 3; i++)
    {
        frame_queue_pop(&q, &out);
        check(out.id == 0x100 + i, "pre-wrap order preserved");
    }

    /* 再压入，触发 tail 回绕到 0 */
    for (uint32_t i = 0; i < 3; i++)
        push_one(&q, 0x200 + i, 1, (uint8_t)i);

    for (uint32_t i = 0; i < 3; i++)
    {
        check(frame_queue_pop(&q, &out) == 0, "pop after wrap");
        check(out.id == 0x200 + i, "post-wrap order preserved");
    }

    frame_queue_free(&q);
}

/* 队满：丢弃新帧并计数，不覆盖旧数据 */
static void test_overflow(void)
{
    frame_queue_t q;
    gw_frame_t out;

    printf("\nOverflow policy\n");

    frame_queue_init(&q, 2);

    push_one(&q, 0x300, 1, 1);
    push_one(&q, 0x301, 1, 2);

    check(push_one(&q, 0x302, 1, 3) == -1, "push on full returns -1");
    check(frame_queue_dropped(&q) == 1, "dropped counter incremented");
    check(frame_queue_count(&q) == 2, "count unchanged after drop");

    /* 旧数据必须还在——队满不能覆盖未消费的帧 */
    frame_queue_pop(&q, &out);
    check(out.id == 0x300, "oldest frame still intact");
    frame_queue_pop(&q, &out);
    check(out.id == 0x301, "second frame still intact");

    /* 腾出空间后可以继续 */
    check(push_one(&q, 0x303, 1, 4) == 0, "push works after drain");

    frame_queue_free(&q);
}

/* 容量 1 的极端情况 */
static void test_single_slot(void)
{
    frame_queue_t q;
    gw_frame_t out;

    printf("\nSingle slot\n");

    frame_queue_init(&q, 1);

    check(push_one(&q, 0x400, 1, 9) == 0, "push into 1-slot queue");
    check(push_one(&q, 0x401, 1, 9) == -1, "second push rejected");
    check(frame_queue_pop(&q, &out) == 0, "pop from 1-slot queue");
    check(out.id == 0x400, "content correct");
    check(frame_queue_pop(&q, &out) == -1, "empty again");

    frame_queue_free(&q);
}

/* 参数校验 */
static void test_params(void)
{
    frame_queue_t q;

    printf("\nParameter checks\n");

    check(frame_queue_init(0, 4) == -1, "null queue rejected");
    check(frame_queue_init(&q, 0) == -1, "zero capacity rejected");

    frame_queue_free(0); /* 不应崩溃 */
    check(frame_queue_count(0) == 0, "count on null is 0");
    check(frame_queue_dropped(0) == 0, "dropped on null is 0");
}

int main(void)
{
    printf("=== frame_queue test ===\n\n");
    test_basic();
    test_wrap_around();
    test_overflow();
    test_single_slot();
    test_params();

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
