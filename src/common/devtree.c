#include "devtree.h"

#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * 内部类型：类型桶（每类型一条 list 维护同类型设备，挂到 type_registry）
 * ========================================================================= */
typedef struct dev_type_bucket {
    list_node_t reg_link;
    char type[DEV_TYPE_MAX_LEN];
    list devices;                 /* 节点 = device_t.type_node */
} dev_type_bucket_t;

/* ---- 字符串安全拷贝 ---- */
static void dev_strcpy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    for (; i + 1 < cap && src && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* 类型注册表中查找某类型的桶（线性查找：类型数通常很少）。*/
static dev_type_bucket_t *find_type_bucket(devtree *self, const char *type) {
    for (list_node_t *n = self->type_registry.fun->first(&self->type_registry);
         n; n = self->type_registry.fun->next(&self->type_registry, n)) {
        dev_type_bucket_t *b = LIST_ENTRY(n, dev_type_bucket_t, reg_link);
        if (strcmp(b->type, type) == 0) return b;
    }
    return NULL;
}

/* 新建类型桶并挂入注册表（堆分配，由 clear/destroy 释放）。*/
static dev_type_bucket_t *create_type_bucket(devtree *self, const char *type) {
    dev_type_bucket_t *b = (dev_type_bucket_t *)malloc(sizeof(dev_type_bucket_t));
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    list_init(&b->devices);
    dev_strcpy(b->type, DEV_TYPE_MAX_LEN, type);
    self->type_registry.fun->insert_before(&self->type_registry,
                                           &self->type_registry.head, &b->reg_link);
    return b;
}

/* ===========================================================================
 * vtable 方法实现
 * ========================================================================= */

/* 前向声明：register 复用 register_ex 实现 */
static bool devtree_method_register_ex(devtree *self, const char *name, const char *type,
                                        device_t *parent, void *priv, dev_status_t status,
                                        device_t **out, const device_drv_t *drv,
                                        const char *deps[], uint8_t ndeps);

/* 内部：把 dev 填字段并挂入全局列表/类型列表/名称哈希。返回 false 表示失败已回滚。*/
static bool dev_attach(devtree *self, device_t *dev, const char *name,
                       const char *type, device_t *parent, void *priv,
                       dev_status_t status, const device_drv_t *drv,
                       const char *deps[], uint8_t ndeps) {
    if (ndeps > DEV_MAX_DEPS) ndeps = DEV_MAX_DEPS;
    dev_strcpy(dev->name, DEV_NAME_MAX_LEN, name);
    dev_strcpy(dev->type, DEV_TYPE_MAX_LEN, type);
    dev->parent = parent;
    dev->priv   = priv;
    dev->status = status;
    dev->drv    = drv;
    dev->ndeps  = ndeps;
    for (uint8_t i = 0; i < ndeps; i++) dev->deps[i] = deps[i];

    /* 挂入全局列表（尾插，保证遍历顺序 = 注册顺序）*/
    self->all.fun->insert_before(&self->all, &self->all.head, &dev->all_node);

    dev_type_bucket_t *b = find_type_bucket(self, type);
    if (!b) {
        b = create_type_bucket(self, type);
        if (!b) {
            self->all.fun->remove(&self->all, &dev->all_node);
            return false;
        }
    }
    b->devices.fun->insert_before(&b->devices, &b->devices.head, &dev->type_node);

    if (!self->by_name.fun->put(&self->by_name, name, dev)) {
        b->devices.fun->remove(&b->devices, &dev->type_node);
        self->all.fun->remove(&self->all, &dev->all_node);
        return false;
    }
    return true;
}

static bool devtree_method_register(devtree *self, const char *name, const char *type,
                                     device_t *parent, void *priv, dev_status_t status,
                                     device_t **out) {
    return devtree_method_register_ex(self, name, type, parent, priv, status,
                                      out, NULL, NULL, 0);
}

static bool devtree_method_register_ex(devtree *self, const char *name, const char *type,
                                        device_t *parent, void *priv, dev_status_t status,
                                        device_t **out, const device_drv_t *drv,
                                        const char *deps[], uint8_t ndeps) {
    if (out) *out = NULL;
    if (!self || !name || !type) return false;
    if (name[0] == '\0' || type[0] == '\0') return false;

    if (self->fun->find(self, name) != NULL) return false;   /* 重名保护 */

    device_t *dev = (device_t *)self->dev_pool.fun->alloc(&self->dev_pool);
    if (!dev) return false;
    memset(dev, 0, sizeof(*dev));

    if (!dev_attach(self, dev, name, type, parent, priv, status, drv, deps, ndeps)) {
        self->dev_pool.fun->free(&self->dev_pool, dev);
        return false;
    }

    self->count++;
    if (out) *out = dev;
    return true;
}

static device_t *devtree_method_find(devtree *self, const char *name) {
    if (!self || !name) return NULL;
    void *v = NULL;
    if (self->by_name.fun->get(&self->by_name, name, &v)) return (device_t *)v;
    return NULL;
}

static bool devtree_method_unregister(devtree *self, const char *name) {
    if (!self || !name) return false;

    device_t *dev = self->fun->find(self, name);
    if (!dev) return false;

    dev_type_bucket_t *b = find_type_bucket(self, dev->type);
    if (b) {
        b->devices.fun->remove(&b->devices, &dev->type_node);
        if (b->devices.fun->empty(&b->devices)) {
            self->type_registry.fun->remove(&self->type_registry, &b->reg_link);
            free(b);
        }
    }

    self->all.fun->remove(&self->all, &dev->all_node);
    self->by_name.fun->remove(&self->by_name, name);
    self->dev_pool.fun->free(&self->dev_pool, dev);
    self->count--;
    return true;
}

static size_t devtree_method_count(const devtree *self) {
    return self ? self->count : 0;
}

static size_t devtree_method_count_type(const devtree *self, const char *type) {
    if (!self || !type) return 0;
    dev_type_bucket_t *b = find_type_bucket((devtree *)self, type);
    if (!b) return 0;
    size_t n = 0;
    for (list_node_t *p = b->devices.fun->first(&b->devices); p;
         p = b->devices.fun->next(&b->devices, p)) n++;
    return n;
}

static void devtree_method_traverse(devtree *self, dev_visit_cb cb, void *ctx) {
    if (!self || !cb) return;
    for (list_node_t *n = self->all.fun->first(&self->all); n;
         n = self->all.fun->next(&self->all, n)) {
        cb(LIST_ENTRY(n, device_t, all_node), ctx);
    }
}

static void devtree_method_traverse_type(devtree *self, const char *type,
                                         dev_visit_cb cb, void *ctx) {
    if (!self || !type || !cb) return;
    dev_type_bucket_t *b = find_type_bucket(self, type);
    if (!b) return;
    for (list_node_t *n = b->devices.fun->first(&b->devices); n;
         n = b->devices.fun->next(&b->devices, n)) {
        cb(LIST_ENTRY(n, device_t, type_node), ctx);
    }
}

/* ---- 统一设备接口分派 ---- */

/* 校验依赖：所有 deps 必须已注册且 RUNNING；否则返回 -2。*/
static int check_deps(devtree *self, device_t *dev) {
    for (uint8_t i = 0; i < dev->ndeps; i++) {
        device_t *d = self->fun->find(self, dev->deps[i]);
        if (!d || d->status != DEV_STATUS_RUNNING) return -2;
    }
    return 0;
}

static int devtree_method_device_open(devtree *self, device_t *dev) {
    if (!self || !dev) return -1;
    if (!dev->drv || !dev->drv->open) return -1;
    if (check_deps(self, dev) != 0) return -2;   /* 依赖未就绪 */
    int rc = dev->drv->open(dev);
    if (rc == 0) dev->status = DEV_STATUS_RUNNING;
    return rc;
}

static int devtree_method_device_read(devtree *self, device_t *dev,
                                      void *buf, size_t len, size_t *got) {
    if (!self || !dev || !dev->drv || !dev->drv->read) return -1;
    return dev->drv->read(dev, buf, len, got);
}

static int devtree_method_device_write(devtree *self, device_t *dev,
                                       const void *buf, size_t len, size_t *put) {
    if (!self || !dev || !dev->drv || !dev->drv->write) return -1;
    return dev->drv->write(dev, buf, len, put);
}

static int devtree_method_device_ioctl(devtree *self, device_t *dev,
                                       unsigned long cmd, void *arg) {
    if (!self || !dev || !dev->drv || !dev->drv->ioctl) return -1;
    return dev->drv->ioctl(dev, cmd, arg);
}

static int devtree_method_device_close(devtree *self, device_t *dev) {
    if (!self || !dev) return -1;
    if (!dev->drv || !dev->drv->close) return -1;
    int rc = dev->drv->close(dev);
    if (rc == 0) dev->status = DEV_STATUS_INIT;
    return rc;
}

static void devtree_method_clear(devtree *self) {
    if (!self) return;
    list_node_t *n;
    while ((n = self->type_registry.fun->first(&self->type_registry)) != NULL) {
        dev_type_bucket_t *b = LIST_ENTRY(n, dev_type_bucket_t, reg_link);
        self->type_registry.fun->remove(&self->type_registry, n);
        free(b);
    }
    self->dev_pool.fun->reset(&self->dev_pool);
    self->all.fun->clear(&self->all);
    self->by_name.fun->clear(&self->by_name);
    self->count = 0;
}

static void devtree_method_deinit(devtree *self) {
    if (!self) return;
    self->fun->clear(self);
    self->by_name.fun->deinit(&self->by_name);
    self->name_hpool.fun->deinit(&self->name_hpool);
    self->dev_pool.fun->deinit(&self->dev_pool);
    list_deinit(&self->all);
    list_deinit(&self->type_registry);
    if (!self->owns) { self->dev_store = NULL; self->name_buckets = NULL; self->name_hbuf = NULL; }
}

static void devtree_method_destroy(devtree *self) {
    if (!self) return;
    self->fun->deinit(self);
    if (self->owns) {
        free(self->dev_store); free(self->name_buckets); free(self->name_hbuf);
        self->dev_store = NULL; self->name_buckets = NULL; self->name_hbuf = NULL;
    }
    free(self);
}

/* ===========================================================================
 * 方法表实例
 * ========================================================================= */
const struct devtreeFun devtree_fun = {
    .register_dev     = devtree_method_register,
    .register_dev_ex  = devtree_method_register_ex,
    .unregister       = devtree_method_unregister,
    .find             = devtree_method_find,
    .count            = devtree_method_count,
    .count_type       = devtree_method_count_type,
    .traverse         = devtree_method_traverse,
    .traverse_type    = devtree_method_traverse_type,
    .device_open      = devtree_method_device_open,
    .device_read      = devtree_method_device_read,
    .device_write     = devtree_method_device_write,
    .device_ioctl     = devtree_method_device_ioctl,
    .device_close     = devtree_method_device_close,
    .clear            = devtree_method_clear,
    .deinit           = devtree_method_deinit,
    .destroy          = devtree_method_destroy,
};

/* ===========================================================================
 * 构造函数 / 工厂
 * ========================================================================= */
void devtree_init(devtree *self,
                  device_t *dev_store, size_t dev_capacity,
                  hash_node_t **name_buckets, size_t name_nbuckets,
                  void *name_hbuf, size_t name_hbuf_size) {
    if (!self) return;
    memset(self, 0, sizeof(*self));
    self->fun = &devtree_fun;
    pool_init(&self->dev_pool, dev_store, dev_capacity * sizeof(device_t), sizeof(device_t));
    pool_init(&self->name_hpool, name_hbuf, name_hbuf_size, sizeof(hash_node_t));
    hash_init(&self->by_name, name_buckets, name_nbuckets, &self->name_hpool);
    list_init(&self->all);
    list_init(&self->type_registry);
    self->count = 0;
    self->owns  = false;
    self->dev_store = dev_store;
    self->name_buckets = name_buckets;
    self->name_hbuf = name_hbuf;
}

devtree *devtree_create(size_t dev_capacity, size_t name_nbuckets) {
    if (dev_capacity == 0 || name_nbuckets == 0) return NULL;
    devtree *self = (devtree *)malloc(sizeof(devtree));
    if (!self) return NULL;
    device_t      *dev_store = (device_t *)malloc(dev_capacity * sizeof(device_t));
    hash_node_t  **name_buckets = (hash_node_t **)malloc(name_nbuckets * sizeof(hash_node_t *));
    void          *name_hbuf = (void *)malloc(dev_capacity * sizeof(hash_node_t));
    if (!dev_store || !name_buckets || !name_hbuf) {
        free(dev_store); free(name_buckets); free(name_hbuf); free(self);
        return NULL;
    }
    devtree_init(self, dev_store, dev_capacity, name_buckets, name_nbuckets,
                 name_hbuf, dev_capacity * sizeof(hash_node_t));
    self->owns = true;
    return self;
}

void devtree_deinit(devtree *self) { if (self) self->fun->deinit(self); }
void devtree_destroy(devtree *self) { if (self) self->fun->destroy(self); }

const char *dev_status_name(dev_status_t status) {
    switch (status) {
        case DEV_STATUS_INIT:    return "init";
        case DEV_STATUS_RUNNING: return "running";
        case DEV_STATUS_ERROR:   return "error";
        default:                 return "unknown";
    }
}
