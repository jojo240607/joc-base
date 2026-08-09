/**
 * Minimal newlib / newlib-nano syscalls for an STM32 (no OS).
 * stdout/stderr are redirected to the UART via uart_putchar().
 */
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* Provided by uart.c (console instance) */
extern void uart_console_putc(char c);

#ifdef RTOS_COVERAGE
/* gcov 覆盖率构建：把 .gcda 的 fopen/fwrite/fclose 流转发到 UART（见 gcov_dump.c）。
 * 只有 fd>=3 且被 gcov_on_open 接管的描述符才走二进制透传，stdout/stderr 仍走
 * 正常（带 CR）通道。 */
#include "common/gcov_dump.h"
#endif

int _write(int file, char *ptr, int len)
{
#ifdef RTOS_COVERAGE
    if (file >= 3 && file < 3 + 4) {   /* 与 gcov_dump.c 的 GCOV_MAX_FD 对应 */
        return gcov_on_write(file, (const uint8_t *)ptr, len);
    }
#endif
    (void)file;
    for (int i = 0; i < len; i++)
    {
        if (ptr[i] == '\n')
            uart_console_putc('\r');   /* add CR for terminal friendliness */
        uart_console_putc(ptr[i]);
    }
    return len;
}

#ifdef RTOS_COVERAGE
/* gcov 经 C 库 fopen -> _open 打开 "<base>.gcda"；返回非负 fd 即接管。 */
int _open(const char *name, int flags, int mode)
{
    (void)flags; (void)mode;
    return gcov_on_open(name);
}

int _close(int file)
{
    if (file >= 3 && file < 3 + 4)
        return gcov_on_close(file);
    return -1;
}

/* .gcda 描述符当作普通文件（全缓冲），避免被当成 tty 行缓冲而打乱二进制帧。 */
int _fstat(int file, struct stat *st)
{
    (void)file;
    st->st_mode = (file >= 3) ? S_IFREG : S_IFCHR;
    return 0;
}
#else
int _close(int file)        { (void)file; return -1; }
int _fstat(int file, struct stat *st) { (void)file; st->st_mode = S_IFCHR; return 0; }
#endif

/* 独立静态系统堆(SYS_STATIC_HEAP 定义时启用，开发版与发布版均启用)：
 * board_init() 要为全部板级设备(当前 57 个)逐一 malloc 驱动结构体 + HAL 句柄 +
 * ring 缓冲，累计远超默认链接器堆(_end..__HeapLimit)。堆耗尽会让靠后设备
 * (如第 54 个 usb0)的 malloc 返回 NULL、device_manager 静默跳过注册，表现为
 * "usb0: NOT REGISTERED"。把系统堆搬进本静态数组，根治堆耗尽。
 *
 * 放置位置按构建分流(由 CMake 经 SYS_HEAP_SECTION 注入段名)：
 *   - 发布版(RTOS_SELFTEST=OFF)：主 SRAM .bss(到 0x20006000 仍有余量)，32KB。
 *   - 开发版(RTOS_SELFTEST=ON)：系统 .bss 已占满近全部主 SRAM(约 124KB/128KB)，
 *     无空间再放静态堆；而 CCMRAM(0x10000000)仅被 RTOS 任务栈/TCB 占用约 56KB/
 *     63KB，尚余 ~7KB。驱动对象(usb/usb_hal 等)为纯软件结构体、不含 DMA 目标缓冲
 *     (USB OTG 用外设内部 FIFO，不搬系统 RAM)，放 CCM 安全。故开发版静态堆置于
 *     CCM(.ccm_bss)，与 App RAM(0x20006000 起)及主 SRAM .bss 均物理隔离。
 * SYS_HEAP_SIZE / SYS_HEAP_SECTION 由 CMake 注入。 */
#ifdef SYS_STATIC_HEAP
#ifndef SYS_HEAP_SIZE
#define SYS_HEAP_SIZE 0x2000u
#endif
#ifndef SYS_HEAP_SECTION
#define SYS_HEAP_SECTION .bss
#endif
static uint8_t g_sys_heap[SYS_HEAP_SIZE] __attribute__((section(SYS_HEAP_SECTION))) __attribute__((aligned(8)));

void *_sbrk(ptrdiff_t incr)
{
    static char *heap = (char *)g_sys_heap;
    char *prev = heap;
    char *limit = (char *)g_sys_heap + sizeof(g_sys_heap);
    /* 越界保护：堆不得超过 g_sys_heap 上界，否则返回 (void *)-1
     * 让 malloc 失败而非吐出未映射区的野指针(曾导致 coverage 构建启动总线错误)。 */
    if (incr > 0 && (heap + incr) > limit) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap += incr;
    return (void *)prev;
}
#else
void *_sbrk(ptrdiff_t incr)
{
    extern char _end;
    extern char __HeapLimit;
    static char *heap = &_end;
    char *prev = heap;
    /* 越界保护：堆不得超过主 SRAM 上界(__HeapLimit)，否则返回 (void *)-1
     * 让 malloc 失败而非吐出未映射区的野指针(曾导致 coverage 构建启动总线错误)。 */
    if (incr > 0 && (heap + incr) > &__HeapLimit) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap += incr;
    return (void *)prev;
}
#endif

int _isatty(int file)       { (void)file; return (file < 3) ? 1 : 0; }
int _lseek(int file, int ptr, int dir) { (void)file; (void)ptr; (void)dir; return 0; }
int _read(int file, char *ptr, int len) { (void)file; (void)ptr; (void)len; return 0; }

void _exit(int status)      { (void)status; for (;;) { } }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void)           { return 1; }
