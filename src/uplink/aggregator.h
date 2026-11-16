/*
 * aggregator.h —— 按 (通道, ID) 聚合，避免逐帧上报
 *
 * 为什么必须聚合：
 * 0x181 是 20ms 周期，一个 ID 就是 50 帧/秒。总线上十几个 ID，
 * 逐帧发 MQTT 就是每秒几百条消息——一天四百万条。broker 扛不住，
 * 云端存储也扛不住，而其中绝大多数是重复值。
 *
 * 所以这里做三件事：
 *   1. 只保留每个 ID 的最新数据 + 窗口内帧数
 *   2. 只在数据变化时导出（dirty）
 *   3. 长期不变的 ID 也要强制导出一次（心跳），否则云端会以为它消失了
 *
 * 第 3 条容易被忽略：只发变化值的话，一个恒定 25℃ 的温度传感器
 * 半小时后云端就再也没收到过它，看板上那个点会「冻结」。
 */
#ifndef __AGGREGATOR_H
#define __AGGREGATOR_H

#include <stdint.h>

#define AGG_MAX_ENTRIES      64
#define AGG_DEFAULT_WINDOW   1000    /* 聚合窗口（毫秒） */
#define AGG_DEFAULT_FORCE    30000   /* 多久没变化就强制导出一次 */

typedef struct
{
    uint8_t  channel;
    uint32_t id;
    uint32_t count;              /* 本窗口内收到的帧数 */
    uint64_t last_ms;            /* 最后一帧的时间 */
    uint64_t last_export_ms;     /* 上次导出的时间 */

    uint8_t  dlc;
    uint8_t  data[8];

    uint8_t  dirty;              /* 本窗口内数据有变化（含首次） */
    uint8_t  used;
} agg_entry_t;

typedef struct
{
    agg_entry_t entry[AGG_MAX_ENTRIES];
    uint64_t exports;            /* 累计导出条数 */
    uint64_t overflow;           /* 表满导致的新 ID 丢弃次数 */
} aggregator_t;

void agg_init(aggregator_t *a);

/*
 * 收到一帧时调用。未登记的 (通道,ID) 自动占一个新槽位。
 * 表满时丢弃并计入 overflow —— 不驱逐已有条目，否则老 ID 会反复进出，
 * 每次都要重新算首次导出，反而是更坏的行为。
 */
void agg_update(aggregator_t *a, uint8_t channel, uint32_t id,
                const uint8_t *data, uint8_t dlc);

/* 导出回调：返回 0 表示已成功投递，非 0 会让该条留在 dirty 状态等下次 */
typedef int (*agg_export_cb)(const agg_entry_t *entry, void *user);

/*
 * 导出本窗口需要上报的条目。
 * 条件是「有变化」或「距上次导出超过 force_ms」。
 * 导出成功后清 dirty 与 count。
 * 返回导出条数。
 */
int agg_export(aggregator_t *a, uint64_t now_ms, uint32_t force_ms,
               agg_export_cb cb, void *user);

int agg_count(const aggregator_t *a);

#endif
