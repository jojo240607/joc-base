#ifndef JOC_BASE_DS_DEVTREE_H
#define JOC_BASE_DS_DEVTREE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "hash.h"
#include "list.h"
#include "pool.h"

/* ===========================================================================
 * 轻量级设备树管理器（增强版）
 *
 * 能力：设备注册/注销/查找（按名称/类型）；统一设备接口（驱动虚表
 * open/close/read/write/ioctl）；设备间依赖关系（例如传感器依赖 i2c 总线）。
 *
 * 组合：hash（名称索引）+ list（全局/同类型列表）+ pool（对象池，硬上限）。
 * 约定：设备由 dev_pool 统一分配；父设备指针为弱引用；deps 仅记录依赖名称，
 * device_open 前校验依赖已注册且 RUNNING；静态路径零堆，devtree_create 才 malloc。
 * ========================================================================= */

#ifndef DEV_MAX_COUNT
#define DEV_MAX_COUNT 64u
#endif

#ifndef DEV_MAX_DEPS
#define DEV_MAX_DEPS 4u
#endif

#define DEV_NAME_MAX_LEN HASH_KEY_MAX_LEN
#define DEV_TYPE_MAX_LEN HASH_KEY_MAX_LEN

typedef struct device_drv device_drv_t;
typedef enum {
    DEV_STATUS_INIT = 0,
    DEV_STATUS_RUNNING,
    DEV_STATUS_ERROR,
} dev_status_t;

typedef struct device {
    char name[DEV_NAME_MAX_LEN];
    char type[DEV_TYPE_MAX_LEN];
    struct device *parent;
    void *priv;
    dev_status_t status;

    const device_drv_t *drv;            /* 统一设备接口（驱动虚表） */
    const char *deps[DEV_MAX_DEPS];     /* 依赖设备名称（如传感器依赖 i2c） */
    uint8_t ndeps;

    list_node_t type_node;
    list_node_t all_node;
} device_t;

/* 统一设备接口：驱动层实现的虚表（每个驱动提供一份 const 实例）。
 * 返回 0 成功，<0 失败；read/write 通过 got/put 回传实际传输字节数。*/
struct device_drv {
    int (*open)(device_t *self);
    int (*close)(device_t *self);
    int (*read)(device_t *self, void *buf, size_t len, size_t *got);
    int (*write)(device_t *self, const void *buf, size_t len, size_t *put);
    int (*ioctl)(device_t *self, unsigned long cmd, void *arg);
};

typedef void (*dev_visit_cb)(device_t *dev, void *ctx);

typedef struct devtree devtree;

struct devtreeFun {
    bool (*register_dev)(devtree *self, const char *name, const char *type,
                         device_t *parent, void *priv, dev_status_t status,
                         device_t **out);

    bool (*unregister)(devtree *self, const char *name);

    /* 扩展注册：携带驱动虚表 drv 与依赖设备名 deps[]（n 个）*/
    bool (*register_dev_ex)(devtree *self, const char *name, const char *type,
                            device_t *parent, void *priv, dev_status_t status,
                            device_t **out, const device_drv_t *drv,
                            const char *deps[], uint8_t ndeps);

    device_t *(*find)(devtree *self, const char *name);

    size_t (*count)(const devtree *self);
    size_t (*count_type)(const devtree *self, const char *type);

    void (*traverse)(devtree *self, dev_visit_cb cb, void *ctx);
    void (*traverse_type)(devtree *self, const char *type, dev_visit_cb cb, void *ctx);

    /* 统一设备接口分派（先校验依赖已注册且 RUNNING；依赖缺失返回 -2）*/
    int (*device_open)(devtree *self, device_t *dev);
    int (*device_read)(devtree *self, device_t *dev, void *buf, size_t len, size_t *got);
    int (*device_write)(devtree *self, device_t *dev, const void *buf, size_t len, size_t *put);
    int (*device_ioctl)(devtree *self, device_t *dev, unsigned long cmd, void *arg);
    int (*device_close)(devtree *self, device_t *dev);

    void (*clear)(devtree *self);
    void (*deinit)(devtree *self);
    void (*destroy)(devtree *self);
};

struct devtree {
    const struct devtreeFun *fun;
    hash   by_name;
    pool   name_hpool;
    pool   dev_pool;
    list   all;
    list   type_registry;

    size_t count;
    bool   owns;

    device_t      *dev_store;
    hash_node_t  **name_buckets;
    void          *name_hbuf;
};

void devtree_init(devtree *self,
                  device_t *dev_store, size_t dev_capacity,
                  hash_node_t **name_buckets, size_t name_nbuckets,
                  void *name_hbuf, size_t name_hbuf_size);
devtree *devtree_create(size_t dev_capacity, size_t name_nbuckets);
void devtree_deinit(devtree *self);
void devtree_destroy(devtree *self);

const char *dev_status_name(dev_status_t status);
extern const struct devtreeFun devtree_fun;

#endif /* JOC_BASE_DS_DEVTREE_H */
