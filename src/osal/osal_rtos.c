#include "osal/osal.h"
#include "rtos.h"
#include "core/rtos_internal.h"   /* rtos_crit_enter/exit：统一内核临界区（零延迟 IRQ 接线） */
#include "common/lock.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * RTOS 版 OSAL：把 osal_sem 接到 jOS 调度器。
 *   - wait：count>0 时直接取走；count==0 时阻塞当前任务（让出 CPU），
 *           由 give（来自任务或 ISR）唤醒。驱动代码零改动。
 *   - 若在内核未启动前调用，退化为裸机忙等（保证启动期语义不变）。
 *   - 若在中断上下文调用 wait（不应发生），同样退化为忙等，避免死锁。
 * ------------------------------------------------------------------------- */

/* 读 SCB->ICSR 的 VECTACTIVE 域判断是否在 ISR 中（透过 port/lock 头，避免散落魔法地址） */
static int osal_in_interrupt(void) {
    return arch_in_isr();
}

void osal_sem_init(osal_sem_t *s, int val) {
    if (s) { s->count = val; s->wait = (void *)0; }
}

void osal_sem_wait(osal_sem_t *s) {
    if (!s) return;
    for (;;) {
        /* ISR 中或内核未启动：不能阻塞，退化为忙等 */
        if (osal_in_interrupt() || rtos_running() == (task_t *)0) {
            unsigned st = rtos_crit_enter();
            if (s->count > 0) { s->count--; rtos_crit_exit(st); return; }
            rtos_crit_exit(st);
            continue;
        }
        unsigned st = rtos_crit_enter();
        if (s->count > 0) {                 /* 有许可，直接取走 */
            s->count--;
            rtos_crit_exit(st);
            return;
        }
        /* 无许可：阻塞当前任务（请求切换；rtos_crit_exit 后 PendSV 真正切换） */
        rtos_pend(&s->wait);
        rtos_crit_exit(st);
        /* 被唤醒后从此处继续，循环重取许可 */
    }
}

void osal_sem_give(osal_sem_t *s) {
    if (!s) return;
    unsigned st = rtos_crit_enter();
    s->count++;
    if (rtos_running() != (task_t *)0) {
        rtos_post(&s->wait);   /* 唤醒最高优先级等待者并请求切换 */
    }
    rtos_crit_exit(st);
}

unsigned osal_enter_critical(void) { return irq_lock(); }
void     osal_exit_critical(unsigned state) { irq_unlock(state); }
