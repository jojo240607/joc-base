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
        /* ISR 中或内核未启动：不能阻塞，退化为忙等。
         * 注意：忙等阶段“绝不能”用 rtos_crit_enter（BASEPRI）关门——BASEPRI 会屏蔽
         * 那个“负责给本信号量”的内核类 ISR（优先级 >= 阈值），导致它永远无法触发、
         * 信号量永远拿不到、死循环且 BASEPRI 卡在阈值（连 UART TX 中断都被挡，串口
         * 直接不输出）。正确做法：仅对“取走许可”这一瞬间用极短的 PRIMASK 关门保证原子，
         * 两次取之间的自旋本身保持中断开启，让给信号量的 ISR 能在自旋间隙 firing，
         * 与 ISR 实际优先级无关，BASEPRI 选择性屏蔽下也成立。 */
        if (osal_in_interrupt() || rtos_running() == (task_t *)0) {
            unsigned st = irq_lock();
            if (s->count > 0) { s->count--; irq_unlock(st); return; }
            irq_unlock(st);
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
