#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "src/util/config.h"

/*
 * 配置解析测试。
 *
 * 重点不在「能读对一个文件」，而在「读错的时候能不能报出来」：
 * 配置写错却静默忽略，是网关最危险的故障之一——规则没生效但没人知道。
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

#define TMP_CONF "./.cfg_test.conf"

static void write_file(const char *content)
{
    FILE *fp = fopen(TMP_CONF, "w");
    if (fp)
    {
        fputs(content, fp);
        fclose(fp);
    }
}

static void test_valid(void)
{
    gw_config_t cfg;

    printf("Valid config\n");

    write_file(
        "# comment line\n"
        "\n"
        "[channels]\n"
        "a = vcan0\n"
        "b = vcan1\n"
        "\n"
        "[routes]\n"
        "a,0x100,0x1FF,b,0,status\n"
        "b,0x300,0x3FF,a,0x10,cmd\n"
        "\n"
        "[diag]\n"
        "a,0x7E0,0x7E8,b,0x7E0,0x7E8\n");

    check(config_load(&cfg, TMP_CONF) == 0, "load succeeds");
    check(cfg.channel_count == 2, "two channels");
    check(strcmp(cfg.channels[0].name, "a") == 0, "channel 0 name");
    check(strcmp(cfg.channels[0].ifname, "vcan0") == 0, "channel 0 ifname");
    check(cfg.route_count == 2, "two routes");
    check(cfg.diag_count == 1, "one diag map");

    check(cfg.routes[0].id_min == 0x100 && cfg.routes[0].id_max == 0x1FF,
          "hex id parsed");
    check(cfg.routes[1].offset == 0x10, "hex offset parsed");
    check(strcmp(cfg.routes[0].note, "status") == 0, "note parsed");

    check(config_find_channel(&cfg, "a") == 0, "find channel a");
    check(config_find_channel(&cfg, "b") == 1, "find channel b");
    check(config_find_channel(&cfg, "zzz") == -1, "unknown channel returns -1");

    check(cfg.diags[0].tester_req == 0x7E0, "diag tester_req parsed");
    check(cfg.diags[0].target_rsp == 0x7E8, "diag target_rsp parsed");
}

static void test_errors(void)
{
    gw_config_t cfg;

    printf("\nError handling\n");

    /* 路由引用了不存在的通道 */
    write_file(
        "[channels]\n"
        "a = vcan0\n"
        "[routes]\n"
        "a,0x100,0x1FF,zzz,0,broken\n");
    check(config_load(&cfg, TMP_CONF) == -1, "unknown channel in route rejected");

    /* 路由字段不足 */
    write_file(
        "[channels]\n"
        "a = vcan0\n"
        "b = vcan1\n"
        "[routes]\n"
        "a,0x100,0x1FF,b\n");
    check(config_load(&cfg, TMP_CONF) == -1, "short route line rejected");

    /* ID 不是数字 */
    write_file(
        "[channels]\n"
        "a = vcan0\n"
        "b = vcan1\n"
        "[routes]\n"
        "a,abc,0x1FF,b,0,bad\n");
    check(config_load(&cfg, TMP_CONF) == -1, "non-numeric id rejected");

    /* 没有任何通道 */
    write_file("[routes]\n");
    check(config_load(&cfg, TMP_CONF) == -1, "config without channels rejected");

    /* 诊断映射字段不足 */
    write_file(
        "[channels]\n"
        "a = vcan0\n"
        "b = vcan1\n"
        "[diag]\n"
        "a,0x7E0,0x7E8,b\n");
    check(config_load(&cfg, TMP_CONF) == -1, "short diag line rejected");

    /* 文件不存在 */
    check(config_load(&cfg, "./no_such_file.conf") == -1, "missing file rejected");
}

static void test_tolerances(void)
{
    gw_config_t cfg;

    printf("\nTolerances\n");

    /* 空行、注释、前后空格、段外的键都要能容忍 */
    write_file(
        "   \n"
        "# leading comment\n"
        "[channels]   \n"
        "   a   =   vcan0   \n"
        "\n"
        "[routes]\n"
        "  a , 0x100 , 0x1FF , a , 0 , spaced  \n");

    check(config_load(&cfg, TMP_CONF) == 0, "whitespace tolerated");
    check(strcmp(cfg.channels[0].name, "a") == 0, "trimmed channel name");
    check(strcmp(cfg.channels[0].ifname, "vcan0") == 0, "trimmed ifname");
    check(strcmp(cfg.routes[0].note, "spaced") == 0, "trimmed trailing note");
}

int main(void)
{
    printf("=== config test ===\n\n");
    test_valid();
    test_errors();
    test_tolerances();

    unlink(TMP_CONF);

    printf("\nResult: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
