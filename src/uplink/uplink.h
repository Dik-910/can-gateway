/*
 * uplink.h —— MQTT 上行（独立线程 + 有界队列）
 *
 * 为什么必须独立线程：
 * 网关的主循环是单线程 epoll，追求确定性延迟。而 MQTT publish 会遇到
 * TCP 发送缓冲满、broker 慢、网络抖动——任何一次阻塞都会直接拖住转发，
 * 把「1ms 的转发延迟」变成「几百毫秒」。这是车载网关不能接受的。
 *
 * 所以主循环只做一件事：把 JSON 塞进队列（非阻塞），塞不进就丢并计数。
 * 真正的网络 IO 交给这个线程，它慢一点没关系，丢几条遥测也没关系。
 *
 * 队列有界且满了就丢，不阻塞生产者 —— 这和 mcu-gateway 里
 * rx_task 用 xQueueSend(0) 是同一个判断：宁可丢帧并计数，
 * 也不要把「网络慢」放大成「转发停摆」。
 */
#ifndef __UPLINK_H
#define __UPLINK_H

#include <stdint.h>

#define GW_UPLINK_QUEUE     64
#define GW_UPLINK_MSG_MAX   256

typedef struct uplink uplink_t;

/*
 * 创建/销毁。用不透明类型 + 工厂函数，是为了不把 pthread 的头文件和
 * 内部锁结构暴露给所有包含 route_engine.h 的翻译单元。
 */
uplink_t *uplink_create(const char *addr, const char *topic, const char *cid);

/* 停止线程并释放。可传 NULL。会先尝试把队列里剩下的发完 */
void uplink_destroy(uplink_t *u);

int  uplink_start(uplink_t *u);

/*
 * 非阻塞投递。返回 0 表示已入队，-1 表示队列满或未运行。
 * 调用方（主循环）不需要处理失败——计数即可。
 */
int  uplink_publish(uplink_t *u, const char *json);

/* 只停线程，不释放内存（destroy 内部会调用） */
void uplink_stop(uplink_t *u);

/* 是否编译时启用了 MQTT（没装 Paho 时为 0） */
int  uplink_available(void);

void uplink_print_stats(uplink_t *u);

#endif
