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

static gcov_write_fn g_out = uart_console_raw;   /* 默认走全局 g_console（d_uart/COM8） */

void gcov_set_write(gcov_write_fn fn)
{
    g_out = fn ? fn : uart_console_raw;
}

static void gcov_raw(const uint8_t *p, size_t n) { g_out(p, n); }

/* 发送一帧：magic(4) + name_len(2) + name + data_len(4) + data
 *
 * 关键约束：USB CDC 批量 IN 的 tx_dma_buf 为 64 字节，usb_tx_pump 每次从 TX 环形缓冲里
 * 取【至多 64 字节】去武装一个 IN 包；若某次武装的字节数 < 64（短包），主机会把它当作
 * 本次批量传输结束，而设备侧 bulk_tx_pending 一直等到 XFRC 才清除——下一帧若以 < 64 字节
 * 起步，泵又会武装一个短包，于是整条流的 64 字节对齐被永久破坏，泵最终卡死在 ~16KB。
 *
 * 因此这里【在生产者侧】把二进制帧数据切成严格的 64 字节整块下发（最后一块不足 64 时用
 * 零补齐）。这样 TX 环里只会出现「整 64 字节」写入，usb_tx_pump 永远只武装满 64 字节的
 * IN 包，从根本上消除短包。host 端按真实 dlen 收数据、再按 64 边界跳到下一帧（见
 * coverage_collect.py）。文本标记（START/END）仍可走裸 g_out，不影响二进制帧对齐。 */
#define GCOV_CHUNK 64
static uint8_t g_stage[GCOV_CHUNK];
static size_t  g_stage_len = 0;

/* 把任意长度字节流切成 64 字节整块下发；不足一块时缓存，等攒满或 flush。 */
static void gcov_stage(const uint8_t *p, size_t n)
{
    while (n) {
        size_t room = GCOV_CHUNK - g_stage_len;
        size_t take = n < room ? n : room;
        memcpy(g_stage + g_stage_len, p, take);
        g_stage_len += take; p += take; n -= take;
        if (g_stage_len == GCOV_CHUNK) { gcov_raw(g_stage, GCOV_CHUNK); g_stage_len = 0; }
    }
}
/* 把缓存里不足一块的尾巴用零补齐成 64 字节下发（每帧结束调用，保证帧间 64 对齐）。 */
static void gcov_stage_flush(void)
{
    if (g_stage_len) {
        static const uint8_t zero[GCOV_CHUNK] = {0};
        memcpy(g_stage + g_stage_len, zero, GCOV_CHUNK - g_stage_len);
        gcov_raw(g_stage, GCOV_CHUNK);
        g_stage_len = 0;
    }
}

static void gcov_emit_frame(const char *name, const uint8_t *data, size_t dlen)
{
    uint8_t hdr[6];
    uint16_t nlen = (uint16_t)strlen(name);
    hdr[0] = (uint8_t)(GCOV_MAGIC >> 24); hdr[1] = (uint8_t)(GCOV_MAGIC >> 16);
    hdr[2] = (uint8_t)(GCOV_MAGIC >> 8);  hdr[3] = (uint8_t)GCOV_MAGIC;
    hdr[4] = (uint8_t)(nlen >> 8);        hdr[5] = (uint8_t)nlen;
    gcov_stage(hdr, 6);
    gcov_stage((const uint8_t *)name, nlen);
    uint8_t dl[4];
    dl[0] = (uint8_t)(dlen >> 24); dl[1] = (uint8_t)(dlen >> 16);
    dl[2] = (uint8_t)(dlen >> 8);  dl[3] = (uint8_t)dlen;
    gcov_stage(dl, 4);
    gcov_stage(data, dlen);
    gcov_stage_flush();   /* 每帧以整 64 字节块结束，帧间严格对齐 */
}

/* name_len==0 的帧标记「所有 .gcda 已发完」。经 64 字节分级下发，无妨。 */
static void gcov_emit_end(void)
{
    uint8_t hdr[6] = {
        (uint8_t)(GCOV_MAGIC >> 24), (uint8_t)(GCOV_MAGIC >> 16),
        (uint8_t)(GCOV_MAGIC >> 8),  (uint8_t)GCOV_MAGIC,
        0, 0
    };
    gcov_stage(hdr, 6);
    gcov_stage_flush();
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
    /* 文本标记经当前输出回调（g_out）发送；非二进制关键，但须与帧同一通道。
     * 注意：这里不做 \n->\r（由 host 端 lstrip 处理），保持与帧一致的二进制干净。 */
    static const char start[] = "\n[GCOV DUMP START]\n";
    static const char end[]   = "[GCOV DUMP END]\n";
    g_out((const uint8_t *)start, sizeof(start) - 1);
    __gcov_dump();         /* 触发每个 TU 的 _open/_write/_close 序列 */
    gcov_emit_end();       /* 发送结束帧 */
    g_out((const uint8_t *)end, sizeof(end) - 1);
}

#endif /* RTOS_COVERAGE */
