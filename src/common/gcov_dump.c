#include "gcov_dump.h"

/* 仅在覆盖率构建中编译真实实现；非覆盖率构建整个文件为空（gcov_dump 是空操作）。 */
#ifdef RTOS_COVERAGE

#include <string.h>
#include "drv/uart.h"   /* uart_console_raw / uart_console_putc */

#ifndef GCOV_MAX_FD
#define GCOV_MAX_FD 4
#endif
#define GCOV_BUF_SZ 8192            /* 单个 .gcda 远小于此；足够容纳累加缓冲 */
#define GCOV_MAGIC  0x47434441U     /* ASCII "G C D A" */

typedef struct {
    int      active;
    char     name[64];
    uint8_t  buf[GCOV_BUF_SZ];
    size_t   len;
} gcov_file_t;

static gcov_file_t g_gcov[GCOV_MAX_FD];

/* GCC 11+ 起 __gcov_flush 被移除，改用 __gcov_dump（GCC 7.1+ 可用）。
 * 本项目工具链为 GNU Tools for STM32 13.3.1，故用 __gcov_dump。 */
extern void __gcov_dump(void);

static void gcov_raw(const uint8_t *p, size_t n) { uart_console_raw(p, n); }

/* 发送一帧：magic(4) + name_len(2) + name + data_len(4) + data */
static void gcov_emit_frame(const char *name, const uint8_t *data, size_t dlen)
{
    uint8_t hdr[6];
    uint16_t nlen = (uint16_t)strlen(name);
    hdr[0] = (uint8_t)(GCOV_MAGIC >> 24); hdr[1] = (uint8_t)(GCOV_MAGIC >> 16);
    hdr[2] = (uint8_t)(GCOV_MAGIC >> 8);  hdr[3] = (uint8_t)GCOV_MAGIC;
    hdr[4] = (uint8_t)(nlen >> 8);        hdr[5] = (uint8_t)nlen;
    gcov_raw(hdr, 6);
    gcov_raw((const uint8_t *)name, nlen);
    uint8_t dl[4];
    dl[0] = (uint8_t)(dlen >> 24); dl[1] = (uint8_t)(dlen >> 16);
    dl[2] = (uint8_t)(dlen >> 8);  dl[3] = (uint8_t)dlen;
    gcov_raw(dl, 4);
    gcov_raw(data, dlen);
}

/* name_len==0 的帧标记「所有 .gcda 已发完」。 */
static void gcov_emit_end(void)
{
    uint8_t hdr[6] = {
        (uint8_t)(GCOV_MAGIC >> 24), (uint8_t)(GCOV_MAGIC >> 16),
        (uint8_t)(GCOV_MAGIC >> 8),  (uint8_t)GCOV_MAGIC,
        0, 0
    };
    gcov_raw(hdr, 6);
}

int gcov_on_open(const char *name)
{
    if (!name || strstr(name, ".gcda") == NULL) return -1;  /* 只接管 .gcda */
    for (int i = 0; i < GCOV_MAX_FD; i++) {
        if (!g_gcov[i].active) {
            g_gcov[i].active = 1;
            g_gcov[i].len = 0;
            strncpy(g_gcov[i].name, name, sizeof(g_gcov[i].name) - 1);
            g_gcov[i].name[sizeof(g_gcov[i].name) - 1] = '\0';
            return 3 + i;   /* fd 从 3 起，避开 0/1/2 */
        }
    }
    return -1;
}

int gcov_on_write(int fd, const uint8_t *buf, int len)
{
    int idx = fd - 3;
    if (idx < 0 || idx >= GCOV_MAX_FD || !g_gcov[idx].active) return len;
    size_t room = GCOV_BUF_SZ - g_gcov[idx].len;
    size_t take = (size_t)len < room ? (size_t)len : room;
    if (take) {
        memcpy(g_gcov[idx].buf + g_gcov[idx].len, buf, take);
        g_gcov[idx].len += take;
    }
    return len;
}

int gcov_on_close(int fd)
{
    int idx = fd - 3;
    if (idx < 0 || idx >= GCOV_MAX_FD || !g_gcov[idx].active) return -1;
    gcov_emit_frame(g_gcov[idx].name, g_gcov[idx].buf, g_gcov[idx].len);
    g_gcov[idx].active = 0;
    g_gcov[idx].len = 0;
    return 0;
}

void gcov_dump(void)
{
    /* 文本标记走正常（带 CR）通道，便于 host 端定位二进制帧起点/终点。 */
    static const char start[] = "\n[GCOV DUMP START]\n";
    static const char end[]   = "[GCOV DUMP END]\n";
    for (const char *p = start; *p; p++) uart_console_putc(*p);
    __gcov_dump();         /* 触发每个 TU 的 _open/_write/_close 序列 */
    gcov_emit_end();       /* 发送结束帧 */
    for (const char *p = end; *p; p++) uart_console_putc(*p);
}

#endif /* RTOS_COVERAGE */
