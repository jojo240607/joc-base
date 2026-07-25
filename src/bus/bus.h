#ifndef JOC_BASE_DS_BUS_H
#define JOC_BASE_DS_BUS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * 轻量级事件总线（发布/订阅，Publish/Subscribe），完全基于静态内存
 *
 * 设计目标：嵌入式/MCU 场景下零堆依赖的事件分发。
 *   - 订阅者（handler）以“槽位”形式存放于一块调用方提供的静态缓冲区中；
 *   - topic 为整数事件号（uint16_t），每个 topic 拥有固定容量的订阅槽位；
 *   - 订阅槽位按 topic 分块连续排布：topic t 的订阅者落在数组
 *       [t*subs_per_topic, (t+1)*subs_per_topic) 区间，发布时只扫描该区间，
 *       因此发布复杂度为 O(subs_per_topic)，与全局订阅总数无关；
 *   - 订阅句柄就是槽位指针（bus_sub_t*），取消订阅 O(1)。
 *
 * 内存布局（bus_init 绑定的 buf 内）：
 *   bus_sub_t slots[max_topics * subs_per_topic];   // 订阅槽位数组
 *   uint16_t  used[max_topics];                      // 每个 topic 的活跃计数
 *
 * 用法（静态，推荐）：
 *   static uint8_t g_buf[BUS_BUF_SIZE(8, 4)];
 *   bus g_bus;
 *   bus_init(&g_bus, g_buf, sizeof(g_buf), 8, 4);
 *   bus_sub_t *h = g_bus.fun->subscribe(&g_bus, 1, my_handler, my_ctx);
 *   g_bus.fun->publish(&g_bus, 1, &evt, sizeof(evt));
 *   g_bus.fun->unsubscribe(&g_bus, h);
 * ------------------------------------------------------------------------- */

typedef struct bus bus;

/* 事件处理器原型：topic 为事件号，data/len 为载荷（可能为空），ctx 为订阅时上下文 */
typedef void (*bus_handler_fn)(uint16_t topic, const void *data, size_t len, void *ctx);

/* 订阅者槽位（静态数组中的元素；订阅句柄即指向它的指针） */
typedef struct bus_sub {
    bus_handler_fn handler;   /* 事件回调（NULL 表示空闲/已取消） */
    void         *ctx;        /* 用户上下文，随回调透传 */
    uint16_t      topic;      /* 所属 topic（用于取消订阅时回写计数） */
    bool          active;     /* 是否处于活跃（已订阅）状态 */
} bus_sub_t;

/* 计算给定容量所需的后备缓冲区字节数（用于静态分配 buf） */
#define BUS_BUF_SIZE(MAX_TOPICS, SUBS_PER_TOPIC)                          \
    ((size_t)(MAX_TOPICS) * (SUBS_PER_TOPIC) * sizeof(bus_sub_t)          \
     + (size_t)(MAX_TOPICS) * sizeof(uint16_t))

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * ------------------------------------------------------------------------- */
struct busFun {
    /* 订阅：在 topic 上注册一个 handler；成功返回订阅句柄（非 NULL），失败返回 NULL。
     * topic 越界或该 topic 订阅位已满时返回 NULL。句柄用于 unsubscribe。*/
    bus_sub_t *(*subscribe)(bus *self, uint16_t topic,
                            bus_handler_fn handler, void *ctx);

    /* 取消订阅：释放指定句柄对应槽位。句柄须来自本总线且仍有效，成功返回 true。
     * 可在事件回调中安全调用（含取消自身）。*/
    bool (*unsubscribe)(bus *self, bus_sub_t *handle);

    /* 取消某 topic 上的全部订阅；返回被移除的订阅数。*/
    uint16_t (*unsubscribe_topic)(bus *self, uint16_t topic);

    /* 发布：向 topic 的所有“广播开始时已活跃”的订阅者广播 data/len；
     * 返回实际投递的订阅者数量。topic 越界返回 0。
     * 注意：广播过程中新注册的订阅者是否收到本次事件由实现决定（通常为否）。*/
    uint16_t (*publish)(bus *self, uint16_t topic, const void *data, size_t len);

    /* 查询 */
    bool     (*has_subscriber)(const bus *self, uint16_t topic);  /* 该 topic 是否有活跃订阅 */
    uint16_t (*subscriber_count)(const bus *self, uint16_t topic);/* 该 topic 活跃订阅数 */
    uint16_t (*topic_count)(const bus *self);                     /* 最大 topic 数 */
    uint16_t (*capacity)(const bus *self);                        /* 总订阅槽位数 */

    /* 移除全部订阅（清空总线，不释放缓冲区） */
    void (*clear)(bus *self);

    /* 生命周期（对应 init / create） */
    void (*deinit)(bus *self);
    void (*destroy)(bus *self);
};

/* 事件总线对象定义（结构体完整可见，便于静态/栈分配） */
struct bus {
    const struct busFun *fun;
    bus_sub_t *slots;            /* 订阅槽位数组（按 topic 分块连续存放） */
    uint16_t  *used;             /* 每个 topic 的活跃订阅计数 */
    uint16_t   max_topics;       /* 最大 topic 数 */
    uint16_t   subs_per_topic;   /* 每个 topic 的订阅槽位容量 */
    uint32_t   total;            /* 总订阅槽位数 = max_topics * subs_per_topic */
    void      *buf;              /* 后备缓冲区起点（owns 时 destroy 释放） */
    bool       owns;             /* 后备缓冲是否由本对象堆分配 */
};

/* ---- 生命周期：构造函数与工厂为自由函数（符合 OOC 惯例）---- */
/* 堆分配（可选，供主机/测试使用）；仍满足“运行时静态分发”，但会动用 malloc。
 * 纯静态场景请用 bus_init 绑定自有缓冲区。 */
bus *bus_create(uint16_t max_topics, uint16_t subs_per_topic);
/* 绑定用户提供的静态缓冲区并切分槽位；缓冲不足时总线保持为空（各方法安全返回）。*/
void  bus_init(bus *self, void *buf, size_t buf_size,
               uint16_t max_topics, uint16_t subs_per_topic);
void  bus_deinit(bus *self);   /* 对应 init：清空调度状态（不释放静态缓冲区） */
void  bus_destroy(bus *self);  /* 对应 create：owns 时释放堆内存 */

/* 方法表实例（由 bus.c 提供并赋值给 self->fun） */
extern const struct busFun bus_fun;

#endif /* JOC_BASE_DS_BUS_H */
