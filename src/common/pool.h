#ifndef JOC_BASE_DS_POOL_H
#define JOC_BASE_DS_POOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* 不透明固定块内存池对象
 *
 * 设计目标：嵌入式场景下的“静态内存池分配器”。
 *  - 静态分配：调用方提供后备缓冲区（RAM 数组），由 pool_init 绑定并切分为 N 个固定块；
 *  - 堆分配：  用 pool_create 让对象自行为缓冲区与对象 malloc（MCU 上可选）。
 * 两种用法均通过同一套 pool 对象接口操作，行为可被派生类型覆写（多态）。
 *
 * 空闲块以单链表管理：每个空闲块的前 sizeof(void*) 字节直接存放“下一个空闲块”指针，
 * 因此不额外占用元数据，池可容纳的块数仅由后备缓冲区大小与块大小决定。
 */
typedef struct pool pool;

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * 内存池的全部能力都在虚表里，pool 成为可派生的对象。
 * 所有公开方法只能通过 self->fun->method(self, ...) 分派。
 * ------------------------------------------------------------------------- */
struct poolFun {
    /* 分配一个固定块，返回块指针；池满返回 NULL */
    void *(*alloc)(pool *self);
    /* 释放之前分配的块，成功返回 true（ptr 须属于本池且按块对齐）*/
    bool (*free)(pool *self, void *ptr);
    /* 判断 ptr 是否属于本池（落在缓冲区范围内且按块对齐）*/
    bool (*in_pool)(const pool *self, const void *ptr);

    /* 计数 */
    size_t (*capacity)(const pool *self);  /* 块总数 */
    size_t (*used)(const pool *self);      /* 已分配块数 */
    size_t (*available)(const pool *self); /* 空闲块数 */
    bool (*empty)(const pool *self);       /* 全部空闲 */
    bool (*full)(const pool *self);        /* 无空闲块 */

    /* 重置：回收全部已分配块（不释放/触碰后备缓冲区内容）*/
    void (*reset)(pool *self);

    /* 生命周期（对应 init / create） */
    void (*deinit)(pool *self);
    void (*destroy)(pool *self);
};

/* 内存池对象定义（结构体完整可见，便于静态/栈分配） */
struct pool {
    const struct poolFun *fun;
    uint8_t *base;        /* 对齐后的可用缓冲区起点（块从这里开始排布） */
    size_t block_size;    /* 用户请求的块大小（字节） */
    size_t stride;        /* 实际每块步长（含对齐填充，且 >= sizeof(void*)） */
    size_t capacity;      /* 块总数 */
    size_t used;          /* 已分配块数 */
    void *free_list;      /* 空闲块单链表头（空闲块首字节存放 next 指针） */
    bool owns;            /* 后备缓冲区是否由本对象堆分配（deinit/destroy 时释放） */
};

/* ---- 生命周期：构造函数与工厂为自由函数（符合 OOC 惯例）---- */
pool *pool_create(size_t block_size, size_t block_count);                     /* 堆分配后备缓冲区 + 初始化 */
void  pool_init(pool *self, void *buf, size_t buf_size, size_t block_size);   /* 绑定用户提供的静态缓冲区 */
void  pool_deinit(pool *self);                                                /* 对应 init，释放对象侧资源 */
void  pool_destroy(pool *self);                                              /* 对应 create，释放堆内存 */

/* 方法表实例（由 pool.c 提供并赋值给 self->fun） */
extern const struct poolFun pool_fun;

#endif /* JOC_BASE_DS_POOL_H */
