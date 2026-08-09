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

/* 独立静态系统堆（仅发布版 SYS_STATIC_HEAP 定义时启用）：
 * board_init() 要为全部板级设备(当前 57 个)逐一 malloc 驱动结构体 + HAL 句柄 +
 * ring 缓冲，累计远超默认链接器堆(_end..__HeapLimit, 发布版仅约 4.6KB)。
 * 堆耗尽会让靠后设备(如第 54 个 usb0)的 malloc 返回 NULL、device_manager 静默
 * 跳过注册，表现为 "usb0: NOT REGISTERED"。
 * 把系统堆搬进本静态数组(置于 .bss，发布版 .bss 区到 0x20006000 仍有余量)，
 * 与 App RAM(0x20006000 起)物理隔离，既保住 App 层大块内存，又根治堆耗尽。
 * 开发版(SELFTEST=ON)系统 .bss 已近 0x2001DC00(g_app_slot 固定地址)，无空间放
 * 静态堆，故保持原链接器堆(_end..__HeapLimit, 约 3.4KB)——开发版不挂载真 App、
 * 设备注册不全可接受，且不引入 g_sys_heap 避免 .bss 越过 g_app_slot。
 * SYS_HEAP_SIZE 由 CMake 注入(发布版 32KB)。 */
#ifdef SYS_STATIC_HEAP
#ifndef SYS_HEAP_SIZE
#define SYS_HEAP_SIZE 0x8000u
#endif
static uint8_t g_sys_heap[SYS_HEAP_SIZE] __attribute__((aligned(8)));

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
