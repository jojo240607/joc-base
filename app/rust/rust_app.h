#ifndef RUST_APP_H
#define RUST_APP_H

/* Rust 应用层（独立工程 joc-app-rust，cargo build 出 libapp.a）暴露给 C 侧的 C ABI。
 * 这些符号由 Rust staticlib 提供，与内核其余部分一起链接进同一 ELF。
 * RTOS 只通过本头声明引用 Rust 侧应用符号；Rust 侧经 tools/abi/rtos_abi.h 契约
 * 调用内核/驱动。两者解耦。 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rust demo 任务入口：经 rtos_task_create 像普通 C 任务一样挂上。 */
void rust_task_entry(void *arg);

/* 返回 Rust demo 任务累计心跳次数（证明它在 RTOS 上运行）。 */
uint32_t rust_ticks(void);

/* 等待一次 Rust demo 任务的信号量 give（证明跨语言 SVC 门 IPC 可用）。 */
int rust_wait_once(void);

/* ---- 飞控示例符号（joc-app-rust/src/lib.rs）---- */
/* 飞控姿态环任务入口（硬实时，prio=3 <= RTOS_PRIO_BH_HIGH）。 */
void rust_attitude_loop(void *arg);

/* App 统一挂载入口：无参，由 RTOS 经 g_app_slot.app_start 调用。
 * 内部只引用 g_app_slot 服务表（方案 Y 解耦），不再直接引用裸 RTOS 符号。
 * 旧版经裸 ctx 注入 app_ctx_t 的签名已废弃；App 经服务表拿到一切能力。 */
int rust_app_start(void);

#ifdef __cplusplus
}
#endif

#endif /* RUST_APP_H */
