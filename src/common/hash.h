#ifndef JOC_BASE_DS_HASH_H
#define JOC_BASE_DS_HASH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "pool.h"   /* 节点内存池：链表节点从 pool 分配 */

/* 链表节点内联存储的 key 最大长度（含结尾 '\0'）。
 * 需要存储更长的 key 时，在包含本头文件前 #define HASH_KEY_MAX_LEN 自定义。
 * 节点池的 block_size 必须 >= sizeof(hash_node_t) 才能容纳该节点。 */
#ifndef HASH_KEY_MAX_LEN
#define HASH_KEY_MAX_LEN 32
#endif

/* 链地址法的链表节点：直接从用户提供的 mempool 分配/释放。
 * next 必须作为首成员，以便与 mempool 的空闲链表复用每个块的首字节。 */
typedef struct hash_node {
    struct hash_node *next;          /* 链地址法：同桶内的下一个节点 */
    char key[HASH_KEY_MAX_LEN];      /* 内联存储 key 字符串（以 '\0' 结尾）*/
    void *value;                     /* 任意类型的 value 指针 */
} hash_node_t;

/* 不透明静态哈希表对象（键为字符串，值为 void*，链地址法解决冲突）
 *
 * 设计要点：
 *  - 桶数组（buckets）与节点内存池（mempool）均由调用方提供（静态分配），
 *    哈希表本身不引入任何堆分配；
 *  - 冲突节点挂在桶链表上，节点从 mempool 分配，删除时归还 mempool；
 *  - 静态分配：hash_init 绑定调用方提供的桶数组与节点池；
 *  - 堆分配：  hash_create 仅堆分配 hash 结构本身，桶与池仍由调用方提供。
 */
typedef struct hash hash;

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * 哈希表的全部能力都在虚表里，hash 成为可派生的对象。
 * 所有公开方法只能通过 self->fun->method(self, ...) 分派。
 * ------------------------------------------------------------------------- */
struct hashFun {
    /* 插入/更新：key 已存在则更新 value 并返回 true；否则从节点池分配新节点。
     * key 过长（>= HASH_KEY_MAX_LEN）或节点池已满时返回 false。 */
    bool (*put)(hash *self, const char *key, void *value);
    /* 查询：找到则通过 out 参数返回 value 并返回 true，否则返回 false */
    bool (*get)(const hash *self, const char *key, void **value);
    /* 删除：找到并移除节点（归还 mempool）返回 true，否则 false */
    bool (*remove)(hash *self, const char *key);
    /* 是否存在该 key */
    bool (*contains)(const hash *self, const char *key);

    /* 计数 */
    size_t (*size)(const hash *self);         /* 已存储的键值对数 */
    size_t (*bucket_count)(const hash *self); /* 桶数量 */

    /* 清空：释放所有节点回 mempool，复位桶与计数（不释放 mempool 本身）*/
    void (*clear)(hash *self);

    /* 生命周期（对应 init / create） */
    void (*deinit)(hash *self);
    void (*destroy)(hash *self);
};

/* 哈希表对象定义（结构体完整可见，便于静态/栈分配） */
struct hash {
    const struct hashFun *fun;
    hash_node_t **buckets;  /* 桶数组（每个元素是一条链表的头），由调用方提供 */
    size_t nbuckets;        /* 桶数量 */
    size_t count;           /* 已存储键值对数 */
    pool *nodes;            /* 节点内存池（由调用方提供并管理生命周期）*/
};

/* ---- 生命周期：构造函数与工厂为自由函数（符合 OOC 惯例）---- */
hash *hash_create(hash_node_t **buckets, size_t nbuckets, pool *node_pool);             /* 堆分配 hash 结构 */
void   hash_init(hash *self, hash_node_t **buckets, size_t nbuckets, pool *node_pool);  /* 绑定调用方桶数组与节点池 */
void   hash_deinit(hash *self);                                                        /* 对应 init：清空并复位 */
void   hash_destroy(hash *self);                                                       /* 对应 create：释放堆结构 */

/* 方法表实例（由 hash.c 提供并赋值给 self->fun） */
extern const struct hashFun hash_fun;

#endif /* JOC_BASE_DS_HASH_H */
