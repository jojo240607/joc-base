#include "hash.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * 私有：字符串哈希（FNV-1a），返回桶索引
 * ------------------------------------------------------------------------- */
static size_t hash_fn(const char *key, size_t nbuckets) {
    uint32_t h = 2166136261u;
    for (; *key; key++) {
        h ^= (uint32_t)(unsigned char)(*key);
        h *= 16777619u;
    }
    return (size_t)(h % nbuckets);
}

/* 安全前提检查：桶数组/节点池就绪且节点池块足够容下节点 */
static bool hash_usable(const hash *self) {
    return self && self->nbuckets > 0 && self->buckets && self->nodes &&
           self->nodes->block_size >= sizeof(hash_node_t);
}

/* ---------------------------------------------------------------------------
 * vtable 方法实现：全部行为集中在虚表里，hash 成为真正的对象
 * ------------------------------------------------------------------------- */

static bool hash_method_put(hash *self, const char *key, void *value) {
    if (!hash_usable(self) || !key) return false;

    size_t idx = hash_fn(key, self->nbuckets);

    /* 已存在该 key：原地更新 value */
    for (hash_node_t *n = self->buckets[idx]; n; n = n->next) {
        if (strcmp(n->key, key) == 0) {
            n->value = value;
            return true;
        }
    }

    /* key 过长无法放入内联缓冲：拒绝 */
    size_t klen = strlen(key);
    if (klen >= HASH_KEY_MAX_LEN) return false;

    /* 从节点池分配新节点（池满返回 NULL）*/
    hash_node_t *node = (hash_node_t *)self->nodes->fun->alloc(self->nodes);
    if (!node) return false;

    memcpy(node->key, key, klen + 1);   /* 含结尾 '\0'，长度已校验 < HASH_KEY_MAX_LEN */
    node->value = value;
    node->next = self->buckets[idx];   /* 头插法挂到桶链表 */
    self->buckets[idx] = node;
    self->count++;
    return true;
}

static bool hash_method_get(const hash *self, const char *key, void **value) {
    if (!hash_usable(self) || !key) return false;

    size_t idx = hash_fn(key, self->nbuckets);
    for (hash_node_t *n = self->buckets[idx]; n; n = n->next) {
        if (strcmp(n->key, key) == 0) {
            if (value) *value = n->value;
            return true;
        }
    }
    return false;
}

static bool hash_method_remove(hash *self, const char *key) {
    if (!hash_usable(self) || !key) return false;

    size_t idx = hash_fn(key, self->nbuckets);
    hash_node_t *prev = NULL;
    for (hash_node_t *n = self->buckets[idx]; n; prev = n, n = n->next) {
        if (strcmp(n->key, key) == 0) {
            if (prev) prev->next = n->next;
            else      self->buckets[idx] = n->next;
            self->nodes->fun->free(self->nodes, n);   /* 归还节点池 */
            self->count--;
            return true;
        }
    }
    return false;
}

static bool hash_method_contains(const hash *self, const char *key) {
    void *v = NULL;
    return self->fun->get(self, key, &v);
}

static size_t hash_method_size(const hash *self) {
    return self ? self->count : 0;
}

static size_t hash_method_bucket_count(const hash *self) {
    return self ? self->nbuckets : 0;
}

static void hash_method_clear(hash *self) {
    if (!self || !self->buckets || !self->nodes) return;

    for (size_t i = 0; i < self->nbuckets; i++) {
        hash_node_t *n = self->buckets[i];
        while (n) {
            hash_node_t *next = n->next;
            self->nodes->fun->free(self->nodes, n);   /* 归还节点池 */
            n = next;
        }
        self->buckets[i] = NULL;
    }
    self->count = 0;
}

static void hash_method_deinit(hash *self) {
    if (!self) return;
    self->fun->clear(self);   /* 先把所有节点归还节点池 */
    self->buckets  = NULL;
    self->nodes    = NULL;
    self->nbuckets = 0;
    self->count    = 0;
}

static void hash_method_destroy(hash *self) {
    if (!self) return;
    self->fun->deinit(self);   /* 清空并复位 */
    free(self);                /* 释放堆分配的 hash 结构（池与桶由调用方管理）*/
}

/* ---------------------------------------------------------------------------
 * 方法表实例
 * ------------------------------------------------------------------------- */
const struct hashFun hash_fun = {
    .put          = hash_method_put,
    .get          = hash_method_get,
    .remove       = hash_method_remove,
    .contains     = hash_method_contains,
    .size         = hash_method_size,
    .bucket_count = hash_method_bucket_count,
    .clear        = hash_method_clear,
    .deinit       = hash_method_deinit,
    .destroy      = hash_method_destroy,
};

/* ---------------------------------------------------------------------------
 * 构造函数 / 工厂（自由函数，符合 OOC 惯例）
 * ------------------------------------------------------------------------- */

void hash_init(hash *self, hash_node_t **buckets, size_t nbuckets, pool *node_pool) {
    if (!self) return;
    if (!buckets || nbuckets == 0 || !node_pool ||
        node_pool->block_size < sizeof(hash_node_t)) {
        /* 参数不合法：标记为不可用（所有方法将安全失败）*/
        self->fun      = &hash_fun;
        self->buckets  = NULL;
        self->nodes    = NULL;
        self->nbuckets = 0;
        self->count    = 0;
        return;
    }

    self->fun      = &hash_fun;
    self->buckets  = buckets;
    self->nodes    = node_pool;
    self->nbuckets = nbuckets;
    self->count    = 0;

    for (size_t i = 0; i < nbuckets; i++) buckets[i] = NULL;
}

hash *hash_create(hash_node_t **buckets, size_t nbuckets, pool *node_pool) {
    hash *self = (hash *)malloc(sizeof(hash));
    if (!self) return NULL;
    hash_init(self, buckets, nbuckets, node_pool);
    return self;
}

void hash_deinit(hash *self) {
    if (!self) return;
    self->fun->deinit(self);
}

void hash_destroy(hash *self) {
    if (!self) return;
    self->fun->destroy(self);
}
