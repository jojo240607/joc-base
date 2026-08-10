/*
 * DEVICE MANAGER — generic name -> device* registry (platform-independent).
 *
 * Storage is now built on the project's common data-structure primitives
 * (src/common/{hash,list,pool}.{c,h}) — the same building blocks devtree uses:
 *
 *   - pool  : fixed-capacity block allocator for registry nodes. Hard cap =
 *             DEVICE_MANAGER_MAX. Pool exhaustion is now explicit (alloc returns
 *             NULL) instead of the old silent "drop the 33rd device" failure
 *             (see memory 36069048).
 *   - hash  : O(1) name -> node lookup (replaces the old O(n) linear scan).
 *   - list  : global registration-order list (g_all) + a per-type list per class.
 *
 * The manager does NOT own the device objects: the board layer creates them via
 * xxx_create() and registers the pointer here. It only stores/retrieves `device *`
 * by name, exactly like before. The public API (device_manager.h) is unchanged,
 * so all 28 existing call sites and every driver stay untouched.
 */
#include "devmgr/device_manager.h"
#include "common/hash.h"
#include "common/list.h"
#include "common/pool.h"
#include "common/ccm_bss.h"
#include "iface/device.h"

#include <string.h>
#include <stdint.h>

/* Name key width mirrors devtree (== HASH_KEY_MAX_LEN, 32 by default). */
#ifndef DM_NAME_MAX
#define DM_NAME_MAX HASH_KEY_MAX_LEN
#endif

#define DM_HASH_BUCKETS 64u

/* Registry node: embeds intrusive list links + the device pointer. Allocated
 * from a fixed pool, so the registry has a hard upper bound (DEVICE_MANAGER_MAX). */
typedef struct dm_node {
    char name[DM_NAME_MAX];
    device *dev;
    driver_type_t type;          /* cached from dev->type at register time */
    list_node_t all_node;        /* link in g_all (registration order) */
    list_node_t type_node;       /* link in g_type_lists[type] */
} dm_node_t;

/* Static storage — zero heap (mirrors devtree_init's static path).
 * 纯软件管理表，不含 DMA 目标缓冲，搬入 CCM(发布版)以收缩主 SRAM .bss。 */
static dm_node_t      RTOS_CCM_BSS g_node_store[DEVICE_MANAGER_MAX];
static hash_node_t    RTOS_CCM_BSS g_hnode_store[DEVICE_MANAGER_MAX];
static hash_node_t   *RTOS_CCM_BSS g_buckets[DM_HASH_BUCKETS];

static pool   RTOS_CCM_BSS g_node_pool;       /* allocates dm_node_t */
static pool   RTOS_CCM_BSS g_hnode_pool;      /* allocates hash_node_t (key/value chain) */
static hash   RTOS_CCM_BSS g_by_name;         /* name -> dm_node_t* */
static list   RTOS_CCM_BSS g_all;             /* all registered devices, in order */
static list   RTOS_CCM_BSS g_type_lists[DEVICE_TYPE_COUNT];  /* per-class lists (stats) */

static bool   RTOS_CCM_BSS g_inited = false;

static void dm_ensure_init(void)
{
    if (g_inited) return;
    memset(g_buckets, 0, sizeof(g_buckets));
    pool_init(&g_node_pool,  g_node_store,  sizeof(g_node_store),  sizeof(dm_node_t));
    pool_init(&g_hnode_pool, g_hnode_store, sizeof(g_hnode_store), sizeof(hash_node_t));
    hash_init(&g_by_name, g_buckets, DM_HASH_BUCKETS, &g_hnode_pool);
    list_init(&g_all);
    for (int i = 0; i < (int)DEVICE_TYPE_COUNT; i++) list_init(&g_type_lists[i]);
    g_inited = true;
}

void device_manager_register(const char *name, device *dev)
{
    if (!name || !dev) return;
    dm_ensure_init();

    /* Overwrite semantics (preserved from the old array registry): if the name
     * already exists, just repoint its device pointer; keep its list/hash slot. */
    void *v = NULL;
    if (g_by_name.fun->get(&g_by_name, name, &v)) {
        dm_node_t *node = (dm_node_t *)v;
        if (node->type != dev->type) {           /* keep type list consistent */
            g_type_lists[node->type].fun->remove(&g_type_lists[node->type],
                                                  &node->type_node);
            node->type = dev->type;
            g_type_lists[node->type].fun->insert_before(&g_type_lists[node->type],
                                                        &g_type_lists[node->type].head,
                                                        &node->type_node);
        }
        node->dev = dev;
        return;
    }

    dm_node_t *node = (dm_node_t *)g_node_pool.fun->alloc(&g_node_pool);
    if (!node) {
        /* Pool exhausted: the old code silently dropped the device, causing a
         * 'XXX: MISSING' BIST. Now we refuse instead — bump DEVICE_MANAGER_MAX
         * (device_manager.h) if the board genuinely needs more devices. */
        return;
    }
    memset(node, 0, sizeof(*node));
    strncpy(node->name, name, DM_NAME_MAX - 1);
    node->name[DM_NAME_MAX - 1] = '\0';
    node->dev  = dev;
    node->type = dev->type;

    g_all.fun->insert_before(&g_all, &g_all.head, &node->all_node);
    g_type_lists[dev->type].fun->insert_before(&g_type_lists[dev->type],
                                               &g_type_lists[dev->type].head,
                                               &node->type_node);

    if (!g_by_name.fun->put(&g_by_name, name, node)) {
        /* Hash insert failed (key too long / hnode pool full): roll back. */
        g_type_lists[node->type].fun->remove(&g_type_lists[node->type], &node->type_node);
        g_all.fun->remove(&g_all, &node->all_node);
        g_node_pool.fun->free(&g_node_pool, node);
    }
}

device *device_manager_get(const char *name)
{
    if (!name) return NULL;
    dm_ensure_init();
    void *v = NULL;
    if (g_by_name.fun->get(&g_by_name, name, &v))
        return ((dm_node_t *)v)->dev;
    return NULL;
}

void device_manager_unregister(const char *name)
{
    if (!name) return;
    dm_ensure_init();
    void *v = NULL;
    if (!g_by_name.fun->get(&g_by_name, name, &v)) return;
    dm_node_t *node = (dm_node_t *)v;

    g_type_lists[node->type].fun->remove(&g_type_lists[node->type], &node->type_node);
    g_all.fun->remove(&g_all, &node->all_node);
    g_by_name.fun->remove(&g_by_name, name);   /* returns the hash_node to hnode pool */
    g_node_pool.fun->free(&g_node_pool, node);
}

void device_manager_reset(void)
{
    dm_ensure_init();
    /* Free every node back to the pool; clear the hash (returns hnodes too). */
    list_node_t *n = g_all.fun->first(&g_all);
    while (n) {
        list_node_t *nx = g_all.fun->next(&g_all, n);
        dm_node_t *node = LIST_ENTRY(n, dm_node_t, all_node);
        g_type_lists[node->type].fun->remove(&g_type_lists[node->type], &node->type_node);
        g_node_pool.fun->free(&g_node_pool, node);
        n = nx;
    }
    g_all.fun->clear(&g_all);
    g_by_name.fun->clear(&g_by_name);
    for (int i = 0; i < (int)DEVICE_TYPE_COUNT; i++)
        g_type_lists[i].fun->clear(&g_type_lists[i]);
}
