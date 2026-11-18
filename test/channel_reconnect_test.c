/*
 * channel_reconnect_test.c —— 断链判定与重开
 *
 * 不需要 vcan：判定逻辑是纯函数，重开的失败路径也只依赖「接口不存在」。
 * 真实链路 down/up 的验证在 tools/reconnect_check.py（需要 vcan 与 root）。
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "can_channel.h"

static int g_total;
static int g_fail;

static void check(int cond, const char *what)
{
    g_total++;
    if (cond)
    {
        printf("  [PASS] %s\n", what);
    }
    else
    {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

static void check_u32(uint32_t got, uint32_t want, const char *what)
{
    g_total++;
    if (got == want)
    {
        printf("  [PASS] %s\n", what);
    }
    else
    {
        g_fail++;
        printf("  [FAIL] %s  (got %u, want %u)\n", what, (unsigned)got, (unsigned)want);
    }
}

static void test_classify(void)
{
    printf("[错误分类]\n");

    /* 链路没了：socket 已经不能再用，必须重开 */
    check(can_channel_error_fatal(ENETDOWN) == 1, "ENETDOWN is fatal");
    check(can_channel_error_fatal(ENODEV) == 1, "ENODEV is fatal");
    check(can_channel_error_fatal(EBADF) == 1, "EBADF is fatal");
    check(can_channel_error_fatal(EPERM) == 1, "EPERM is fatal");

    /*
     * 这些不能触发重开。
     * ENOBUFS 尤其关键：它是「发得太快」，链路是好的；
     * 误判成断链会平白丢掉一段接收窗口。
     */
    check(can_channel_error_fatal(ENOBUFS) == 0, "ENOBUFS is not fatal");
    check(can_channel_error_fatal(EAGAIN) == 0, "EAGAIN is not fatal");
    check(can_channel_error_fatal(EINTR) == 0, "EINTR is not fatal");
    check(can_channel_error_fatal(0) == 0, "no error is not fatal");
}

static void test_note_error(void)
{
    can_channel_t ch;

    printf("[错误记录]\n");
    memset(&ch, 0, sizeof(ch));

    can_channel_note_error(&ch, ENOBUFS);
    check(ch.need_reopen == 0, "a busy socket does not ask for a reopen");
    check(ch.last_errno == ENOBUFS, "errno is kept for diagnosis");

    can_channel_note_error(&ch, ENETDOWN);
    check(ch.need_reopen == 1, "a downed interface asks for a reopen");
    check(ch.last_errno == ENETDOWN, "last errno is updated");

    check(can_channel_needs_reopen(&ch) == 1, "needs_reopen reports it");
    check(can_channel_needs_reopen(0) == 0, "null channel is never in need");
}

static void test_reopen_failure(void)
{
    can_channel_t ch;

    printf("[重开失败]\n");
    memset(&ch, 0, sizeof(ch));
    strncpy(ch.name, "nosuchcan0", sizeof(ch.name) - 1);
    ch.fd = -1;
    ch.opened = 0;
    ch.bitrate = 500000;
    ch.rx_frames = 1234;
    ch.tx_frames = 567;
    ch.err_bus = 3;
    ch.need_reopen = 1;

    check(can_channel_reopen(&ch) != 0, "reopening a missing interface fails");
    check_u32(ch.reopen_fail, 1, "failure is counted");
    check_u32(ch.reopen_count, 0, "no successful reopen is counted");
    check(ch.need_reopen == 1, "the channel stays marked for retry");
    check(strcmp(ch.name, "nosuchcan0") == 0, "channel name survives a failed reopen");
    check(ch.opened == 0 && ch.fd == -1, "a failed reopen leaves the channel closed");

    /*
     * 统计必须保留：重开发生在故障之后，这时候「断之前收了多少」
     * 正是排查要用的数字，清零等于把证据抹掉。
     */
    check_u32(ch.rx_frames, 1234, "rx counter survives a failed reopen");
    check_u32(ch.tx_frames, 567, "tx counter survives a failed reopen");
    check_u32(ch.err_bus, 3, "bus error counter survives a failed reopen");

    check(can_channel_reopen(&ch) != 0, "second attempt also fails");
    check_u32(ch.reopen_fail, 2, "retries accumulate");
}

static void test_closed_channel(void)
{
    can_channel_t ch;

    printf("[未打开的通道]\n");
    memset(&ch, 0, sizeof(ch));
    ch.fd = -1;

    check(can_channel_fd(&ch) == -1, "closed channel has no fd");
    check(can_channel_needs_reopen(&ch) == 0, "a never-opened channel is not 'broken'");
}

int main(void)
{
    printf("=== channel_reconnect_test ===\n");

    test_classify();
    test_note_error();
    test_reopen_failure();
    test_closed_channel();

    printf("\nResult: %d checks, %d failed\n", g_total, g_fail);
    return g_fail == 0 ? 0 : 1;
}
