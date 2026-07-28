#ifndef GCOV_DUMP_H
#define GCOV_DUMP_H

#include <stdint.h>
#include <stddef.h>

/* ===========================================================================
 * gcov 覆盖率数据导出（docs/rtos-test-plan.md §6.6）
 *
 * 裸机 STM32F4 没有文件系统，gcc 的 --coverage 在 __gcov_flush() 时通过 C 库
 * 的 fopen/fwrite/fclose 写出 "<base>.gcda"。本模块把这些写操作截获，按
 * 「魔法字 + 文件名 + 长度 + 数据」的二进制帧，经调试 UART 透传（二进制安全，
 * 不做 \n->\r 转换）。host 端 tools/coverage_collect.py 收帧、落盘 .gcda，
 * 再调 gcov 出报告。
 *
 * 仅当构建开启 -DCOVERAGE=ON（即定义 RTOS_COVERAGE）时才有真实实现；
 * 否则 gcov_dump() 为空操作，其它 TU 可无条件调用而无需 ifdef。
 * ========================================================================= */

#ifdef RTOS_COVERAGE
/* 由 syscalls.c 的 _open/_write/_close 调用，把 .gcda 写流转发到 UART。 */
int  gcov_on_open(const char *name);
int  gcov_on_write(int fd, const uint8_t *buf, int len);
int  gcov_on_close(int fd);

/* 覆盖率数据输出回调：gcov_dump() 用它把 [GCOV DUMP START] 标记、二进制 .gcda
 * 帧、[GCOV DUMP END] 标记逐字节发出。默认指向 uart_console_raw（全局 g_console，
 * 即 d_uart / COM8）。命令处理器在收到 RTOSCOV 时可改指向当前控制台自己的
 * vtable->write（USB CDC 到达时即 d_usb / COM9），从而保证帧从同一通道回传且
 * 二进制安全、非阻塞（uart_console_raw 在 USB 控制台会因 uart_tx_blocking 阻塞）。 */
typedef void (*gcov_write_fn)(const uint8_t *p, size_t n);
void gcov_set_write(gcov_write_fn fn);   /* fn==NULL 还原默认 uart_console_raw */

/* 触发一次全量 flush 并发送 END 帧（RTOSCOV 命令）。 */
void gcov_dump(void);
#else
static inline void gcov_dump(void) { (void)0; }
#endif

#endif /* GCOV_DUMP_H */
