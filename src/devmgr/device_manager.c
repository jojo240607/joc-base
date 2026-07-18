#include "devmgr/device_manager.h"
#include <string.h>
#include <stdint.h>

/*
 * Generic registry implementation. No driver or HAL header is included here:
 * the manager only ever handles `device *`, so it is fully platform-independent.
 */

typedef struct {
    const char *name;
    device *dev;
} dm_entry_t;

static dm_entry_t g_reg[DEVICE_MANAGER_MAX];
static uint32_t   g_count = 0;

void device_manager_register(const char *name, device *dev)
{
    if (!name || !dev) return;
    if (g_count >= DEVICE_MANAGER_MAX) return;

    /* overwrite if the name is already registered */
    for (uint32_t i = 0; i < g_count; i++) {
        if (strcmp(g_reg[i].name, name) == 0) {
            g_reg[i].dev = dev;
            return;
        }
    }
    g_reg[g_count].name = name;
    g_reg[g_count].dev  = dev;
    g_count++;
}

device *device_manager_get(const char *name)
{
    if (!name) return NULL;
    for (uint32_t i = 0; i < g_count; i++) {
        if (strcmp(g_reg[i].name, name) == 0)
            return g_reg[i].dev;
    }
    return NULL;
}

void device_manager_unregister(const char *name)
{
    if (!name) return;
    for (uint32_t i = 0; i < g_count; i++) {
        if (strcmp(g_reg[i].name, name) == 0) {
            for (uint32_t j = i; j < g_count - 1; j++)
                g_reg[j] = g_reg[j + 1];
            g_count--;
            return;
        }
    }
}

void device_manager_reset(void)
{
    g_count = 0;
}
