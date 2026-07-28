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

/* 触发一次全量 flush 并发送 END 帧（RTOSCOV 命令）。 */
void gcov_dump(void);
#else
static inline void gcov_dump(void) { (void)0; }
#endif

#endif /* GCOV_DUMP_H */
