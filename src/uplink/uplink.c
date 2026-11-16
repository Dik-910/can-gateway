/*
 * uplink.c —— MQTT 上行实现
 *
 * 三个角色分离得很清楚：
 *   生产者 = 主循环（epoll 线程），只入队，绝不阻塞
 *   队列   = 有界环形 + 互斥量 + 条件变量
 *   消费者 = 本文件的线程，负责连接、重连、发布
 */
#define _POSIX_C_SOURCE 200809L

#include "uplink.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log/log.h"

#ifdef GW_HAVE_MQTT
#include <MQTTClient.h>
#endif

#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 30000

struct uplink
{
    pthread_mutex_t lock;
    pthread_cond_t cond;          /* 有数据可发 或 要退出 */
    pthread_cond_t space;         /* 队列腾出空位（仅用于唤醒，不阻塞生产者） */

    char q[GW_UPLINK_QUEUE][GW_UPLINK_MSG_MAX];
    int head;
    int tail;
    int count;

    pthread_t thread;
    int running;
    int started;

    char addr[64];
    char topic[64];
    char cid[32];

#ifdef GW_HAVE_MQTT
    MQTTClient client;
    int client_created;
#endif
    int connected;
    uint32_t backoff_ms;
    uint64_t retry_at_ms;

    /* 统计 */
    uint64_t enqueued;
    uint64_t published;
    uint64_t dropped;      /* 队列满 */
    uint64_t failed;       /* publish 失败或未连接 */
    uint64_t reconnects;
};

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

int uplink_available(void)
{
#ifdef GW_HAVE_MQTT
    return 1;
#else
    return 0;
#endif
}

int uplink_init(uplink_t *u, const char *addr, const char *topic, const char *cid)
{
    if (u == 0)
        return -1;

    memset(u, 0, sizeof(*u));

    snprintf(u->addr, sizeof(u->addr), "%s",
             addr ? addr : "tcp://127.0.0.1:1883");
    snprintf(u->topic, sizeof(u->topic), "%s",
             topic ? topic : "gw/can");
    snprintf(u->cid, sizeof(u->cid), "%s",
             cid ? cid : "can-gateway");

    if (pthread_mutex_init(&u->lock, 0) != 0)
        return -1;

    if (pthread_cond_init(&u->cond, 0) != 0)
    {
        pthread_mutex_destroy(&u->lock);
        return -1;
    }

    if (pthread_cond_init(&u->space, 0) != 0)
    {
        pthread_cond_destroy(&u->cond);
        pthread_mutex_destroy(&u->lock);
        return -1;
    }

    u->backoff_ms = BACKOFF_MIN_MS;
    return 0;
}

int uplink_publish(uplink_t *u, const char *json)
{
    size_t n;

    if (u == 0 || json == 0)
        return -1;

    n = strlen(json);
    if (n == 0 || n >= GW_UPLINK_MSG_MAX)
        return -1;

    pthread_mutex_lock(&u->lock);

    if (u->count >= GW_UPLINK_QUEUE)
    {
        /*
         * 满了直接丢。不等待、不扩容：
         * 等待会把网络慢传导成主循环抖动，扩容则让延迟无上限增长。
         */
        u->dropped++;
        pthread_mutex_unlock(&u->lock);
        return -1;
    }

    memcpy(u->q[u->head], json, n + 1);
    u->head = (u->head + 1) % GW_UPLINK_QUEUE;
    u->count++;
    u->enqueued++;

    pthread_cond_signal(&u->cond);
    pthread_mutex_unlock(&u->lock);

    return 0;
}

/* ---------- MQTT 连接管理（只在 uplink 线程里调用） ---------- */

static int do_connect(uplink_t *u)
{
#ifdef GW_HAVE_MQTT
    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    int rc;

    if (!u->client_created)
    {
        rc = MQTTClient_create(&u->client, u->addr, u->cid,
                               MQTTCLIENT_PERSISTENCE_NONE, NULL);
        if (rc != MQTTCLIENT_SUCCESS)
        {
            log_error("uplink: create failed (%d)", rc);
            return -1;
        }
        u->client_created = 1;
    }

    opts.keepAliveInterval = 20;
    opts.cleansession = 1;
    opts.connectTimeout = 3;

    rc = MQTTClient_connect(u->client, &opts);
    if (rc != MQTTCLIENT_SUCCESS)
        return -1;

    u->connected = 1;
    u->backoff_ms = BACKOFF_MIN_MS;
    log_info("uplink: connected %s topic %s", u->addr, u->topic);
    return 0;
#else
    (void)u;
    return -1;
#endif
}

static void ensure_connected(uplink_t *u)
{
    uint64_t now;

    if (u->connected)
        return;

    now = now_ms();
    if (now < u->retry_at_ms)
        return;

    if (do_connect(u) == 0)
    {
        u->reconnects++;
        return;
    }

    u->retry_at_ms = now + u->backoff_ms;
    u->backoff_ms = (u->backoff_ms * 2 > BACKOFF_MAX_MS) ? BACKOFF_MAX_MS : u->backoff_ms * 2;

    log_info("uplink: retry in %u ms", (unsigned)u->backoff_ms);
}

static int do_publish(uplink_t *u, const char *msg)
{
#ifdef GW_HAVE_MQTT
    MQTTClient_message m = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;

    if (!u->connected)
    {
        u->failed++;
        return -1;
    }

    m.payload = (void *)msg;
    m.payloadlen = (int)strlen(msg);
    m.qos = 0;          /* 遥测：丢一条下一窗口还有 */
    m.retained = 0;

    rc = MQTTClient_publishMessage(u->client, u->topic, &m, &token);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        /* 连着失败就别再试了，让 ensure_connected 走重连流程 */
        u->connected = 0;
        u->failed++;
        return -1;
    }

    u->published++;
    return 0;
#else
    (void)u;
    (void)msg;
    u->failed++;
    return -1;
#endif
}

static void *uplink_thread(void *arg)
{
    uplink_t *u = (uplink_t *)arg;
    char msg[GW_UPLINK_MSG_MAX];

    log_info("uplink: thread started (%s)", u->addr);

    for (;;)
    {
        pthread_mutex_lock(&u->lock);

        while (u->running && u->count == 0)
            pthread_cond_wait(&u->cond, &u->lock);

        if (!u->running && u->count == 0)
        {
            pthread_mutex_unlock(&u->lock);
            break;
        }

        memcpy(msg, u->q[u->tail], sizeof(msg));
        u->tail = (u->tail + 1) % GW_UPLINK_QUEUE;
        u->count--;

        pthread_mutex_unlock(&u->lock);

        /* 网络操作全部在锁外做：持锁调 publish 会卡住主循环入队 */
        ensure_connected(u);
        do_publish(u, msg);
    }

#ifdef GW_HAVE_MQTT
    if (u->client_created)
    {
        if (u->connected)
            MQTTClient_disconnect(u->client, 1000);
        MQTTClient_destroy(&u->client);
        u->client_created = 0;
        u->connected = 0;
    }
#endif

    log_info("uplink: thread exit (pub=%llu drop=%llu fail=%llu)",
             (unsigned long long)u->published,
             (unsigned long long)u->dropped,
             (unsigned long long)u->failed);
    return 0;
}

uplink_t *uplink_create(const char *addr, const char *topic, const char *cid)
{
    uplink_t *u = (uplink_t *)malloc(sizeof(*u));

    if (u == 0)
        return 0;

    if (uplink_init(u, addr, topic, cid) != 0)
    {
        free(u);
        return 0;
    }

    return u;
}

void uplink_destroy(uplink_t *u)
{
    if (u == 0)
        return;

    if (u->started)
        uplink_stop(u);

    /* 同步对象的销毁放在这里，而不是 stop 里 ——
     * 否则 destroy 和 stop 会各销毁一次，重复调用是未定义行为。 */
    pthread_cond_destroy(&u->cond);
    pthread_cond_destroy(&u->space);
    pthread_mutex_destroy(&u->lock);

    free(u);
}

int uplink_start(uplink_t *u)
{
    if (u == 0 || u->started)
        return -1;

    u->running = 1;

    if (pthread_create(&u->thread, 0, uplink_thread, u) != 0)
    {
        u->running = 0;
        return -1;
    }

    u->started = 1;

    if (!uplink_available())
        log_warn("uplink: built without Paho, messages will be dropped");

    return 0;
}

void uplink_stop(uplink_t *u)
{
    if (u == 0 || !u->started)
        return;

    pthread_mutex_lock(&u->lock);
    u->running = 0;
    pthread_cond_broadcast(&u->cond);
    pthread_mutex_unlock(&u->lock);

    pthread_join(u->thread, 0);
    u->started = 0;
}

void uplink_print_stats(uplink_t *u)
{
    if (u == 0)
        return;

    log_info("uplink: enq=%llu pub=%llu drop=%llu fail=%llu reconn=%llu",
             (unsigned long long)u->enqueued,
             (unsigned long long)u->published,
             (unsigned long long)u->dropped,
             (unsigned long long)u->failed,
             (unsigned long long)u->reconnects);
}
