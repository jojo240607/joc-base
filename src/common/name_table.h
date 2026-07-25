#ifndef JOC_BASE_COMMON_NAME_TABLE_H
#define JOC_BASE_COMMON_NAME_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * 类型安全名字表（name_table）
 *
 * 通用“名字 -> 指针”注册表，编译期定大小、零堆分配。用途：
 *   - RTOS 内核对象注册表（kobj）：按名查找 sem/mutex/mq/event/task；
 *   - task_get("net_rx") 按名取任务；
 *   - 任何需要“字符串句柄”的场景。
 *
 * 与 ds/objpool/objpool.h 同一思路：宏 NAME_TABLE_DEFINE 在编译期绑定元素
 * 类型 T，find 返回 T*（传错类型编译报错），且后备数组静态声明，确定性好、
 * 可被 MPU 映射为特权区。不同于 devmgr（device 专用、线性查找），本表通用。
 *
 * 约束：N 为正整数常量表达式；名字字符串由调用方保证生命周期（通常取
 *       静态常量字符串）；比较用 strcmp，区分大小写。
 * ------------------------------------------------------------------------- */

#define NAME_TABLE_DEFINE(NAME, T, N) \
    typedef struct { \
        const char *names[(N)];   /* 名字（NULL=空槽）*/ \
        T          *ptrs[(N)];    /* 对应指针 */ \
        size_t      count;        /* 当前条目数 */ \
    } NAME##_t; \
    \
    static inline void NAME##_init(NAME##_t *self) { \
        if (!self) return; \
        for (size_t _i = 0; _i < (size_t)(N); _i++) { \
            self->names[_i] = (const char *)0; \
            self->ptrs[_i]  = (T *)0; \
        } \
        self->count = 0U; \
    } \
    \
    static inline bool NAME##_register(NAME##_t *self, const char *name, T *ptr) { \
        if (!self || !name || !ptr) return false; \
        if (self->count >= (size_t)(N)) return false;               /* 表满 */ \
        for (size_t _i = 0; _i < self->count; _i++) { \
            if (self->names[_i] && strcmp(self->names[_i], name) == 0) \
                return false;                                        /* 重名拒绝 */ \
        } \
        self->names[self->count] = name; \
        self->ptrs[self->count]  = ptr; \
        self->count++; \
        return true; \
    } \
    \
    static inline T *NAME##_find(NAME##_t *self, const char *name) { \
        if (!self || !name) return (T *)0; \
        for (size_t _i = 0; _i < self->count; _i++) { \
            if (self->names[_i] && strcmp(self->names[_i], name) == 0) \
                return self->ptrs[_i]; \
        } \
        return (T *)0; \
    } \
    \
    static inline bool NAME##_unregister(NAME##_t *self, const char *name) { \
        if (!self || !name) return false; \
        for (size_t _i = 0; _i < self->count; _i++) { \
            if (self->names[_i] && strcmp(self->names[_i], name) == 0) { \
                /* 用末项填补空洞，保持前 count-1 项连续（不保序但 O(1)）*/ \
                size_t _last = self->count - 1U; \
                if (_i != _last) { \
                    self->names[_i] = self->names[_last]; \
                    self->ptrs[_i]  = self->ptrs[_last]; \
                } \
                self->names[_last] = (const char *)0; \
                self->ptrs[_last]  = (T *)0; \
                self->count--; \
                return true; \
            } \
        } \
        return false; \
    } \
    \
    static inline T *NAME##_get(NAME##_t *self, size_t idx) { \
        if (!self || idx >= self->count) return (T *)0; \
        return self->ptrs[idx]; \
    } \
    \
    static inline const char *NAME##_name_of(NAME##_t *self, size_t idx) { \
        if (!self || idx >= self->count) return (const char *)0; \
        return self->names[idx]; \
    } \
    \
    static inline size_t NAME##_count(const NAME##_t *self) { \
        return self ? self->count : 0U; \
    } \
    \
    static inline bool NAME##_is_full(const NAME##_t *self) { \
        return self ? (self->count >= (size_t)(N)) : true; \
    } \
    \
    static inline void NAME##_clear(NAME##_t *self) { \
        NAME##_init(self); \
    }

/* 遍历所有已注册条目：先声明 T *it; const char *nm; 再 NAME##_FOREACH(&t, nm, it){...} */
#define NAME_TABLE_FOREACH(NAME, self, nm, it) \
    for (size_t _nt_cur = 0; \
         (_nt_cur < (self)->count) && \
         ((nm) = (self)->names[_nt_cur], (it) = (self)->ptrs[_nt_cur], true); \
         _nt_cur++)

#endif /* JOC_BASE_COMMON_NAME_TABLE_H */
