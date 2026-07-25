#ifndef JOC_BASE_DS_LIST_H
#define JOC_BASE_DS_LIST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* offsetof, NULL */

/* 侵入式链表节点：嵌入到用户结构体中 */
typedef struct list_node {
    struct list_node *prev;
    struct list_node *next;
} list_node_t;

/* 不透明链表对象（作为链表头/管理器） */
typedef struct list list;

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * 所有链表行为都通过该表分派：self->fun->method(self, ...)
 * 这样 list 成为真正的对象，行为可被派生类型覆写（多态），
 * 而不是散落为一堆直接操作指针的宏。
 * ------------------------------------------------------------------------- */
struct listFun {
    /* 判空 */
    bool (*empty)(const list *self);

    /* 在 anchor 之后/之前插入 node（anchor 传 &self->head 即尾插/头插） */
    void (*insert_after)(list *self, list_node_t *anchor, list_node_t *node);
    void (*insert_before)(list *self, list_node_t *anchor, list_node_t *node);

    /* 从链表中摘除 node（不释放宿主内存） */
    void (*remove)(list *self, list_node_t *node);

    /* 摘除所有节点，回到空链表 */
    void (*clear)(list *self);

    /* 迭代访问器（到哨兵即返回 NULL，便于普通 for/while 遍历） */
    list_node_t *(*first)(const list *self);
    list_node_t *(*last)(const list *self);
    list_node_t *(*next)(const list *self, const list_node_t *node);
    list_node_t *(*prev)(const list *self, const list_node_t *node);

    /* 生命周期（对应 init / create） */
    void (*deinit)(list *self);
    void (*destroy)(list *self);
};

/* 链表对象定义（结构体完整可见，便于用户取 &self->head 作锚点） */
struct list {
    const struct listFun *fun;
    list_node_t head;   /* 哨兵节点，代表整个链表 */
};

/* ---- 生命周期：构造函数与工厂为自由函数（符合 OOC 惯例）---- */
list *list_create(void);                 /* 堆分配 + 初始化 */
void  list_init(list *self);             /* 栈/静态分配初始化 */
void  list_deinit(list *self);           /* 对应 init，释放对象侧资源 */
void  list_destroy(list *self);          /* 对应 create，释放堆内存 */

/* ---- 便捷插入（内联语法糖，底层仍走虚表分派）---- */
static inline void list_push_front(list *self, list_node_t *node) {
    self->fun->insert_before(self, &self->head, node);
}
static inline void list_push_back(list *self, list_node_t *node) {
    self->fun->insert_after(self, &self->head, node);
}

/* 方法表实例（由 list.c 提供并赋值给 self->fun） */
extern const struct listFun list_fun;

/* ---------------------------------------------------------------------------
 * LIST_ENTRY：唯一的宏
 * 作用：把 list_node_t* 反算成宿主结构体指针（类型安全转换）。
 * 它依赖编译期 offsetof 与宿主类型，返回的是带具体类型的指针，
 * 无法表达为虚表方法（方法只能返回 list_node_t* 或 void*）。
 * 这也是 Linux / Zephyr / RT-Thread 等所有 OOC 侵入式容器的通用做法。
 * ------------------------------------------------------------------------- */
#define LIST_ENTRY(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#endif /* JOC_BASE_DS_LIST_H */
