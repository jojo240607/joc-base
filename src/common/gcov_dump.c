#include "gcov_dump.h"

/* 仅在覆盖率构建中编译真实实现；非覆盖率构建整个文件为空（gcov_dump 是空操作）。 */
#ifdef RTOS_COVERAGE

#include <string.h>
#include "drv/uart.h"   /* uart_console_raw / uart_console_raw_poll / uart_console_putc */

#ifndef GCOV_MAX_FD
/* 覆盖率构建：.gcda 直接流式导出（见 gcov_on_write），无需整文件缓冲，
 * 故只需 1 个文件槽；常态(非 RAM 受限)可放宽到 4。 */
#define GCOV_MAX_FD 1
#endif
#define GCOV_MAGIC  0x47434441U     /* ASCII "G C D A" */
#define GCOV_CHUNK  64              /* USB CDC 批量 IN 的 64 字节整块对齐 */

/* 单文件槽：只记录文件名 + 活跃标记。.gcda 数据在 gcov_on_write 时直接流式下发，
 * 不缓存整文件——F407 主 SRAM/CCM 均近饱和，8K 级 .gcda 无法静态缓冲（原 2048 缓冲会截断
 * 导致 .gcda 损坏）。缓冲只保留 64 字节分级暂存（g_stage），零大块内存占用。 */
typedef struct {
    int     active;
    char    name[64];
} gcov_file_t;

static gcov_file_t g_gcov[GCOV_MAX_FD];

/* GCC 11+ 起 __gcov_flush 被移除，改用 __gcov_dump（GCC 7.1+ 可用）。 */
extern void __gcov_dump(void);

/* 默认走 polling busy-wait 发送（uart_hal_putc），不依赖 IRQ/DMA TX 状态机 + RTOS 信号量。
 * 原因：gcov_dump 在 RTOSCOV 命令上下文中一次性流式下发整块二进制 .gcda，per-byte 的
 * uart_tx_blocking 在批量二进制下会丢/错字节（IRQ 抢占 + 信号量时序），导致 .gcda 损坏。
 * polling 路径逐字节忙等发送、无状态机、无缓冲截断，二进制透传确定无失真。 */
static gcov_write_fn g_out = uart_console_raw_poll;

void gcov_set_write(gcov_write_fn fn)
{
    g_out = fn ? fn : uart_console_raw_poll;
}

static void gcov_raw(const uint8_t *p, size_t n) { g_out(p, n); }

/* 把任意长度字节流切成 64 字节整块下发；不足一块时缓存，等攒满或 flush。
 * 关键约束：USB CDC 批量 IN 的 tx_dma_buf 为 64 字节，usb_tx_pump 每次从 TX 环形缓冲里取
 * 【至多 64 字节】去武装一个 IN 包；若某次武装字节数 < 64（短包），主机会把它当作本次批量
 * 传输结束，泵最终卡死在 ~16KB。故在生产者侧把二进制帧数据切成严格 64 字节整块下发（最后
 * 一块不足 64 时用零补齐），从根本上消除短包。host 端按真实 dlen 收数据、再按 64 边界跳到
 * 下一帧。 */
static uint8_t g_stage[GCOV_CHUNK];
static size_t  g_stage_len = 0;

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
/* 把缓存里不足一块的尾巴原样下发（不补零）。
 * 注意：早期版本这里用零把尾巴补齐到 64 字节，目的是消除 USB CDC 批量 IN 的「短包」卡死。
 * 但补齐的零字节会混进 .gcda 数据区，使 host 端按 next-magic 切出的 data 长度比真实 .gcda
 * 多若干个零字节 → gcov 解析到文件尾的残缺记录，计数器被当成全 0（覆盖率 0%）。
 * 现默认输出改为 polling UART（uart_hal_putc 逐字节忙等，无短包约束），故【不再补零】，
 * 尾巴原样下发即可。若改用 USB CDC 路径（gcov_set_write(uart_console_raw)），需自行保证
 * 64 字节整块——但那会重新引入补零，故 USB 路径下 host 端应改回按 64 边界切帧。 */
static void gcov_stage_flush(void)
{
    if (g_stage_len) {
        gcov_raw(g_stage, g_stage_len);
        g_stage_len = 0;
    }
}

/* 发出一帧头：magic(4) + name_len(2) + name + data_len(4)=0（流式标记）。
 * data_len 置 0 告知 host：真实 .gcda 数据紧随其后、直到下一 magic/结束帧，无需整文件缓冲。 */
static void gcov_emit_header(const char *name)
{
    uint8_t hdr[6];
    uint16_t nlen = (uint16_t)strlen(name);
    hdr[0] = (uint8_t)(GCOV_MAGIC >> 24); hdr[1] = (uint8_t)(GCOV_MAGIC >> 16);
    hdr[2] = (uint8_t)(GCOV_MAGIC >> 8);  hdr[3] = (uint8_t)GCOV_MAGIC;
    hdr[4] = (uint8_t)(nlen >> 8);        hdr[5] = (uint8_t)nlen;
    gcov_stage(hdr, 6);
    gcov_stage((const uint8_t *)name, nlen);
    uint8_t dl[4] = {0, 0, 0, 0};   /* data_len=0 → 流式：data 直到下一 magic/结束帧 */
    gcov_stage(dl, 4);
}

/* name_len==0 的帧标记「所有 .gcda 已发完」。 */
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
            strncpy(g_gcov[i].name, name, sizeof(g_gcov[i].name) - 1);
            g_gcov[i].name[sizeof(g_gcov[i].name) - 1] = '\0';
            gcov_emit_header(g_gcov[i].name);   /* 开文件即发帧头，数据随后流式到达 */
            return 3 + i;   /* fd 从 3 起，避开 0/1/2 */
        }
    }
    return -1;
}

int gcov_on_write(int fd, const uint8_t *buf, int len)
{
    int idx = fd - 3;
    if (idx < 0 || idx >= GCOV_MAX_FD || !g_gcov[idx].active) return len;
    /* 直接流式下发，不缓存整文件：F407 RAM 装不下 8K 级 .gcda 静态缓冲。 */
    gcov_stage((const uint8_t *)buf, (size_t)len);
    return len;
}

int gcov_on_close(int fd)
{
    int idx = fd - 3;
    if (idx < 0 || idx >= GCOV_MAX_FD || !g_gcov[idx].active) return -1;
    gcov_stage_flush();   /* 补齐本文件末块到 64 字节，下一文件帧头从 64 边界起步 */
    g_gcov[idx].active = 0;
    return 0;
}

void gcov_dump(void)
{
    static const char start[] = "\n[GCOV DUMP START]\n";
    static const char end[]   = "[GCOV DUMP END]\n";
    /* 导出前把控制台 UART 临时切到 STREAM_MODE_POLL：板级默认是 DMA+IDLE，其 DMA TX
     * 流在连续发送后【仍挂载】，此时走 uart_console_raw_poll（uart_hal_putc 直接写 DR）
     * 会与 DMA 竞争 → 偶发只发 START+魔法字就停（108 字节残帧，__gcov_dump 的二进制
     * 全部丢失）。POLL 模式无 DMA 武装，uart_hal_putc 成为 DR 唯一拥有者，逐字节忙等
     * 发送确定无失真。导出结束后恢复原 engine。 */
    stream_xfer_mode_t saved_mode = STREAM_MODE_POLL;
    uart *cons = uart_get_console();
    if (cons) {
        saved_mode = cons->parent.mode;
        cons->parent.mode = STREAM_MODE_POLL;
    }
    g_out((const uint8_t *)start, sizeof(start) - 1);
    __gcov_dump();         /* 触发每个 TU 的 _open/_write/_close 序列（流式下发 .gcda） */
    gcov_emit_end();       /* 发送结束帧 */
    g_out((const uint8_t *)end, sizeof(end) - 1);
    if (cons) cons->parent.mode = saved_mode;   /* 恢复控制台原引擎 */
}

#endif /* RTOS_COVERAGE */
