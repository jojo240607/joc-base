#include "list.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * 私有辅助：哨兵节点自环初始化
 * ------------------------------------------------------------------------- */
static inline void list_head_init(list_node_t *head) {
    head->prev = head;
    head->next = head;
}

/* ---------------------------------------------------------------------------
 * vtable 方法实现：所有行为集中在虚表里，list 成为真正的对象
 * ------------------------------------------------------------------------- */

static bool list_method_empty(const list *self) {
    return self->head.next == &self->head;
}

static void list_method_insert_after(list *self, list_node_t *anchor, list_node_t *node) {
    (void)self;  /* 插入逻辑与具体链表实例无关，self 仅用于分派一致性 */
    node->next = anchor->next;
    node->prev = anchor;
    anchor->next->prev = node;
    anchor->next = node;
}

static void list_method_insert_before(list *self, list_node_t *anchor, list_node_t *node) {
    (void)self;
    node->prev = anchor->prev;
    node->next = anchor;
    anchor->prev->next = node;
    anchor->prev = node;
}

static void list_method_remove(list *self, list_node_t *node) {
    (void)self;
    node->prev->next = node->next;
    node->next->prev = node->prev;
}

static void list_method_clear(list *self) {
    list_head_init(&self->head);
}

static list_node_t *list_method_first(const list *self) {
    return (self->head.next == &self->head) ? NULL : self->head.next;
}

static list_node_t *list_method_last(const list *self) {
    return (self->head.prev == &self->head) ? NULL : self->head.prev;
}

static list_node_t *list_method_next(const list *self, const list_node_t *node) {
    return (node->next == &self->head) ? NULL : node->next;
}

static list_node_t *list_method_prev(const list *self, const list_node_t *node) {
    return (node->prev == &self->head) ? NULL : node->prev;
}

static void list_method_deinit(list *self) {
    /* 侵入式链表：节点内存由宿主结构体管理，对象侧无可释放资源 */
    (void)self;
}

static void list_method_destroy(list *self) {
    if (!self) return;
    self->fun->deinit(self);
    free(self);
}

/* ---------------------------------------------------------------------------
 * 方法表实例
 * ------------------------------------------------------------------------- */
const struct listFun list_fun = {
    .empty        = list_method_empty,
    .insert_after = list_method_insert_after,
    .insert_before= list_method_insert_before,
    .remove       = list_method_remove,
    .clear        = list_method_clear,
    .first        = list_method_first,
    .last         = list_method_last,
    .next         = list_method_next,
    .prev         = list_method_prev,
    .deinit       = list_method_deinit,
    .destroy      = list_method_destroy,
};

/* ---------------------------------------------------------------------------
 * 构造函数 / 工厂（自由函数，符合 OOC 惯例）
 * ------------------------------------------------------------------------- */
list *list_create(void) {
    list *self = (list *)malloc(sizeof(list));
    if (!self) return NULL;
    memset(self, 0, sizeof(list));
    list_init(self);
    return self;
}

void list_init(list *self) {
    if (!self) return;
    self->fun = &list_fun;          /* 绑定虚表 */
    list_head_init(&self->head);    /* 自环初始化哨兵节点 */
}

void list_deinit(list *self) {
    if (!self) return;
    self->fun->deinit(self);
}

void list_destroy(list *self) {
    if (!self) return;
    self->fun->destroy(self);
}
