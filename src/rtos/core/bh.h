#ifndef JOC_RTOS_BH_H
#define JOC_RTOS_BH_H

#include <stddef.h>
#include <stdint.h>
#include "rtos_config.h"

/* ---------------------------------------------------------------------------
 * jOS 中断上下半部（Top-Half / Bottom-Half）
 *
 * 上半部（ISR，Handler 模式，永远特权）：只做原子、快速的事——读/清外设状态、
 * 把数据推入 SPSC ringbuffer、然后 rtos_bh_trigger() / rtos_work_submit() 唤醒
 * 下半部。绝不阻塞、绝不忙等、绝不做耗时逻辑。
 *
 * 下半部（高优先级任务，任务模式）：承接耗时/可能阻塞的逻辑（协议解析、缓冲
 * 整理、向别的任务发消息），随时可被更高优先级 IRQ/任务抢占，绝不长时间霸占
 * CPU。这样任何单个中断都不会让系统卡死。
 *
 * 设计要点（见 docs/rtos-design.md 第 4 章）：
 *  - 上半部通过 rtos_bh_trigger()（= ISR 安全的 rtos_sem_give + 请求调度）唤醒
 *    一个专属的高优先级 BH 任务（A）；或把工作挂入共享工作队列(B)。
 *  - 数据通道推荐用 SPSC ringbuffer（上半部写 head、下半部读 tail，零锁），
 *    这正是 stream_device.rx_rb 已在用的模式，直接沿用。
 * ------------------------------------------------------------------------- */

/* (A) BH 任务：每个驱动/模块可拥有专属的高优先级下半部任务 */
typedef struct bh bh_t;

/* 创建一个下半部任务。
 *  name      : 任务名（同名重复创建会复用已有任务，便于驱动 init / 重复自测）
 *  prio      : 下半部优先级（建议 RTOS_PRIO_BH_HIGH / RTOS_PRIO_BH_MED）
 *  stack     : 任务栈（调用方提供，8 字节对齐）
 *  stack_size: 栈字节数
 *  fn        : 下半部处理函数（被触发时调用，ctx 透传）
 *  ctx       : 透传给 fn 的用户上下文
 * 返回 bh 句柄（用作 rtos_bh_trigger 的参数）；失败返回 NULL。 */
bh_t *rtos_bh_task_create(const char *name, uint8_t prio,
                          void *stack, size_t stack_size,
                          void (*fn)(void *), void *ctx);

/* 上半部唤醒下半部：ISR 安全。每个调用对应一次下半部 fn 调用（计数信号量）。 */
void rtos_bh_trigger(bh_t *bh);

/* 下半部在 fn 内可主动等待（高级用法：自行编写的 BH 循环）；任务上下文调用。 */
void rtos_bh_wait(bh_t *bh);

/* (B) 工作队列：省 RAM 的共享变体，多个 work 共用一组 worker 任务。
 * 从 ISR/任务提交一个延迟工作；worker 任务（RTOS_PRIO_BH_MED）随后执行 fn(arg)。 */
typedef struct work {
    struct work *next;
    void (*fn)(void *);
    void *arg;
} rtos_work_t;

void rtos_work_submit(rtos_work_t *w);

/* 共享 worker 初始化：由 rtos_start() 在任务上下文调用一次（提前建好 wq 任务），
 * 使 rtos_work_submit 可安全地从 ISR 调用（不会在中断上下文建任务）。 */
void rtos_workq_init(void);

#if RTOS_SELFTEST
/* 运行时自测（RTOSBH 命令触发；并注册进 RTOSALL） */
int rtos_bh_selftest(void);
#endif /* RTOS_SELFTEST */

#endif /* JOC_RTOS_BH_H */
