/* 阶段 2 应用分区自举（方案 Y 轻量版）—— 重构版：异步任务化拉起。
 *
 * 架构关键解耦：App 入口【绝不在 RTOS 主线程同步调用】。
 *  - 轨 A（开发期单 ELF）：rust_app_start 由链接器解析，直接作为入口符号；
 *  - 轨 B（部署双分区）：按固定地址读 APP_HEADER_ADDR，校验 magic/abi_version，
 *    取 entry 作为 App 入口。
 * 两条轨都在本函数里：①清零 App RAM（irq_lock 保护）；②创建一个【独立的
 * app_host RTOS 任务】承载 App 入口；③本函数立即返回，console 主线程不被
 * App 初始化阻塞。App 内部经 g_app_slot 服务表再创建自己的业务任务。
 *
 * 收益：即使 App 初始化耗时 / 卡顿 / 内部 panic，console 仍响应（PING 有 PONG），
 * 可通过 APP_LOAD 命令重拉起；App 任务 fault 由系统 fault handler 隔离恢复。 */

#include "app_slot/app_slot.h"
#include "log/log.h"
#include "log/app_log.h"
#include "rtos.h"            /* rtos_task_create_rt, RTOS_TASK_STACK, RTOS_TASK */
#include "common/lock.h"    /* irq_lock / irq_unlock */

/* 契约版本号（与 tools/abi/rtos_abi.h 的 RTOS_ABI_VERSION 及 Rust 侧 app.ld
 * 头部硬编码值三处必须一致）。此处不 include rtos_abi.h（其 device 结构体
 * 与系统 device.h 重复定义会冲突）；版本号作为单一标量本地定义。
 * 改契约（app_slot_t 字段/签名）时必须同步 +1 本值。 */
#define RTOS_ABI_VERSION 1

/* App 运行期 .bss 专用 RAM 块（链接脚本 APP_RAM 段）。起点/尺寸由 CMake 经
 * APP_RAM_BASE / APP_RAM_SIZE 宏注入（与 linker 的 APP_RAM ORIGIN/LENGTH 一致，
 * 开发版 8KB、发布版约 101KB，详见 CMakeLists.txt 的 App RAM 分流）。
 * 整个块在挂载前清零——App 独立镜像没有自己的 C 启动 pre-init，其 .bss
 * 必须由系统加载器清零（App 不使用 .data，故无需 LMA 拷贝）。 */
#ifndef APP_RAM_BASE
#define APP_RAM_BASE   0x20006000u
#endif
#ifndef APP_RAM_SIZE
#define APP_RAM_SIZE   0x17C00u
#endif

/* app_host 任务运行体栈（独立栈，避免占用 App 业务栈 / 主栈）。
 * 2KB 足够承载 rust_app_start 入口初始化（内部再创建业务任务后返回）。
 * RTOS_TASK_STACK 宏已含 static + .ccm_bss 段属性。 */
RTOS_TASK_STACK(app_host_stack, 2048);

/* 重入保护：非 0 表示 app_host 任务已拉起，避免重复 app_slot_load_app
 * 创建多个 app_host 实例覆盖同一个栈。 */
volatile int g_app_loaded = 0;

/* 待拉起的 App 入口（轨 A=rust_app_start，轨 B=header->entry|1）。app_host
 * 任务体从这里取，避免把函数指针塞进 rtos_task_create 的 arg（arm-none-eabi
 * 下 void* 与函数指针等宽，但用全局更清晰）。 */
static int (*g_app_entry)(void) = (int (*)(void))0;

/* 轨 A：rust_app_start 由链接器解析（仅 RUST_APP_LIB 构建提供符号）。
 * 用弱符号占位，未链入 libapp.a 时为空，本函数据此判断轨 A 是否可用。 */
#ifdef RUST_APP_LIB
extern int rust_app_start(void);
#else
#define rust_app_start ((int (*)(void))0)
#endif

/* --------------------------------------------------------------------------
 * app_host 任务运行体：在独立 RTOS 任务里调用 App 入口。
 * 主线程在 app_slot_load_app() 创建本任务后立即返回，故 App 初始化不阻塞
 * console。App 入口（rust_app_start）正常应在内部创建业务任务后返回；
 * 若它返回了（demo 或卸载），本任务进入阻塞睡眠，不再占用 CPU；若它永不
 * 返回（卡死），也只是本任务卡，console 主线程和 RTOS 其他任务照常运行。 */
void app_host_task_entry(void *arg)
{
    (void)arg;
    int (*entry)(void) = g_app_entry;
    if (!entry) {
        log_printf(app_log(), LOG_ERROR, "app_slot",
                   "[app_host] null entry, abort\n");
        g_app_loaded = 0;
        return;
    }

    log_printf(app_log(), LOG_INFO, "app_slot",
               "[app_host] starting App entry @0x%08X...\n",
               (unsigned)((uintptr_t)entry & ~1u));

    int rc = entry();   /* 调用 App 入口；内部通常创建业务任务后返回 0 */

    log_printf(app_log(), LOG_INFO, "app_slot",
               "[app_host] App entry returned rc=%d (App mounted)\n", rc);

    /* App 入口已返回：保持本任务存活但不占 CPU（避免被回收后 g_app_loaded
     * 语义丢失；若 App 需持久运行，其业务任务在各自 RTOS 任务里跑）。
     * 阻塞睡眠即可，不占用调度。 */
    for (;;) {
        rtos_msleep(1000u);
    }
}

/* --------------------------------------------------------------------------
 * 应用分区自举统一入口（轨 A / 轨 B）。
 *  1) 决定 App 入口：
 *       轨 A（RUST_APP_LIB）：rust_app_start 链接器符号；
 *       轨 B：读 APP_HEADER_ADDR，校验 magic + abi_version + entry 范围。
 *  2) 清零 App 专用 RAM（irq_lock 保护，防 IRQ 改写清零中的内存）。
 *  3) 创建独立 app_host 任务承载入口；立即返回。
 * 返回 0=已挂载/无 App（正常），<0=校验失败。 */
int app_slot_load_app(void)
{
    /* ---- 1) 决定 App 入口 ---- */
    uintptr_t entry_addr = 0;

#ifdef RUST_APP_LIB
    /* 轨 A：链接器已解析 rust_app_start 符号 */
    if (rust_app_start) {
        entry_addr = (uintptr_t)rust_app_start;
        log_printf(app_log(), LOG_INFO, "app_slot",
                   "[boot] RUST_APP_LIB: using linked rust_app_start @0x%08X\n",
                   (unsigned)entry_addr);
    } else {
        log_printf(app_log(), LOG_INFO, "app_slot",
                   "[boot] RUST_APP_LIB set but rust_app_start unresolved\n");
        return -1;
    }
#else
    /* 轨 B：读分区头部 */
    const volatile app_header_t *hdr =
        (const volatile app_header_t *)APP_HEADER_ADDR;

    if (hdr->magic != APP_HEADER_MAGIC) {
        log_printf(app_log(), LOG_INFO, "app_slot",
                   "[boot] no app partition (header magic=0x%08X)\n",
                   (unsigned)hdr->magic);
        return 0;
    }
    if (hdr->abi_version != RTOS_ABI_VERSION) {
        log_printf(app_log(), LOG_ERROR, "app_slot",
                   "[boot] app ABI mismatch: app=%u sys=%u -> SKIP\n",
                   (unsigned)hdr->abi_version, (unsigned)RTOS_ABI_VERSION);
        return -1;
    }
    if (hdr->entry < APP_FLASH_BASE ||
        hdr->entry >= (APP_FLASH_BASE + 0x00060000u)) {
        log_printf(app_log(), LOG_ERROR, "app_slot",
                   "[boot] app entry 0x%08X out of APP_FLASH range -> SKIP\n",
                   (unsigned)hdr->entry);
        return -1;
    }
    entry_addr = hdr->entry;
    log_printf(app_log(), LOG_INFO, "app_slot",
               "[boot] app partition found: entry=0x%08X size=%u\n",
               (unsigned)hdr->entry, (unsigned)hdr->app_size);
#endif

    if (g_app_loaded) {
        log_printf(app_log(), LOG_WARN, "app_slot",
                   "[boot] App already loaded, skip duplicate load\n");
        return 0;
    }

#ifndef RUST_APP_LIB
    /* ---- 2) 清零 App 运行期 RAM 块（.bss，APP_RAM 区；App 不使用 .data）----
     * 仅轨 B（从 APP_FLASH 分区加载镜像）需要：镜像的 .bss 运行时须清零。
     * 轨 A（RUST_APP_LIB）App 已链进主 ELF，其 .bss 由 startup 的 C 初始化
     * 代码清零，且 g_app_loaded/g_app_entry 等系统全局变量恰落在 APP_RAM 区域
     * 内——若此处再清零 95KB，会把这些系统变量清 0、并掐断 running 任务的
     * 状态，导致进 IRQ 风暴卡死。故轨 A 跳过。 */
    unsigned st = irq_lock();
    for (volatile char *p = (volatile char *)APP_RAM_BASE;
         p < (volatile char *)(APP_RAM_BASE + APP_RAM_SIZE); ++p)
        *p = 0;
    irq_unlock(st);
#endif

    /* ---- 3) 创建独立 app_host 任务承载 App 入口（异步，不阻塞主线程）----
     * Cortex-M 间接分支目标须带 Thumb 位(bit0=1)，故 OR 1。 */
    g_app_entry = (int (*)(void))(entry_addr | 1u);
    g_app_loaded = 1;

    rtos_task_create("app_host", app_host_task_entry, NULL,
                     20, app_host_stack, sizeof(app_host_stack));

    log_printf(app_log(), LOG_INFO, "app_slot",
               "[boot] app_host task spawned (async mount)\n");
    return 0;
}
