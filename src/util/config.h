#ifndef __CONFIG_H
#define __CONFIG_H

#include <stdint.h>

/*
 * 网关配置。
 *
 * 原先通道名、路由表、诊断映射全部硬编码在 gateway_main.c 里，
 * 改一条规则要重新编译——这不是能部署的东西。
 *
 * 格式：INI 风格，通道用名字引用而不是索引，配置里写 "a"/"b" 比写 0/1 直观得多。
 *
 *   [channels]
 *   a = vcan0
 *   b = vcan1
 *
 *   [routes]
 *   # src,id_min,id_max,dst,offset,note
 *   a,0x100,0x1FF,b,0,status A->B
 *   b,0x300,0x3FF,a,0,cmd B->A
 *
 *   [diag]
 *   # tester_ch,tester_req,tester_rsp,target_ch,target_req,target_rsp
 *   a,0x7E0,0x7E8,b,0x7E0,0x7E8
 *
 * 解析器故意做得很朴素：不引入 JSON 库，不处理引号转义，不支持续行。
 * 网关配置是给工程人员看和改的，越简单越不容易出错。
 */

#define CFG_NAME_MAX   16
#define CFG_IFNAME_MAX 16
#define CFG_NOTE_MAX   32
#define CFG_MAX_CHANNELS 8
#define CFG_MAX_ROUTES   32
#define CFG_MAX_DIAGS    16

typedef struct
{
    char src[CFG_NAME_MAX];
    char dst[CFG_NAME_MAX];
    uint32_t id_min;
    uint32_t id_max;
    int32_t offset;
    char note[CFG_NOTE_MAX];
} cfg_route_t;

typedef struct
{
    char tester_ch[CFG_NAME_MAX];
    uint32_t tester_req;
    uint32_t tester_rsp;
    char target_ch[CFG_NAME_MAX];
    uint32_t target_req;
    uint32_t target_rsp;
} cfg_diag_t;

typedef struct
{
    char name[CFG_NAME_MAX];     /* 配置里的短名，如 "a" */
    char ifname[CFG_IFNAME_MAX]; /* 系统接口名，如 "vcan0" */
} cfg_channel_t;

typedef struct
{
    cfg_channel_t channels[CFG_MAX_CHANNELS];
    int channel_count;

    cfg_route_t routes[CFG_MAX_ROUTES];
    int route_count;

    cfg_diag_t diags[CFG_MAX_DIAGS];
    int diag_count;
} gw_config_t;

/* 载入配置。成功返回 0，失败返回 -1（会打印具体行号） */
int config_load(gw_config_t *cfg, const char *path);

/* 按短名查通道索引，找不到返回 -1 */
int config_find_channel(const gw_config_t *cfg, const char *name);

/* 打印配置摘要，便于启动时确认读到了什么 */
void config_dump(const gw_config_t *cfg);

#endif
