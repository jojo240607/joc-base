/* 阶段 2 应用分区自举（方案 Y 轻量版）。
 * RTOS 启动后按固定地址(APP_HEADER_ADDR)读 App 头部，校验 magic/abi_version，
 * 清零 App 专用 RAM，取 entry 作为 App 入口经 g_app_slot 服务表挂载。
 * App 镜像与系统镜像完全解耦：系统区烧一次，之后只烧 APP_FLASH 块。 */

#include "app_slot/app_slot.h"
#include "log/log.h"
#include "log/app_log.h"

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

int app_slot_load_app(void)
{
    const volatile app_header_t *hdr =
        (const volatile app_header_t *)APP_HEADER_ADDR;

    /* 空分区（全 0xFF）或 magic 不符 → 未烧 App，纯 C 固件行为 */
    if (hdr->magic != APP_HEADER_MAGIC) {
        log_printf(app_log(), LOG_INFO, "app_slot",
                   "[boot] no app partition (header magic=0x%08X)\n",
                   (unsigned)hdr->magic);
        return 0;
    }

    /* ABI 版本错配 → 拒绝挂载，不崩（防止契约漂移导致诡异崩溃） */
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

    /* 清零 App 运行期 RAM 块（.bss，APP_RAM 区；App 不使用 .data） */
    for (volatile char *p = (volatile char *)APP_RAM_BASE;
         p < (volatile char *)(APP_RAM_BASE + APP_RAM_SIZE); ++p)
        *p = 0;

    log_printf(app_log(), LOG_INFO, "app_slot",
               "[boot] app partition found: entry=0x%08X size=%u -> mounting\n",
               (unsigned)hdr->entry, (unsigned)hdr->app_size);

    /* 经服务表挂载：把 App 入口钉到 g_app_slot.app_start，再经契约调用。
     * 这样 App 内部只引用 g_app_slot（固定地址 0x2001DC00），不依赖任何
     * 链接进系统的 App 符号，实现真·双分区解耦。
     *
     * 关键：Cortex-M 的间接分支(BLX/BX)目标地址【必须带 Thumb 位(bit0=1)】，
     * 否则 CPU 误判为 ARM 态 → INVSTATE UsageFault(cfsr 0x00020000)。头部里
     * 的 entry 是裸地址(bit0=0)，故此处 OR 1 补上 Thumb 位。裸地址用于
     * range 校验(entry>=APP_FLASH_BASE)，调用前再补位。 */
    g_app_slot.app_start = (int (*)(void))(hdr->entry | 1u);
    return g_app_slot.app_start();
}
