/**
 * Minimal newlib / newlib-nano syscalls for an STM32 (no OS).
 * stdout/stderr are redirected to the UART via uart_putchar().
 */
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* Provided by uart_stm32.c (console instance) */
extern void uart_stm32_console_putc(char c);

int _write(int file, char *ptr, int len)
{
    (void)file;
    for (int i = 0; i < len; i++)
    {
        if (ptr[i] == '\n')
            uart_stm32_console_putc('\r');   /* add CR for terminal friendliness */
        uart_stm32_console_putc(ptr[i]);
    }
    return len;
}

void *_sbrk(ptrdiff_t incr)
{
    extern char _end;
    static char *heap = &_end;
    char *prev = heap;
    heap += incr;
    return (void *)prev;
}

int _close(int file)        { (void)file; return -1; }
int _fstat(int file, struct stat *st) { (void)file; st->st_mode = S_IFCHR; return 0; }
int _isatty(int file)       { (void)file; return 1; }
int _lseek(int file, int ptr, int dir) { (void)file; (void)ptr; (void)dir; return 0; }
int _read(int file, char *ptr, int len) { (void)file; (void)ptr; (void)len; return 0; }

void _exit(int status)      { (void)status; for (;;) { } }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void)           { return 1; }
