#include "rtos.h"
#include "rtos_internal.h"
#include "common/lock.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * SVC 系统调用分发（core/syscalls.c）：非特权任务访问内核对象的唯一受控入口。
 *
 * context.S 的 SVC_Handler 在“非启动类 SVC”时以 `b rtos_svc_dispatch_entry`
 * 尾调用进入本文件（lr 仍为 EXC_RETURN）。frame 是触发 SVC 的任务栈帧(PSP)，
 * frame[0..3] = r0..r3 = [调用号, 参数0, 参数1, 参数2]。运行在特权 Handler 模式，
 * 可直接操作内核对象；处理完把返回值写回 frame[0]，随 `bx lr` 带回用户态任务。
 * 全程 g_in_svc=1，故内部再调用的 rtos_sem_ 与 rtos_yield 等不会递归触发 SVC。
 * ------------------------------------------------------------------------- */

/* 当前任务是否需走 SVC 门：非特权 + 非 ISR + 非 SVC 重入。
 * 特权任务（含 ISR、内核启动前）一律直连内核，路径与历史完全一致。 */
int rtos_need_svc(void) {
    return !arch_in_isr() && !g_in_svc && g_running && !g_running->priv;
}

void rtos_svc_dispatch(uint32_t *frame, uint32_t nr) {
    uint32_t u0 = frame[1], u1 = frame[2], u2 = frame[3];   /* 用户参数(调用号在 frame[0]) */
    uint32_t ret = 0;
    switch (nr) {
    case RTOS_SYS_GET_TICK:  ret = rtos_tick_count(); break;
    case RTOS_SYS_YIELD:     rtos_yield(); break;
    case RTOS_SYS_MSLEEP:    rtos_msleep(u0); break;
    case RTOS_SYS_SEM_GIVE:
        if (rtos_kobj_validate((void *)u0, KOBJ_SEM)) rtos_sem_give((rtos_sem_t *)u0);
        else ret = (uint32_t)-1;
        break;
    case RTOS_SYS_SEM_WAIT:
        ret = rtos_kobj_validate((void *)u0, KOBJ_SEM)
              ? (uint32_t)rtos_sem_wait((rtos_sem_t *)u0) : (uint32_t)-1;
        break;
    case RTOS_SYS_MUTEX_LOCK:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MUTEX)
              ? (uint32_t)rtos_mutex_lock((rtos_mutex_t *)u0) : (uint32_t)-1;
        break;
    case RTOS_SYS_MUTEX_UNLOCK:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MUTEX)
              ? (uint32_t)rtos_mutex_unlock((rtos_mutex_t *)u0) : (uint32_t)-1;
        break;
    case RTOS_SYS_MUTEX_TRYLOCK:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MUTEX)
              ? (uint32_t)rtos_mutex_trylock((rtos_mutex_t *)u0) : (uint32_t)-1;
        break;
    case RTOS_SYS_MQ_SEND:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MQ)
              ? (uint32_t)rtos_mq_send((rtos_mq_t *)u0, (const void *)u1) : (uint32_t)-1;
        break;
    case RTOS_SYS_MQ_RECV:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MQ)
              ? (uint32_t)rtos_mq_recv((rtos_mq_t *)u0, (void *)u1) : (uint32_t)-1;
        break;
    case RTOS_SYS_SEM_TRYWAIT:
        ret = rtos_kobj_validate((void *)u0, KOBJ_SEM)
              ? (uint32_t)rtos_sem_trywait((rtos_sem_t *)u0) : (uint32_t)-1;
        break;
    case RTOS_SYS_MQ_TRYSEND:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MQ)
              ? (uint32_t)rtos_mq_trysend((rtos_mq_t *)u0, (const void *)u1) : (uint32_t)-1;
        break;
    case RTOS_SYS_MQ_TRYRECV:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MQ)
              ? (uint32_t)rtos_mq_tryrecv((rtos_mq_t *)u0, (void *)u1) : (uint32_t)-1;
        break;
    case RTOS_SYS_EVENT_SET:
        if (rtos_kobj_validate((void *)u0, KOBJ_EVENT)) rtos_event_set((rtos_event_t *)u0, u1);
        else ret = (uint32_t)-1;
        break;
    case RTOS_SYS_EVENT_WAIT: {
        int wait_all = (int)(u2 & 1u), block = (int)((u2 >> 1) & 1u);
        ret = rtos_kobj_validate((void *)u0, KOBJ_EVENT)
              ? rtos_event_wait((rtos_event_t *)u0, u1, wait_all, block) : (uint32_t)-1;
        break;
    }
    case RTOS_SYS_TASK_CREATE: {
        rtos_task_create_args_t *p = (rtos_task_create_args_t *)u0;
        rtos_task_create_ex(p->name, p->entry, p->arg, p->prio,
                            p->stack, p->stack_size, p->priv);
        break;
    }
    case RTOS_SYS_BUS_WAIT: {
        rtos_bus_wait_args_t *p = (rtos_bus_wait_args_t *)u0;
        ret = rtos_bus_wait(p->bus, p->topic, p->buf, p->len, p->timeout_ms);
        break;
    }
    case RTOS_SYS_BUS_PUBLISH: {
        rtos_bus_publish_args_t *p = (rtos_bus_publish_args_t *)u0;
        ret = rtos_bus_publish(p->bus, p->topic, p->data, p->len);
        break;
    }
    case RTOS_SYS_MUTEX_TIMEDLOCK:
        ret = rtos_kobj_validate((void *)u0, KOBJ_MUTEX)
              ? (uint32_t)rtos_mutex_timedlock((rtos_mutex_t *)u0, u1) : (uint32_t)-1;
        break;
    case RTOS_SYS_TASK_DELETE:
        rtos_task_delete((task_t *)u0);
        break;
    case RTOS_SYS_TASK_SET_PRIO:
        rtos_task_set_prio((task_t *)u0, (uint8_t)u1);
        break;
    case RTOS_SYS_TASK_SUSPEND:
        rtos_task_suspend((task_t *)u0);
        break;
    case RTOS_SYS_TASK_RESUME:
        rtos_task_resume((task_t *)u0);
        break;
    default: ret = (uint32_t)-1; break;
    }
    frame[0] = ret;   /* 返回值经 r0 带回用户态任务 */
}

/* 由汇编尾调用：置重入标志后分发，确保内部调用不递归 SVC。 */
void rtos_svc_dispatch_entry(uint32_t *frame, uint32_t nr) {
    g_in_svc = 1;
    rtos_svc_dispatch(frame, nr);
    g_in_svc = 0;
}
