/* 轻量级事件总线（发布/订阅），完全基于静态内存 */
#include "bus.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * 内部辅助与前向声明
 * 注意：函数表 bus_fun 的初始化式引用了下面的静态函数，因此前向声明必须位于
 * bus_fun 之前，否则 MSVC 会因“先隐式 extern、后定义为 static”报 C4211。
 * ------------------------------------------------------------------------- */
static size_t bus_need(uint16_t max_topics, uint16_t subs_per_topic) {
    if (max_topics == 0 || subs_per_topic == 0) return 0;
    return (size_t)max_topics * subs_per_topic * sizeof(bus_sub_t)
         + (size_t)max_topics * sizeof(uint16_t);
}

static bus_sub_t *bus_subscribe(bus *self, uint16_t topic,
                                bus_handler_fn handler, void *ctx);
static bool       bus_unsubscribe(bus *self, bus_sub_t *handle);
static uint16_t   bus_unsubscribe_topic(bus *self, uint16_t topic);
static uint16_t   bus_publish(bus *self, uint16_t topic,
                              const void *data, size_t len);
static bool       bus_has_subscriber(const bus *self, uint16_t topic);
static uint16_t   bus_subscriber_count(const bus *self, uint16_t topic);
static uint16_t   bus_topic_count(const bus *self);
static uint16_t   bus_capacity(const bus *self);
static void       bus_clear(bus *self);
/* 注：bus_init / bus_deinit / bus_destroy / bus_create 为公开接口，已在 bus.h 声明，
 * 此处不再加 static 前向声明，避免与头文件 extern 声明冲突（C4211）。*/

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * ------------------------------------------------------------------------- */
const struct busFun bus_fun = {
    .subscribe         = bus_subscribe,
    .unsubscribe       = bus_unsubscribe,
    .unsubscribe_topic = bus_unsubscribe_topic,
    .publish           = bus_publish,
    .has_subscriber    = bus_has_subscriber,
    .subscriber_count  = bus_subscriber_count,
    .topic_count       = bus_topic_count,
    .capacity          = bus_capacity,
    .clear             = bus_clear,
    .deinit            = bus_deinit,
    .destroy           = bus_destroy,
};

bus *bus_create(uint16_t max_topics, uint16_t subs_per_topic) {
    size_t need = bus_need(max_topics, subs_per_topic);
    bus  *self = (bus *)malloc(sizeof(bus));
    void *buf  = need ? malloc(need) : NULL;
    if (!self || (need && !buf)) {
        free(self);
        free(buf);
        return NULL;
    }
    memset(self, 0, sizeof(bus));
    bus_init(self, buf, need, max_topics, subs_per_topic);
    self->owns = true;
    self->buf  = buf;
    return self;
}

void bus_init(bus *self, void *buf, size_t buf_size,
              uint16_t max_topics, uint16_t subs_per_topic) {
    if (!self) return;

    /* 先置为“空总线”并绑定方法表，使任何后续调用都安全失败 */
    self->fun             = &bus_fun;
    self->owns            = false;
    self->buf             = buf;
    self->max_topics      = 0;
    self->subs_per_topic  = 0;
    self->total           = 0;
    self->slots           = NULL;
    self->used            = NULL;

    size_t need = bus_need(max_topics, subs_per_topic);
    if (need == 0 || !buf || buf_size < need) return;   /* 静态绑定失败 */

    self->max_topics     = max_topics;
    self->subs_per_topic = subs_per_topic;
    self->total          = (uint32_t)max_topics * subs_per_topic;
    self->slots = (bus_sub_t *)buf;
    self->used  = (uint16_t *)((uint8_t *)buf
                    + (size_t)max_topics * subs_per_topic * sizeof(bus_sub_t));

    memset(self->slots, 0, (size_t)self->total * sizeof(bus_sub_t));
    memset(self->used,  0, (size_t)max_topics * sizeof(uint16_t));
}

static bus_sub_t *bus_subscribe(bus *self, uint16_t topic,
                                bus_handler_fn handler, void *ctx) {
    if (!self || !self->slots || topic >= self->max_topics || !handler)
        return NULL;

    uint32_t base = (uint32_t)topic * self->subs_per_topic;
    for (uint16_t i = 0; i < self->subs_per_topic; i++) {
        bus_sub_t *s = &self->slots[base + i];
        if (!s->active) {
            s->handler = handler;
            s->ctx     = ctx;
            s->topic   = topic;
            s->active  = true;
            self->used[topic]++;
            return s;
        }
    }
    return NULL;   /* 该 topic 订阅位已满 */
}

static bool bus_unsubscribe(bus *self, bus_sub_t *handle) {
    if (!self || !self->slots || !handle) return false;
    /* 句柄必须落在 slots 范围内且属于本总线 */
    if (handle < self->slots || handle >= self->slots + self->total)
        return false;
    if (!handle->active) return false;

    uint16_t t      = handle->topic;
    handle->active  = false;
    handle->handler = NULL;
    handle->ctx     = NULL;
    if (self->used[t] > 0) self->used[t]--;
    return true;
}

static uint16_t bus_unsubscribe_topic(bus *self, uint16_t topic) {
    if (!self || !self->slots || topic >= self->max_topics) return 0;

    uint32_t base = (uint32_t)topic * self->subs_per_topic;
    uint16_t removed = 0;
    for (uint16_t i = 0; i < self->subs_per_topic; i++) {
        bus_sub_t *s = &self->slots[base + i];
        if (s->active) {
            s->active  = false;
            s->handler = NULL;
            s->ctx     = NULL;
            removed++;
        }
    }
    self->used[topic] = 0;
    return removed;
}

static uint16_t bus_publish(bus *self, uint16_t topic,
                            const void *data, size_t len) {
    if (!self || !self->slots || topic >= self->max_topics) return 0;

    uint32_t base = (uint32_t)topic * self->subs_per_topic;
    uint16_t delivered = 0;
    for (uint16_t i = 0; i < self->subs_per_topic; i++) {
        bus_sub_t *s = &self->slots[base + i];
        /* 仅投递“广播开始时已活跃”者；回调中取消订阅会把 active 置否，下轮即跳过。
         * 回调中新注册的订阅者此时 active 为 false，不会收到本次事件。*/
        if (!s->active || !s->handler) continue;

        bus_handler_fn h   = s->handler;
        void          *ctx = s->ctx;
        h(topic, data, len, ctx);
        delivered++;
    }
    return delivered;
}

static bool bus_has_subscriber(const bus *self, uint16_t topic) {
    if (!self || !self->used || topic >= self->max_topics) return false;
    return self->used[topic] > 0;
}

static uint16_t bus_subscriber_count(const bus *self, uint16_t topic) {
    if (!self || !self->used || topic >= self->max_topics) return 0;
    return self->used[topic];
}

static uint16_t bus_topic_count(const bus *self) {
    return self ? self->max_topics : 0;
}

static uint16_t bus_capacity(const bus *self) {
    return (uint16_t)(self ? self->total : 0);
}

static void bus_clear(bus *self) {
    if (!self || !self->slots) return;
    memset(self->slots, 0, (size_t)self->total * sizeof(bus_sub_t));
    if (self->used)
        memset(self->used, 0, (size_t)self->max_topics * sizeof(uint16_t));
}

void bus_deinit(bus *self) {
    if (!self) return;
    if (self->slots) {
        memset(self->slots, 0, (size_t)self->total * sizeof(bus_sub_t));
        self->slots = NULL;
    }
    self->used            = NULL;
    self->max_topics      = 0;
    self->subs_per_topic  = 0;
    self->total           = 0;
    /* 静态路径下 buf 由调用方拥有，此处不释放 */
}

void bus_destroy(bus *self) {
    if (!self) return;
    bus_deinit(self);
    if (self->owns && self->buf) free(self->buf);
    free(self);
}
