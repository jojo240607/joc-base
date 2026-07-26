#include "rtos.h"
#include "rtos_internal.h"
#include "common/lock.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * 消息队列（定长项环形缓冲）（core/ipc_mq.c）
 * ------------------------------------------------------------------------- */
void rtos_mq_init(rtos_mq_t *q, void *buf, size_t item_size, size_t cap) {
    if (!q) return;
    q->buf = (uint8_t *)buf;
    q->item_size = item_size;
    q->cap = cap;
    q->count = 0;
    q->head = 0;
    q->recv_waitq = (void *)0;
    q->send_waitq = (void *)0;
    rtos_kobj_register((const char *)0, KOBJ_MQ, q);
}

static void mq_push(rtos_mq_t *q, const void *item) {
    uint8_t *dst = q->buf + q->head * q->item_size;
    memcpy(dst, item, q->item_size);
    q->head = (q->head + 1) % q->cap;
    q->count++;
}
static void mq_pop(rtos_mq_t *q, void *item) {
    size_t tail = (q->head + q->cap - q->count) % q->cap;
    uint8_t *src = q->buf + tail * q->item_size;
    memcpy(item, src, q->item_size);
    q->count--;
}

int rtos_mq_trysend(rtos_mq_t *q, const void *item) {
    if (!q) return -1;
    if (rtos_need_svc()) return (int)rtos_syscall(RTOS_SYS_MQ_TRYSEND, (uint32_t)q, (uint32_t)item, 0);
    unsigned st = irq_lock();
    if (q->count < q->cap) {
        mq_push(q, item);
        if (q->recv_waitq) {                  /* 有接收者空等：唤醒一个去取 */
            task_t *t = rtos_waitq_pop_highest(&q->recv_waitq);
            t->wait_obj = (void *)0; t->state = TASK_READY; ready_add(t);
            irq_unlock(st);
            rtos_schedule_request();
            return 0;
        }
        irq_unlock(st);
        return 0;
    }
    irq_unlock(st);
    return -1;
}

int rtos_mq_tryrecv(rtos_mq_t *q, void *item) {
    if (!q) return -1;
    if (rtos_need_svc()) return (int)rtos_syscall(RTOS_SYS_MQ_TRYRECV, (uint32_t)q, (uint32_t)item, 0);
    unsigned st = irq_lock();
    if (q->count > 0) {
        mq_pop(q, item);
        if (q->send_waitq) {                  /* 有发送者满等：唤醒一个去填 */
            task_t *t = rtos_waitq_pop_highest(&q->send_waitq);
            t->wait_obj = (void *)0; t->state = TASK_READY; ready_add(t);
            irq_unlock(st);
            rtos_schedule_request();
            return 0;
        }
        irq_unlock(st);
        return 0;
    }
    irq_unlock(st);
    return -1;
}

int rtos_mq_send(rtos_mq_t *q, const void *item) {
    if (!q) return -1;
    if (rtos_need_svc()) return (int)rtos_syscall(RTOS_SYS_MQ_SEND, (uint32_t)q, (uint32_t)item, 0);
    if (rtos_ipc_in_isr() || !rtos_is_started()) return rtos_mq_trysend(q, item);
    for (;;) {
        unsigned st = irq_lock();
        if (q->count < q->cap) {
            mq_push(q, item);
            if (q->recv_waitq) {
                task_t *t = rtos_waitq_pop_highest(&q->recv_waitq);
                t->wait_obj = (void *)0; t->state = TASK_READY; ready_add(t);
                irq_unlock(st);
                rtos_schedule_request();
                return 0;
            }
            irq_unlock(st);
            return 0;
        }
        rtos_pend(&q->send_waitq);   /* 满：阻塞，被接收者唤醒后重试 */
        irq_unlock(st);
    }
}

int rtos_mq_recv(rtos_mq_t *q, void *item) {
    if (!q) return -1;
    if (rtos_need_svc()) return (int)rtos_syscall(RTOS_SYS_MQ_RECV, (uint32_t)q, (uint32_t)item, 0);
    if (rtos_ipc_in_isr() || !rtos_is_started()) return rtos_mq_tryrecv(q, item);
    for (;;) {
        unsigned st = irq_lock();
        if (q->count > 0) {
            mq_pop(q, item);
            if (q->send_waitq) {
                task_t *t = rtos_waitq_pop_highest(&q->send_waitq);
                t->wait_obj = (void *)0; t->state = TASK_READY; ready_add(t);
                irq_unlock(st);
                rtos_schedule_request();
                return 0;
            }
            irq_unlock(st);
            return 0;
        }
        rtos_pend(&q->recv_waitq);   /* 空：阻塞，被发送者唤醒后重试 */
        irq_unlock(st);
    }
}
