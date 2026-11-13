#include "frame_queue.h"

#include <stdlib.h>
#include <string.h>

int frame_queue_init(frame_queue_t *q, uint32_t capacity)
{
    if (q == 0 || capacity == 0)
        return -1;

    memset(q, 0, sizeof(*q));
    q->slots = (gw_frame_t *)calloc(capacity, sizeof(gw_frame_t));
    if (q->slots == 0)
        return -1;

    q->capacity = capacity;
    return 0;
}

void frame_queue_free(frame_queue_t *q)
{
    if (q == 0)
        return;

    free(q->slots);
    q->slots = 0;
    q->capacity = 0;
    q->head = 0;
    q->count = 0;
}

int frame_queue_push(frame_queue_t *q, const gw_frame_t *f)
{
    uint32_t tail;

    if (q == 0 || q->slots == 0 || f == 0)
        return -1;

    /*
     * 队满时直接丢弃并计数，不阻塞调用方。
     * 阻塞会让读线程停下来，导致内核 socket 缓冲区溢出——那是更大的问题。
     * 丢帧 + 计数，由监控暴露出来，比偷偷卡住好。
     */
    if (q->count >= q->capacity)
    {
        q->dropped++;
        return -1;
    }

    tail = (q->head + q->count) % q->capacity;
    q->slots[tail] = *f;
    q->count++;
    return 0;
}

int frame_queue_pop(frame_queue_t *q, gw_frame_t *f)
{
    if (q == 0 || q->slots == 0 || f == 0)
        return -1;
    if (q->count == 0)
        return -1;

    *f = q->slots[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return 0;
}

uint32_t frame_queue_count(const frame_queue_t *q)
{
    return (q != 0) ? q->count : 0;
}

uint32_t frame_queue_dropped(const frame_queue_t *q)
{
    return (q != 0) ? q->dropped : 0;
}

uint32_t frame_queue_capacity(const frame_queue_t *q)
{
    return (q != 0) ? q->capacity : 0;
}
