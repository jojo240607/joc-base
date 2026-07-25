#ifndef JOC_RTOS_MPU_H
#define JOC_RTOS_MPU_H

#include <stdint.h>
#include "rtos.h"

/* ---------------------------------------------------------------------------
 * jOS MPU 支持（Cortex-M4 MPU，8 区域）
 *
 * 策略：任务保持特权模式运行（驱动直接访问外设，零改造）；MPU 启用并配置
 * 固定区域（flash RO+X / sram RW+nX / 外设 priv-RW），PRIVDEFENA=1 使特权代码
 * 拥有背景区，因此对现行特权任务完全透明、零回归；同时 MPU 真正限制“非特权”
 * 访问——用自测临时降到非特权去碰受保护区域，验证 MemFault 能被捕获并恢复。
 *
 * 另含栈哨兵：每个任务栈底填魔数，上下文切换时检查是否被踩，检测栈溢出。
 * ------------------------------------------------------------------------- */

void rtos_mpu_init(void);     /* 配置 3 个固定区域并使能 MPU + MemManage */
void rtos_mpu_enable(void);
void rtos_mpu_disable(void);

/* 栈哨兵（由 rtos_core.c 在创建/切换任务时调用） */
void rtos_stack_fill_sentinel(task_t *t);
int  rtos_stack_check_sentinel(task_t *t);   /* 返回 1 = 栈底被踩(溢出) */

/* 故障处理：MemManage_Handler 把栈帧交给它；返回非 0 表示已“恢复”（自测用） */
int  rtos_fault_handler(uint32_t *frame, uint32_t lr);

/* 运行时自测（RTOSMPU 命令触发） */
int  rtos_mpu_selftest(void);

/* 诊断导出（OpenOCD/串口可读） */
extern volatile int     g_mpu_violation;   /* 自测中成功捕获 MPU 越权 */
extern volatile int     g_mpu_test_active;  /* 自测进行中：故障处理器需恢复 */
extern volatile int     g_stack_overflow;   /* 检测到任务栈溢出 */
extern volatile uint32_t g_fault_cfsr;      /* 最近一次故障的 CFSR（调试） */

#endif /* JOC_RTOS_MPU_H */
