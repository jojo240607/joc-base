#ifndef JOC_BASE_DS_OBJPOOL_H
#define JOC_BASE_DS_OBJPOOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * 类型安全的静态对象池（编译期绑定元素类型 T）
 *
 * 与 ds/pool/pool.h 的区别：
 *   - pool    是“通用固定块内存池”，alloc 返回 void*，free 接受 void*，
 *             调用方必须自己强制转换，类型信息在接口处丢失；
 *   - objpool 在“编译期”由宏 OBJPOOL_DEFINE 绑定元素类型 T：后备存储就是
 *             T data[N] 数组，因此：
 *                * objpool_alloc  返回 T*（无需任何强制转换）；
 *                * objpool_free   接受 T*（传错类型会在编译期直接报错）。
 *             整个调用过程零强制转换、零堆分配（后备数组由调用方静态/栈声明）。
 *
 * 实现要点（嵌入式友好、O(1) 分配/释放）：
 *   - 对象存储 data[N] 是一段“干净”的 T 数组：绝不在对象体内写入任何元数据，
 *     保证对象内存布局与对齐完全由 T 决定，是真正的类型安全；
 *   - 空闲槽位用“下标栈” free_stack[N] 管理（存的是 data 的下标），LIFO；
 *   - occupied[N] 位图记录每个槽的占用状态，用于：
 *        * 拒绝“重复释放 / 释放未分配对象 / 越界指针”，返回 false（防破坏）；
 *        * clear 时一次性回收全部对象。
 *
 * 用法示例：
 *   typedef struct { int id; char name[8]; } task_t;
 *   OBJPOOL_DEFINE(task_pool, task_t, 16);   // 声明一个 16 容量的 task_t 对象池
 *
 *   task_pool_t pool;
 *   task_pool_init(&pool);
 *   task_t *t = task_pool_alloc(&pool);      // 返回 task_t*，满则 NULL
 *   if (t) { t->id = 1; strcpy(t->name, "a"); }
 *   task_pool_free(&pool, t);                // 接受 task_t*，传错类型编译报错
 * ------------------------------------------------------------------------- */

/* 声明一个名为 NAME 的“类型安全对象池”：
 *   - 生成类型  NAME##_t         （池实例类型，可直接静态/栈声明）
 *   - 生成函数  NAME##_init      （重置为空池）
 *   - 生成函数  NAME##_alloc     （分配一个 T 对象，返回 T*，满则 NULL）
 *   - 生成函数  NAME##_free      （释放 T 对象，返回是否成功）
 *   - 生成函数  NAME##_clear     （回收全部，重置占用位图）
 *   - 生成函数  NAME##_get       （按下标取 T*，用于遍历/索引访问）
 *   - 生成函数  NAME##_is_occupied（下标是否被分配）
 *   - 生成函数  NAME##_index_of  （T* -> 下标，越界返回 (size_t)-1）
 *   - 生成函数  NAME##_capacity / NAME##_count / NAME##_available
 *   - 生成函数  NAME##_is_empty / NAME##_is_full
 *   - 生成宏    NAME##_FOREACH   （遍历所有已分配对象：T *it; NAME##_FOREACH(&p, it){...}）
 *
 * 约束：N 必须为正整数常量表达式；T 可为任意完整类型（含结构体/联合体）。
 */
#define OBJPOOL_DEFINE(NAME, T, N) \
    typedef struct { \
        T        data[(N)];          /* 对象存储：干净的 T 数组，绝不写入元数据 */ \
        size_t   free_stack[(N)];    /* 空闲下标栈（栈内存的是 data 的下标） */ \
        uint8_t  occupied[(N)];      /* 占用位图：1=已分配，0=空闲 */ \
        size_t   free_count;         /* 空闲栈深度 */ \
        size_t   used;               /* 已分配对象数 */ \
    } NAME##_t; \
    \
    static inline void NAME##_init(NAME##_t *self) { \
        for (size_t _i = 0; _i < (size_t)(N); _i++) { \
            self->free_stack[_i] = (size_t)(N) - 1U - _i; \
            self->occupied[_i]   = 0U; \
        } \
        self->free_count = (size_t)(N); \
        self->used       = 0U; \
    } \
    \
    static inline T *NAME##_alloc(NAME##_t *self) { \
        if (self->free_count == 0U) return (T *)0;            /* 池满 */ \
        size_t idx = self->free_stack[--self->free_count];    /* LIFO 取空闲下标 */ \
        self->occupied[idx] = 1U; \
        self->used++; \
        return &self->data[idx]; \
    } \
    \
    static inline bool NAME##_free(NAME##_t *self, T *ptr) { \
        if (!ptr) return false; \
        size_t idx = (size_t)(ptr - self->data); \
        if (idx >= (size_t)(N))     return false;   /* 越界指针 */ \
        if (self->occupied[idx] == 0U) return false; /* 重复释放 / 未分配 */ \
        self->occupied[idx] = 0U; \
        self->free_stack[self->free_count++] = idx; /* 归还到空闲栈 */ \
        self->used--; \
        return true; \
    } \
    \
    static inline void NAME##_clear(NAME##_t *self) { \
        NAME##_init(self); \
    } \
    \
    static inline T *NAME##_get(NAME##_t *self, size_t idx) { \
        if (idx >= (size_t)(N)) return (T *)0; \
        return &self->data[idx]; \
    } \
    \
    static inline bool NAME##_is_occupied(const NAME##_t *self, size_t idx) { \
        if (idx >= (size_t)(N)) return false; \
        return self->occupied[idx] != 0U; \
    } \
    \
    static inline size_t NAME##_index_of(const NAME##_t *self, const T *ptr) { \
        if (!ptr) return (size_t)-1; \
        size_t idx = (size_t)(ptr - self->data); \
        if (idx >= (size_t)(N)) return (size_t)-1; \
        return idx; \
    } \
    \
    static inline size_t NAME##_capacity(const NAME##_t *self) { \
        (void)self; return (size_t)(N); \
    } \
    \
    static inline size_t NAME##_count(const NAME##_t *self) { \
        return self->used; \
    } \
    \
    static inline size_t NAME##_available(const NAME##_t *self) { \
        return (size_t)(N) - self->used; \
    } \
    \
    static inline bool NAME##_is_empty(const NAME##_t *self) { \
        return self->used == 0U; \
    } \
    \
    static inline bool NAME##_is_full(const NAME##_t *self) { \
        return self->used == (size_t)(N); \
    } \
    \
    /* 遍历所有“已分配”对象：调用方先声明 T *it; 再 NAME##_FOREACH(&pool, it){...} */ \
    static inline T *NAME##_next(NAME##_t *self, size_t *cur) { \
        for (size_t _i = *cur; _i < (size_t)(N); _i++) { \
            if (self->occupied[_i]) { *cur = _i + 1U; return &self->data[_i]; } \
        } \
        *cur = (size_t)(N); \
        return (T *)0; \
    }

/* 简化遍历：T *it; NAME##_FOREACH(&pool, it) { ... 使用 it ... } */
#define OBJPOOL_FOREACH(NAME, self, it) \
    for (size_t _op_cur = 0; \
         ((it) = NAME##_next((self), &_op_cur)) != (void *)0; )

#endif /* JOC_BASE_DS_OBJPOOL_H */
