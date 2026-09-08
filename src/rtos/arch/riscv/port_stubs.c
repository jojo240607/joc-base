/*
 * RISC-V (RV32IMC) port stubs — ARM Cortex-M MPU/stack-guard symbols.
 *
 * The jOS RTOS core and self-test files unconditionally reference a set of
 * MPU / stack overflow globals that only exist on Cortex-M (mpu.c).  On
 * RISC-V there is no MPU and no hardware stack-limit register, so these are
 * NOP stubs that keep the linker happy while never being functionally called
 * (RTOS_USE_MPU=0, the scheduler and self-tests check g_fault_cfsr but
 * nothing ever sets it).
 *
 * All symbols are weak where possible so a real MPU port can override them.
 */

#include "rtos.h"               /* task_t, RTOS_CCM_BSS */

/* ---- Newlib reentrancy: __getreent returns &_impure_data. The weak default
 * __getreent in libgloss returns NULL, which causes malloc to crash. ---- */
struct _reent;
extern struct _reent _impure_data;
struct _reent *__getreent(void) {
    return &_impure_data;
}

/* ---- Fault / overflow sticky flags (ARM CFSR analog) ---- */
volatile uint32_t RTOS_CCM_BSS g_fault_cfsr     = 0;
volatile int      RTOS_CCM_BSS g_stack_overflow = 0;

/* ---- Stack sentinel / watermark (ARM MPU stack-guard analog) ---- */
void rtos_stack_fill_watermark(task_t *t) { (void)t; }
void rtos_stack_fill_sentinel(task_t *t)   { (void)t; }
int  rtos_stack_check_sentinel(task_t *t)  { (void)t; return 0; }

/* ---- MPU region programming (ARM MPU analog, RTOS_USE_MPU=0 on RISC-V) ---- */
void rtos_mpu_init(void) {}
void rtos_mpu_set_stack_region(const task_t *t) { (void)t; }
void rtos_mpu_set_priv_region(int priv)         { (void)priv; }