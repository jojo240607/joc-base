/* Application composition glue: bind the generic `log` object to the board's
 * console UART (direct write, serialized by the UART console path). */
#include "log/app_log.h"
#include "drv/uart.h"

/* Sink: write the finished line straight to the console UART. The UART console
 * path (uart_console_putc) owns the serial wire, so concurrent log_printf
 * callers (any task, incl. the Rust app via dev_write) are serialized there
 * and never interleave on the wire. */
static void uart_log_sink(void *ctx, log_level_t level, const char *data, size_t len)
{
    (void)ctx;
    (void)level;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') uart_console_putc('\r');
        uart_console_putc(data[i]);
    }
}

static logger g_log;
static int    g_log_ready;

logger *app_log(void)
{
    if (!g_log_ready) {
        log_init(&g_log);
        /* Route all output to the console UART (replaces the default stdout
         * sink). set_sink clears any existing sinks and keeps only this one. */
        g_log.fun->set_sink(&g_log, uart_log_sink, NULL);
        /* Show debug-and-below so USB diagnostics (LOG_DEBUG) are emitted. */
        g_log.fun->set_level(&g_log, LOG_DEBUG);
        g_log_ready = 1;
    }
    return &g_log;
}
