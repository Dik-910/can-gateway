#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include "route_engine.h"
#include "log/log.h"

/*
 * 网关主程序。
 *
 * 默认监听两路 CAN，配置一组基础路由。真实产品这里应该读配置文件
 * （路由表、通道名、过滤规则都该可配），当前先硬编码，等引擎稳定再抽配置。
 *
 * 用法：
 *   ./gateway                          # vcan0 <-> vcan1
 *   ./gateway --a can0 --b can1        # 指定接口名
 */

#define CH_A 0
#define CH_B 1

static route_engine_t *g_engine = 0;

static void on_signal(int sig)
{
    (void)sig;
    if (g_engine != 0)
        route_engine_stop(g_engine);
}

int main(int argc, char **argv)
{
    const char *if_a = "vcan0";
    const char *if_b = "vcan1";
    route_engine_t engine;
    int idx_a, idx_b;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--a") == 0 && i + 1 < argc)
            if_a = argv[++i];
        else if (strcmp(argv[i], "--b") == 0 && i + 1 < argc)
            if_b = argv[++i];
        else if (strcmp(argv[i], "--help") == 0)
        {
            printf("usage: %s [--a ifname] [--b ifname]\n", argv[0]);
            printf("  default: --a vcan0 --b vcan1\n");
            return 0;
        }
    }

    log_set_level(LOG_INFO);

    if (route_engine_init(&engine) != 0)
        return 1;

    g_engine = &engine;

    idx_a = route_engine_add_channel(&engine, if_a);
    idx_b = route_engine_add_channel(&engine, if_b);

    if (idx_a < 0 || idx_b < 0)
    {
        log_error("failed to open channels (%s, %s)", if_a, if_b);
        route_engine_close(&engine);
        return 1;
    }

    /*
     * 基础路由：
     *   0x100-0x1FF  A -> B   节点状态类报文
     *   0x200-0x2FF  A -> B   采样数据
     *   0x300-0x3FF  B -> A   控制命令
     *   0x7E0-0x7E7  A -> B   诊断请求（与响应需会话映射，暂按静态转发）
     *   0x7E8-0x7EF  B -> A   诊断响应
     */
    {
        route_rule_t r;

        memset(&r, 0, sizeof(r));
        r.src_channel = (uint8_t)idx_a;
        r.id_min = 0x100;
        r.id_max = 0x1FF;
        r.dst_channel = (uint8_t)idx_b;
        r.id_offset = 0;
        r.enabled = 1;
        snprintf(r.note, sizeof(r.note), "status A->B");
        route_engine_add_route(&engine, &r);

        memset(&r, 0, sizeof(r));
        r.src_channel = (uint8_t)idx_a;
        r.id_min = 0x200;
        r.id_max = 0x2FF;
        r.dst_channel = (uint8_t)idx_b;
        snprintf(r.note, sizeof(r.note), "data A->B");
        route_engine_add_route(&engine, &r);

        memset(&r, 0, sizeof(r));
        r.src_channel = (uint8_t)idx_b;
        r.id_min = 0x300;
        r.id_max = 0x3FF;
        r.dst_channel = (uint8_t)idx_a;
        snprintf(r.note, sizeof(r.note), "cmd B->A");
        route_engine_add_route(&engine, &r);

        memset(&r, 0, sizeof(r));
        r.src_channel = (uint8_t)idx_a;
        r.id_min = 0x7E0;
        r.id_max = 0x7E7;
        r.dst_channel = (uint8_t)idx_b;
        snprintf(r.note, sizeof(r.note), "diag req A->B");
        route_engine_add_route(&engine, &r);

        memset(&r, 0, sizeof(r));
        r.src_channel = (uint8_t)idx_b;
        r.id_min = 0x7E8;
        r.id_max = 0x7EF;
        r.dst_channel = (uint8_t)idx_a;
        snprintf(r.note, sizeof(r.note), "diag rsp B->A");
        route_engine_add_route(&engine, &r);
    }

    /*
     * 诊断映射示例。
     *
     * tester1 在 A 通道用 0x7E0 发请求，诊断 B 通道上的节点（节点侧 0x7E0 / 回 0x7E8）。
     * 这条映射一旦存在，诊断 ID 段的帧会优先走映射，不再落到上面那两条静态规则。
     *
     * 要接第二个 tester 时再加一条：把 tester_req_id 换成 0x7E1、tester_rsp_id 换成 0x7E9，
     * 节点的 target_rsp_id 换成 0x7EA。注意 target_rsp_id 必须与已有映射不同——
     * 响应帧里只有 ID 能定位来源，重复就无法分辨（add 时会直接拒绝）。
     */
    {
        diag_map_t m;

        memset(&m, 0, sizeof(m));
        m.tester_channel = (uint8_t)idx_a;
        m.tester_req_id = 0x7E0;
        m.tester_rsp_id = 0x7E8;
        m.target_channel = (uint8_t)idx_b;
        m.target_req_id = 0x7E0;
        m.target_rsp_id = 0x7E8;

        if (route_engine_add_diag_map(&engine, &m) == 0)
            log_info("diag map: %s 0x7E0 -> %s 0x7E0 (response 0x7E8 back)",
                     if_a, if_b);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    log_info("gateway starting: %s <-> %s", if_a, if_b);

    route_engine_run(&engine);

    log_info("gateway stopping");
    route_engine_print_stats(&engine);
    route_engine_close(&engine);
    return 0;
}
