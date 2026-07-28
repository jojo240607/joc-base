#include "rtos.h"
#include <string.h>
#include "log/log.h"
#include "log/app_log.h"

/* ---------------------------------------------------------------------------
 * 内核对象注册表（core/kobj.c）：调试/按名查找 + SVC 门指针校验。
 *
 * 所有 IPC 对象在 init 时自动登记（name 可 NULL，仅用于按指针校验），
 * 任务在创建时登记（带名字，可按名查找）；SVC 门用 rtos_kobj_validate
 * 校验“用户态传入的对象指针”确为已登记的内核对象，防伪造指针越权。
 *
 * 原位于 rtos_ipc.c（聚合文件），按 docs/rtos-design.md §10 拆分到 core/。
 * ------------------------------------------------------------------------- */
/* KOBJ_MAX 必须覆盖“整个 RTOSALL 串联运行中所有存活内核对象”的峰值：
 * 常态下各模块累计的 IPC 对象（sem/mq/mutex/event/bus，多为常驻静态对象）
 * 约数十个；加上存活任务。任务在退出(TASK_DEAD)时经 rtos_kobj_deregister
 * 回收槽位（见 core/task.c），故峰值由“同时存活任务数 + 累计 IPC 对象数”
 * 决定，而非“历史上创建过的全部任务”。128 给稳健模块(如 RTOSROBUST 的
 * 32 个 filler 任务瞬时共存)留足余量，避免注册表在 RTOSALL 中途溢出、导致
 * 后续模块(尤其唯一走 SVC 门校验的 RTOSUSR)的 IPC 对象登记失败而误判。 */
#define KOBJ_MAX 128
static struct { const char *name; rtos_kobj_type_t type; void *ptr; } g_kobj[KOBJ_MAX];
static int g_kobj_n = 0;

int rtos_kobj_register(const char *name, rtos_kobj_type_t type, void *ptr) {
    if (!ptr || g_kobj_n >= KOBJ_MAX) return 0;
    for (int i = 0; i < g_kobj_n; i++)            /* 按 指针+类型 去重，避免重复登记 */
        if (g_kobj[i].ptr == ptr && g_kobj[i].type == type) return 1;
    g_kobj[g_kobj_n].name = name;                 /* 允许 NULL（仅按指针校验） */
    g_kobj[g_kobj_n].type = type;
    g_kobj[g_kobj_n].ptr  = ptr;
    g_kobj_n++;
    return 1;
}

/* 任务退出时回收其内核对象表条目（按 指针+类型 定位后整体前移、计数减一，
 * 使槽位可被后续登记复用）。IPC 对象(sem/mq/...)为常驻内核对象、不回收。 */
void rtos_kobj_deregister(rtos_kobj_type_t type, void *ptr) {
    if (!ptr) return;
    for (int i = 0; i < g_kobj_n; i++) {
        if (g_kobj[i].ptr == ptr && g_kobj[i].type == type) {
            for (int j = i; j < g_kobj_n - 1; j++) g_kobj[j] = g_kobj[j + 1];
            g_kobj_n--;
            return;
        }
    }
}
void *rtos_kobj_lookup(const char *name) {
    if (!name) return (void *)0;
    for (int i = 0; i < g_kobj_n; i++)
        if (g_kobj[i].name && strcmp(g_kobj[i].name, name) == 0) return g_kobj[i].ptr;
    return (void *)0;
}
void rtos_kobj_foreach(void (*cb)(const char *name, rtos_kobj_type_t type, void *ptr)) {
    if (!cb) return;
    for (int i = 0; i < g_kobj_n; i++)
        cb(g_kobj[i].name, g_kobj[i].type, g_kobj[i].ptr);
}
/* SVC 门校验：指针确为某已登记的内核对象且类型匹配。 */
int rtos_kobj_validate(void *ptr, rtos_kobj_type_t type) {
    if (!ptr) return 0;
    for (int i = 0; i < g_kobj_n; i++)
        if (g_kobj[i].ptr == ptr && g_kobj[i].type == type) return 1;
    return 0;
}
/* 控制台诊断（RTOSKOBJ 命令） */
void rtos_kobj_dump(void) {
    for (int i = 0; i < g_kobj_n; i++)
        log_printf(app_log(), LOG_INFO, "rtos", "[KOBJ] %-16s type=%u ptr=%p\n",
                   g_kobj[i].name ? g_kobj[i].name : "(anon)",
                   (unsigned)g_kobj[i].type, g_kobj[i].ptr);
}
