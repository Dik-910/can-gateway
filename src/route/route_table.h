#ifndef __ROUTE_TABLE_H
#define __ROUTE_TABLE_H

#include <stdint.h>

/*
 * 静态路由表。
 *
 * 一条规则回答：从哪条通道来的、ID 落在什么范围的帧，应该发到哪条通道、ID 要不要改写。
 *
 * 为什么用「范围 + 偏移」而不是逐条 ID 列举：
 *   网关常按功能域整体转发（例如 0x180-0x1FF 全部转发），
 *   逐条列举会让配置表膨胀到几百行，改一个 ID 就得找出所有相关行。
 *
 * 匹配是**首次命中即返回**，所以规则的顺序有意义：特殊规则放前面，兜底规则放后面。
 */

#define GW_MAX_ROUTES  32
#define GW_NOTE_MAX    32
#define GW_CHANNEL_ANY 0xFF /* 用于 src_channel，表示任意来源 */

typedef struct
{
    uint8_t src_channel;  /* 来源通道索引，GW_CHANNEL_ANY 表示不限 */
    uint32_t id_min;      /* ID 匹配范围（含） */
    uint32_t id_max;      /* ID 匹配范围（含） */

    uint8_t dst_channel;  /* 目标通道索引 */
    int32_t id_offset;    /* 目标 ID = 源 ID + offset，0 表示不改写 */
    uint8_t enabled;
    char note[GW_NOTE_MAX];
} route_rule_t;

typedef struct
{
    route_rule_t rules[GW_MAX_ROUTES];
    uint16_t count;

    uint32_t matched; /* 命中次数，用于监控 */
    uint32_t missed;  /* 没有规则匹配的帧数 */
} route_table_t;

void route_table_init(route_table_t *t);

/* 追加一条规则。表满或参数非法返回 -1 */
int route_table_add(route_table_t *t, const route_rule_t *rule);

/*
 * 查找匹配规则。命中返回规则指针并累加 matched；未命中返回 NULL 并累加 missed。
 */
const route_rule_t *route_table_match(route_table_t *t, uint8_t src_channel, uint32_t id);

/*
 * 计算转发后的目标 ID（应用 offset）。
 * 结果超出 11 位标准帧范围时返回 -1 —— 这种规则应该在配置阶段就被拒绝，
 * 留到运行期改写会发出非法 ID。
 */
int route_target_id(const route_rule_t *rule, uint32_t src_id, uint32_t *out_id);

#endif
