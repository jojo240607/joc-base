#include "rtos.h"
#include "rtos_internal.h"
#include "common/lock.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * 事件总线（RTOS 一等 IPC 原语：阻塞式 topic 邮箱 + 广播唤醒）（core/ipc_bus.c）
 *
 * 详见 rtos.h 该段注释。每 topic 一个固定大小邮箱，发布覆盖最新一条；等待者
 * 被唤醒后从邮箱拷贝载荷。发布时广播唤醒该 topic 上所有等待任务（pub/sub）。
 * 阻塞变体仅任务上下文可用；ISR/未启动的等待调用返回 -1（不可阻塞）。发布为
 * ISR 安全（仅拷贝+唤醒，不阻塞）。非特权任务经 SVC 门执行（KOBJ_BUS 校验指针）。
 * ------------------------------------------------------------------------- */
void rtos_bus_init(rtos_bus_t *b, uint16_t max_topics, size_t item_size,
                   void *buf, size_t buf_size) {
    if (!b || max_topics == 0 || item_size == 0 || !buf) return;
    size_t need = (size_t)max_topics * item_size
                + (size_t)max_topics * sizeof(uint16_t)   /* mlen */
                + (size_t)max_topics * sizeof(uint16_t)   /* mpend */
                + (size_t)max_topics * sizeof(void *);    /* waitq */
    if (buf_size < need) return;                          /* 缓冲不足：总线保持空 */
    b->max_topics = max_topics;
    b->item_size  = (uint16_t)item_size;
    b->mbuf  = (uint8_t *)buf;
    b->mlen  = (uint16_t *)((uint8_t *)buf + (size_t)max_topics * item_size);
    b->mpend = (uint16_t *)((uint8_t *)b->mlen + (size_t)max_topics * sizeof(uint16_t));
    b->waitq = (void **)((uint8_t *)b->mpend + (size_t)max_topics * sizeof(uint16_t));
    memset(b->mbuf,  0, (size_t)max_topics * item_size);
    memset(b->mlen,  0, (size_t)max_topics * sizeof(uint16_t));
    memset(b->mpend, 0, (size_t)max_topics * sizeof(uint16_t));
    memset(b->waitq, 0, (size_t)max_topics * sizeof(void *));
    rtos_kobj_register((const char *)0, KOBJ_BUS, b);
}

/* 从 topic 邮箱拷贝载荷到用户缓冲并清除“待取”标志（调用方持锁） */
static void bus_copy_out(rtos_bus_t *b, uint16_t topic, void *buf, size_t *len) {
    size_t n = b->mlen[topic];
    memcpy(buf, b->mbuf + (size_t)topic * b->item_size, n);
    *len = n;
    b->mpend[topic] = 0;
}

int rtos_bus_wait(rtos_bus_t *b, uint16_t topic, void *buf, size_t *len, uint32_t timeout_ms) {
    if (!b || !buf || !len || topic >= b->max_topics) return -1;
    if (rtos_need_svc()) {
        rtos_bus_wait_args_t a = { b, topic, buf, len, timeout_ms };
        return (int)rtos_syscall(RTOS_SYS_BUS_WAIT, (uint32_t)&a, 0, 0);
    }
    if (rtos_ipc_in_isr() || !rtos_is_started()) return -1;   /* ISR/未启动：不可阻塞 */
    unsigned st = irq_lock();
    if (b->mpend[topic]) {            /* 已有待取消息：立即取走 */
        bus_copy_out(b, topic, buf, len);
        irq_unlock(st);
        return 0;
    }
    if (timeout_ms == 0) { irq_unlock(st); return -1; }   /* 非阻塞：无消息 */
    rtos_pend(&b->waitq[topic]);      /* 阻塞，直到该 topic 被发布后唤醒 */
    irq_unlock(st);
    /* 被唤醒：发布者已把数据放进邮箱 */
    unsigned st2 = irq_lock();
    bus_copy_out(b, topic, buf, len);
    irq_unlock(st2);
    return 0;
}

int rtos_bus_publish(rtos_bus_t *b, uint16_t topic, const void *data, size_t len) {
    if (!b || topic >= b->max_topics || !data) return -1;
    if (rtos_need_svc()) {
        rtos_bus_publish_args_t a = { b, topic, data, len };
        return (int)rtos_syscall(RTOS_SYS_BUS_PUBLISH, (uint32_t)&a, 0, 0);
    }
    unsigned st = irq_lock();
    size_t n = (len > b->item_size) ? b->item_size : len;
    memcpy(b->mbuf + (size_t)topic * b->item_size, data, n);
    b->mlen[topic]  = (uint16_t)n;
    b->mpend[topic] = 1;
    /* 广播唤醒该 topic 上所有等待者（pub/sub 语义：一个发布多订阅者皆得） */
    task_t *t;
    while ((t = rtos_waitq_pop_highest(&b->waitq[topic])) != (task_t *)0) {
        t->wait_obj = (void *)0;
        t->state = TASK_READY;
        ready_add(t);
    }
    irq_unlock(st);
    rtos_schedule_request();
    return 0;
}
