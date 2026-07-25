#ifndef JOC_BASE_APP_LOG_H
#define JOC_BASE_APP_LOG_H

#include "log/log.h"

/* Shared application logger.
 *
 * This is the single composition point that wires the generic `log` object
 * (src/log/log.{c,h}) to the board's console UART. Every module that wants to
 * emit debug output calls log_printf(app_log(), LEVEL, TAG, ...) instead of
 * raw printf, gaining level filtering + a uniform "L/tag:" prefix.
 *
 * The logger is lazily initialised on first use, so no explicit init call is
 * needed from board_init(). It routes to uart_console_putc (the same sink the
 * toolchain _write() uses), so output lands on the board's debug UART. */
logger *app_log(void);

#endif /* JOC_BASE_APP_LOG_H */
