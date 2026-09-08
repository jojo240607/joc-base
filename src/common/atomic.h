#ifndef JOC_BASE_COMMON_ATOMIC_H
#define JOC_BASE_COMMON_ATOMIC_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * 原子操作原语（atomic）
 *
 * 用途：ISR 与任务共享计数 / 无锁数据结构的提交与可见性 / 内核统计。
 * 这是对 common 现有无锁类（spsc / ringbuffer）的必要补充——它们目前
 * 仅靠“单生产者单消费者”约定保证安全，一旦有多生产者就必须有真正的原子。
 *
 * 平台策略（自包含，不依赖 cmsis，host 与 arm 均可编译）：
 *   - ARM Cortex-M3/M4/M7 (ARMv7-M / ARMv7E-M)：使用 LDREX/STREX 独占访问 +
 *     轻量 DMB 实现 acquire/release 语义；
 *   - 其它（host x86 等）：使用 GCC/Clang 的 __atomic 内建（需要 -std=gnu11
 *     或 c11 的 <stdatomic.h>，此处用内建以保证对老工具链兼容）。
 *
 * 注意：本原语在单核 Cortex-M 上提供“跨异常级别（ISR/任务）安全”的原子性；
 *       真正的多核/多硬件线程并发需额外 DMB 域控制，当前目标平台为单核 M4，
 *       因此不做多核扩展。
 * ------------------------------------------------------------------------- */

/* 平台判定：是否为 ARMv7-M 系列（含 LDREX/STREX） */
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || \
    (defined(__CORTEX_M) && __CORTEX_M >= 3)
  #define ATOMIC_ARMv7M 1
#elif defined(__riscv)
  /* RV32IMC 无原子指令（无 A 扩展；__atomic 内建会生成 amoswap 等，链接/运行
   * 均失败）：单核下用 irq_lock 临界区近似，语义等价（关中断窗口极短）。 */
  #define ATOMIC_RISCV 1
  #include "common/lock.h"   /* irq_lock/irq_unlock（仅 RISC-V 分支展开） */
#endif

/* ---------------- ARM 路径的底层内联 ---------------- */
#ifdef ATOMIC_ARMv7M
  /* 数据内存屏障：保证前面的访问在后面的访问之前完成（release/acquire 用） */
  #define atomic_dmb()  __asm__ volatile("dmb" ::: "memory")
  /* 独占加载 / 独占存储（返回 0=成功, 1=失败需重试）*/
  static inline uint32_t atomic_ldrex(volatile uint32_t *addr) {
      uint32_t v;
      __asm__ volatile("ldrex %0, [%1]" : "=r"(v) : "r"(addr) : "memory");
      return v;
  }
  static inline uint32_t atomic_strex(uint32_t val, volatile uint32_t *addr) {
      uint32_t res;
      __asm__ volatile("strex %0, %1, [%2]" : "=r"(res) : "r"(val), "r"(addr) : "memory");
      return res;
  }
  static inline void atomic_clrex(void) {
      __asm__ volatile("clrex" ::: "memory");
  }
#else
  /* host / 非 arm：用编译器屏障近似 DMB（真实内存序由 __atomic 内建保证） */
  #define atomic_dmb()  __asm__ volatile("" ::: "memory")
#endif

/* ---------------------------------------------------------------------------
 * 原子字（32 位）。内部用 volatile，所有访问都经原子接口，禁止直接裸读。
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t v;
} atomic_u32_t;

static inline void atomic_u32_init(atomic_u32_t *a, uint32_t val) {
    a->v = val;
}

/* 读（acquire 语义）*/
static inline uint32_t atomic_u32_load(const atomic_u32_t *a) {
#ifdef ATOMIC_ARMv7M
    atomic_dmb();
    uint32_t v = a->v;
    atomic_dmb();
    return v;
#elif defined(ATOMIC_RISCV)
    irq_state_t st = irq_lock();
    uint32_t v = a->v;
    irq_unlock(st);
    return v;
#else
    return __atomic_load_n(&((atomic_u32_t *)a)->v, __ATOMIC_ACQUIRE);
#endif
}

/* 写（release 语义）*/
static inline void atomic_u32_store(atomic_u32_t *a, uint32_t val) {
#ifdef ATOMIC_ARMv7M
    atomic_dmb();
    a->v = val;
    atomic_dmb();
#elif defined(ATOMIC_RISCV)
    irq_state_t st = irq_lock();
    a->v = val;
    irq_unlock(st);
#else
    __atomic_store_n(&a->v, val, __ATOMIC_RELEASE);
#endif
}

/* 取原值并加（返回修改前的旧值）*/
static inline uint32_t atomic_u32_fetch_add(atomic_u32_t *a, uint32_t val) {
#ifdef ATOMIC_ARMv7M
    uint32_t old, res;
    do {
        old = atomic_ldrex(&a->v);
        res = atomic_strex(old + val, &a->v);
    } while (res != 0U);
    atomic_dmb();
    return old;
#elif defined(ATOMIC_RISCV)
    irq_state_t st = irq_lock();
    uint32_t old = a->v;
    a->v = old + val;
    irq_unlock(st);
    return old;
#else
    return __atomic_fetch_add(&a->v, val, __ATOMIC_ACQ_REL);
#endif
}

/* 取原值并减 */
static inline uint32_t atomic_u32_fetch_sub(atomic_u32_t *a, uint32_t val) {
#ifdef ATOMIC_ARMv7M
    uint32_t old, res;
    do {
        old = atomic_ldrex(&a->v);
        res = atomic_strex(old - val, &a->v);
    } while (res != 0U);
    atomic_dmb();
    return old;
#elif defined(ATOMIC_RISCV)
    irq_state_t st = irq_lock();
    uint32_t old = a->v;
    a->v = old - val;
    irq_unlock(st);
    return old;
#else
    return __atomic_fetch_sub(&a->v, val, __ATOMIC_ACQ_REL);
#endif
}

/* 取原值并按位或 */
static inline uint32_t atomic_u32_fetch_or(atomic_u32_t *a, uint32_t val) {
#ifdef ATOMIC_ARMv7M
    uint32_t old, res;
    do {
        old = atomic_ldrex(&a->v);
        res = atomic_strex(old | val, &a->v);
    } while (res != 0U);
    atomic_dmb();
    return old;
#elif defined(ATOMIC_RISCV)
    irq_state_t st = irq_lock();
    uint32_t old = a->v;
    a->v = old | val;
    irq_unlock(st);
    return old;
#else
    return __atomic_fetch_or(&a->v, val, __ATOMIC_ACQ_REL);
#endif
}

/* 取原值并按位与 */
static inline uint32_t atomic_u32_fetch_and(atomic_u32_t *a, uint32_t val) {
#ifdef ATOMIC_ARMv7M
    uint32_t old, res;
    do {
        old = atomic_ldrex(&a->v);
        res = atomic_strex(old & val, &a->v);
    } while (res != 0U);
    atomic_dmb();
    return old;
#elif defined(ATOMIC_RISCV)
    irq_state_t st = irq_lock();
    uint32_t old = a->v;
    a->v = old & val;
    irq_unlock(st);
    return old;
#else
    return __atomic_fetch_and(&a->v, val, __ATOMIC_ACQ_REL);
#endif
}

/* 交换：写入 val，返回旧值 */
static inline uint32_t atomic_u32_swap(atomic_u32_t *a, uint32_t val) {
#ifdef ATOMIC_ARMv7M
    uint32_t old, res;
    do {
        old = atomic_ldrex(&a->v);
        res = atomic_strex(val, &a->v);
    } while (res != 0U);
    atomic_dmb();
    return old;
#elif defined(ATOMIC_RISCV)
    irq_state_t st = irq_lock();
    uint32_t old = a->v;
    a->v = val;
    irq_unlock(st);
    return old;
#else
    return __atomic_exchange_n(&a->v, val, __ATOMIC_ACQ_REL);
#endif
}

/* 比较并交换：若当前值 == *expected，则写入 desired，返回 true；
 *              否则把 *expected 更新为当前值，返回 false。 */
static inline bool atomic_u32_compare_exchange(atomic_u32_t *a,
                                               uint32_t *expected,
                                               uint32_t desired) {
#ifdef ATOMIC_ARMv7M
    uint32_t old = atomic_ldrex(&a->v);
    if (old != *expected) {
        atomic_clrex();
        *expected = old;
        return false;
    }
    uint32_t res = atomic_strex(desired, &a->v);
    if (res != 0U) {
        /* 独占失败但值匹配，重试整个流程 */
        uint32_t e = *expected;
        return atomic_u32_compare_exchange(a, &e, desired);
    }
    atomic_dmb();
    return true;
#elif defined(ATOMIC_RISCV)
    irq_state_t st = irq_lock();
    uint32_t old = a->v;
    if (old != *expected) {
        *expected = old;
        irq_unlock(st);
        return false;
    }
    a->v = desired;
    irq_unlock(st);
    return true;
#else
    return __atomic_compare_exchange_n(&a->v, expected, desired,
                                       false /*weak*/,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#endif
}

/* ---------------------------------------------------------------------------
 * 便捷封装
 * ------------------------------------------------------------------------- */
static inline uint32_t atomic_u32_inc(atomic_u32_t *a) {
    return atomic_u32_fetch_add(a, 1U);
}
static inline uint32_t atomic_u32_dec(atomic_u32_t *a) {
    return atomic_u32_fetch_sub(a, 1U);
}
/* 测试并设置某一位，返回该位设置前的状态（0 或 1）*/
static inline uint32_t atomic_u32_test_and_set_bit(atomic_u32_t *a, uint32_t bit) {
    uint32_t mask = (uint32_t)1U << bit;
    uint32_t old = atomic_u32_fetch_or(a, mask);
    return (old & mask) ? 1U : 0U;
}
/* 清除某一位，返回该位清除前的状态 */
static inline uint32_t atomic_u32_test_and_clear_bit(atomic_u32_t *a, uint32_t bit) {
    uint32_t mask = (uint32_t)1U << bit;
    uint32_t old = atomic_u32_fetch_and(a, ~mask);
    return (old & mask) ? 1U : 0U;
}

#endif /* JOC_BASE_COMMON_ATOMIC_H */
