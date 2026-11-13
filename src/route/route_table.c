#include "route_table.h"

#include <string.h>

#include "log/log.h"

#define GW_STD_ID_MAX 0x7FFu

void route_table_init(route_table_t *t)
{
    if (t == 0)
        return;
    memset(t, 0, sizeof(*t));
}

int route_table_add(route_table_t *t, const route_rule_t *rule)
{
    if (t == 0 || rule == 0)
        return -1;

    if (t->count >= GW_MAX_ROUTES)
    {
        log_error("route table full (%d rules)", GW_MAX_ROUTES);
        return -1;
    }

    if (rule->id_mask != 0)
    {
        if (rule->id_min > GW_STD_ID_MAX || rule->id_mask > GW_STD_ID_MAX)
        {
            log_error("invalid mask rule value=0x%03X mask=0x%03X", rule->id_min, rule->id_mask);
            return -1;
        }
    }
    else if (rule->id_min > rule->id_max || rule->id_max > GW_STD_ID_MAX)
    {
        log_error("invalid id range 0x%03X-0x%03X", rule->id_min, rule->id_max);
        return -1;
    }

    /*
     * 在配置阶段就检查改写后的 ID 是否越界。
     * 如果留到运行期，每条帧都要判断一次，且出错时只能丢帧。
     */
    if (rule->id_offset != 0)
    {
        int64_t lo = (int64_t)rule->id_min + rule->id_offset;
        int64_t hi = (int64_t)rule->id_max + rule->id_offset;

        if (lo < 0 || hi > (int64_t)GW_STD_ID_MAX)
        {
            log_error("id_offset %d pushes range out of bounds (0x%llX-0x%llX)",
                      rule->id_offset, (unsigned long long)lo, (unsigned long long)hi);
            return -1;
        }
    }

    t->rules[t->count] = *rule;
    t->rules[t->count].note[GW_NOTE_MAX - 1] = '\0';
    t->count++;
    return 0;
}

const route_rule_t *route_table_match(route_table_t *t, uint8_t src_channel, uint32_t id)
{
    if (t == 0)
        return 0;

    for (uint16_t i = 0; i < t->count; i++)
    {
        const route_rule_t *r = &t->rules[i];

        if (!r->enabled)
            continue;

        if (r->src_channel != GW_CHANNEL_ANY && r->src_channel != src_channel)
            continue;

        if (r->id_mask != 0)
        {
            if ((id & r->id_mask) != (r->id_min & r->id_mask))
                continue;
        }
        else if (id < r->id_min || id > r->id_max)
        {
            continue;
        }

        t->matched++;
        return r;
    }

    t->missed++;
    return 0;
}

int route_target_id(const route_rule_t *rule, uint32_t src_id, uint32_t *out_id)
{
    int64_t target;

    if (rule == 0 || out_id == 0)
        return -1;

    target = (int64_t)src_id + rule->id_offset;
    if (target < 0 || target > (int64_t)GW_STD_ID_MAX)
        return -1;

    *out_id = (uint32_t)target;
    return 0;
}
