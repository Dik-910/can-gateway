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
 *   # src,id_min,id_max,dst,offset,note[,mask]
 *   a,0x100,0x1FF,b,0,status A->B
 *   b,0x300,0x3FF,a,0,cmd B->A
 *   a,0x180,0,b,0,only xx0 ids,0x7F0
 *
 *   [diag]
 *   # tester_ch,tester_req,tester_rsp,target_ch,target_req,target_rsp
 *   a,0x7E0,0x7E8,b,0x7E0,0x7E8
 *
 *   [mqtt]
 *   # 不写这一段或 enabled = 0 时不上报
 *   addr = tcp://127.0.0.1:1883
 *   topic = gw/can
 *   window = 1000          # 聚合窗口（毫秒）
 *   force = 30             # 数据不变时，最多隔多少秒也要上报一次（默认 30）
 *   channels = a,b         # 只采集这两路；留空表示全部
 *
 *   [limits]
 *   # 通道总速率（帧/秒）
 *   a = 3000
 *   # 单 ID 速率：id 通道名,ID,上限
 *   id a,0x181,60
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
#define CFG_MAX_LIMITS   32

typedef struct
{
    char src[CFG_NAME_MAX];
    char dst[CFG_NAME_MAX];
    uint32_t id_min;
    uint32_t id_max;
    uint32_t id_mask; /* 0 表示用范围匹配，非 0 表示按掩码匹配 */
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
    uint32_t bitrate;            /* 用于负载率计算，缺省 500000 */
} cfg_channel_t;

/* 通道级速率上限（帧/秒） */
typedef struct
{
    char ch[CFG_NAME_MAX];
    uint32_t limit;
} cfg_chan_limit_t;

/* 单 ID 速率上限 */
typedef struct
{
    char ch[CFG_NAME_MAX];
    uint32_t id;
    uint32_t limit;
} cfg_id_limit_t;

/* MQTT 上行配置 */
typedef struct
{
    int      enabled;
    char     addr[64];
    char     topic[64];
    char     client_id[32];
    uint32_t window_ms;              /* 聚合窗口 */
    uint32_t force_ms;               /* 数据不变时，多久强制上报一次 */
    char     channels[64];           /* 逗号分隔的通道短名；空表示全部 */
} cfg_mqtt_t;

typedef struct
{
    cfg_channel_t channels[CFG_MAX_CHANNELS];
    int channel_count;

    cfg_route_t routes[CFG_MAX_ROUTES];
    int route_count;

    cfg_diag_t diags[CFG_MAX_DIAGS];
    int diag_count;

    cfg_chan_limit_t chan_limits[CFG_MAX_CHANNELS];
    int chan_limit_count;
    cfg_id_limit_t id_limits[CFG_MAX_LIMITS];
    int id_limit_count;

    cfg_mqtt_t mqtt;
} gw_config_t;

/* 载入配置。成功返回 0，失败返回 -1（会打印具体行号） */
int config_load(gw_config_t *cfg, const char *path);

/* 按短名查通道索引，找不到返回 -1 */
int config_find_channel(const gw_config_t *cfg, const char *name);

/* 打印配置摘要，便于启动时确认读到了什么 */
void config_dump(const gw_config_t *cfg);

#endif
