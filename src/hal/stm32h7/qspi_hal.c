#include "qspi_hal.h"
#include "stm32h7xx.h"
#include <stddef.h>

/*
 * STM32H7 QUADSPI HAL — memory-mapped (XIP) init + indirect programming.
 *
 * Indirect flow (all 1-line SPI commands, W25Q-class flash):
 *   1. ensure the controller is idle (SR.BUSY=0) and clear status flags;
 *   2. write AR (address) + DLR (data length - 1);
 *   3. write CCR with the instruction / modes / FMODE for this command;
 *   4. pump data through DR (FIFO threshold = 1 byte, FTHRES=0);
 *   5. wait TCF (transfer complete), clear it, restore the MM-mode CCR;
 *   6. for program/erase, poll WIP via RDSR (0x05) until the flash is idle.
 *
 * The previous CCR (memory-mapped config from qspi_hal_init_mm) is restored
 * after every command, so XIP stays functional between operations.
 *
 * Under Renode (JOC_RENODE=1) init_mm is a no-op and every indirect
 * operation returns -1: Renode has no QUADSPI peripheral model (the .resc
 * loads the app straight into the 0x90000000 MappedMemory), so indirect
 * programming is a real-hardware-only path.
 */

/* W25Q command set (1-line) */
#define QSPI_CMD_WREN       0x06u   /* write enable             */
#define QSPI_CMD_RDSR       0x05u   /* read status register     */
#define QSPI_CMD_PAGE_PROG  0x02u   /* page program (<=256 B)   */
#define QSPI_CMD_SECTOR_ER  0x20u   /* 4 KB sector erase        */
#define QSPI_CMD_BLOCK_ER   0xD8u   /* 64 KB block erase        */
#define QSPI_CMD_READ       0x03u   /* 1-line read              */
#define QSPI_CMD_JEDEC_ID   0x9Fu   /* read JEDEC ID            */

#define QSPI_SR_WIP         0x01u   /* status register bit 0    */

/* Busy-wait loop budgets (480 MHz core; flash WIP can take ~400 ms on a
 * 4 KB sector erase). Loop iteration is a few cycles, so ~2.4e8 iterations
 * cover the worst-case erase. */
#define QSPI_IDLE_TIMEOUT   100000u      /* controller BUSY -> idle   */
#define QSPI_WIP_TIMEOUT    240000000UL  /* flash WIP -> idle         */

/* All-clear value for FCR (write-1-to-clear flags). */
#define QSPI_FLAG_CLR_ALL   (QUADSPI_FCR_CTEF | QUADSPI_FCR_CTCF | \
                             QUADSPI_FCR_CSMF | QUADSPI_FCR_CTOF)

/* Static MM-mode CCR captured at qspi_hal_init_mm(); restored after every
 * indirect command so the XIP window keeps working. */
static uint32_t g_ccr_mm = 0u;
static int g_initialized = 0;

int qspi_hal_init_mm(void)
{
#if defined(JOC_RENODE)
    return 0;
#else
    /* 1. clock gate (QSPI on AHB3) */
    RCC->AHB3ENR |= RCC_AHB3ENR_QSPIEN;

    /* 2. prescaler: QSPI clock = 240 MHz / (2 * (PRESCALER + 1)).
     *    PRESCALER = 5 -> 240 / 12 = 20 MHz (safe for any flash). */
    QUADSPI->CR &= ~QUADSPI_CR_PRESCALER_Msk;
    QUADSPI->CR |= (5U << QUADSPI_CR_PRESCALER_Pos);

    /* 3. device config: 16 MB flash, min CS high time 8 cycles */
    QUADSPI->DCR = (23U << QUADSPI_DCR_FSIZE_Pos) | (8U << QUADSPI_DCR_CSHT_Pos);

    /* clear any stale flags before starting */
    QUADSPI->FCR = QSPI_FLAG_CLR_ALL;

    /* 4. communication config: 0xEB quad-I/O fast read, memory-mapped */
    QUADSPI->CCR = 0xEBU
                 | (0x1UL << QUADSPI_CCR_IMODE_Pos)  /* instruction: 1 line  */
                 | (0x3UL << QUADSPI_CCR_ADMODE_Pos) /* address:    4 lines  */
                 | (0x2UL << QUADSPI_CCR_ADSIZE_Pos) /* address size: 24 bits */
                 | (6U << QUADSPI_CCR_DCYC_Pos)      /* 6 dummy cycles        */
                 | (0x3UL << QUADSPI_CCR_DMODE_Pos)  /* data:       4 lines  */
                 | QUADSPI_CCR_FMODE_MM;             /* memory-mapped         */

    /* 5. enable */
    QUADSPI->CR |= QUADSPI_CR_EN;

    __DSB();
    g_ccr_mm = QUADSPI->CCR;
    g_initialized = 1;
    return 0;
#endif
}

#if defined(JOC_RENODE)
/* Renode: no QUADSPI model — every indirect operation reports unavailable. */
int qspi_hal_read_id(uint8_t id[3]) { (void)id; return -1; }
int qspi_hal_erase_sector(uint32_t addr) { (void)addr; return -1; }
int qspi_hal_erase_block(uint32_t addr) { (void)addr; return -1; }
int qspi_hal_program(uint32_t addr, const uint8_t *data, uint32_t len)
    { (void)addr; (void)data; (void)len; return -1; }
int qspi_hal_read(uint32_t addr, uint8_t *data, uint32_t len)
    { (void)addr; (void)data; (void)len; return -1; }
int qspi_hal_wait_ready(void) { return -1; }
#endif /* JOC_RENODE */

#if !defined(JOC_RENODE)

/* Wait until the QUADSPI controller itself is idle (SR.BUSY=0). */
static int qspi_wait_ctrl_idle(void)
{
    volatile uint32_t n = 0;
    while (QUADSPI->SR & QUADSPI_SR_BUSY) {
        if (++n > QSPI_IDLE_TIMEOUT) return -1;
    }
    return 0;
}

/* Wait for transfer-complete flag (TCF), then clear all flags. */
static int qspi_wait_tcf(void)
{
    volatile uint32_t n = 0;
    while (!(QUADSPI->SR & QUADSPI_SR_TCF)) {
        if (QUADSPI->SR & QUADSPI_SR_TEF) return -1;   /* transfer error */
        if (++n > QSPI_IDLE_TIMEOUT) return -1;
    }
    QUADSPI->FCR = QSPI_FLAG_CLR_ALL;
    return 0;
}

/* Issue WREN (0x06) — required before every program/erase. */
static int qspi_write_enable(void)
{
    if (qspi_wait_ctrl_idle()) return -1;
    QUADSPI->FCR = QSPI_FLAG_CLR_ALL;
    QUADSPI->CCR = QSPI_CMD_WREN
                 | (0x1UL << QUADSPI_CCR_IMODE_Pos)   /* 1-line instruction  */
                 | (0x0UL << QUADSPI_CCR_DMODE_Pos)   /* no data phase       */
                 | (0x0UL << QUADSPI_CCR_FMODE_Pos);  /* indirect write      */
    return qspi_wait_tcf();
}

int qspi_hal_wait_ready(void)
{
    uint8_t sr;
    volatile uint32_t n = 0;
    do {
        /* indirect read of status register (1 byte, no address) */
        if (qspi_wait_ctrl_idle()) return -1;
        QUADSPI->FCR = QSPI_FLAG_CLR_ALL;
        QUADSPI->DLR = 0u;                             /* 1 byte */
        QUADSPI->CCR = QSPI_CMD_RDSR
                     | (0x1UL << QUADSPI_CCR_IMODE_Pos)
                     | (0x1UL << QUADSPI_CCR_DMODE_Pos)
                     | QUADSPI_CCR_FMODE_0;            /* indirect read */
        volatile uint32_t m = 0;
        while (!(QUADSPI->SR & QUADSPI_SR_FTF)) {
            if (++m > QSPI_IDLE_TIMEOUT) return -1;
        }
        sr = (uint8_t)(QUADSPI->DR & 0xFFu);
        if (qspi_wait_tcf()) return -1;
        if (++n > QSPI_WIP_TIMEOUT) return -1;
    } while (sr & QSPI_SR_WIP);
    QUADSPI->CCR = g_ccr_mm;   /* restore memory-mapped mode */
    return 0;
}

/* Common indirect-write command: instruction, optional 24-bit address,
 * optional data phase (NULL => none). Restores MM CCR on exit. */
static int qspi_cmd_ind_write(uint8_t inst, uint32_t addr, int has_addr,
                              const uint8_t *data, uint32_t len)
{
    if (qspi_wait_ctrl_idle()) return -1;
    QUADSPI->FCR = QSPI_FLAG_CLR_ALL;
    if (has_addr) QUADSPI->AR = addr & 0x00FFFFFFu;
    if (data && len) QUADSPI->DLR = len - 1u;
    QUADSPI->CCR = (uint32_t)inst
                 | (0x1UL << QUADSPI_CCR_IMODE_Pos)
                 | (has_addr ? (0x1UL << QUADSPI_CCR_ADMODE_Pos) : 0u)
                 | (0x2UL << QUADSPI_CCR_ADSIZE_Pos)   /* 24-bit address */
                 | (data && len ? (0x1UL << QUADSPI_CCR_DMODE_Pos) : 0u)
                 | (0x0UL << QUADSPI_CCR_FMODE_Pos);   /* indirect write */

    if (data && len) {
        for (uint32_t i = 0; i < len; i++) {
            volatile uint32_t m = 0;
            while (!(QUADSPI->SR & QUADSPI_SR_FTF)) {
                if (++m > QSPI_IDLE_TIMEOUT) return -1;
            }
            QUADSPI->DR = data[i];
        }
    }
    int rc = qspi_wait_tcf();
    QUADSPI->CCR = g_ccr_mm;
    return rc;
}

/* Common indirect-read command: instruction, optional 24-bit address,
 * read len bytes into data. Restores MM CCR on exit. */
static int qspi_cmd_ind_read(uint8_t inst, uint32_t addr, int has_addr,
                             uint8_t *data, uint32_t len)
{
    if (!data || len == 0) return -1;
    if (qspi_wait_ctrl_idle()) return -1;
    QUADSPI->FCR = QSPI_FLAG_CLR_ALL;
    if (has_addr) QUADSPI->AR = addr & 0x00FFFFFFu;
    QUADSPI->DLR = len - 1u;
    QUADSPI->CCR = (uint32_t)inst
                 | (0x1UL << QUADSPI_CCR_IMODE_Pos)
                 | (has_addr ? (0x1UL << QUADSPI_CCR_ADMODE_Pos) : 0u)
                 | (0x2UL << QUADSPI_CCR_ADSIZE_Pos)   /* 24-bit address */
                 | (0x1UL << QUADSPI_CCR_DMODE_Pos)
                 | QUADSPI_CCR_FMODE_0;                /* indirect read */

    for (uint32_t i = 0; i < len; i++) {
        volatile uint32_t m = 0;
        while (!(QUADSPI->SR & QUADSPI_SR_FTF)) {
            if (++m > QSPI_IDLE_TIMEOUT) return -1;
        }
        data[i] = (uint8_t)(QUADSPI->DR & 0xFFu);
    }
    int rc = qspi_wait_tcf();
    QUADSPI->CCR = g_ccr_mm;
    return rc;
}

int qspi_hal_read_id(uint8_t id[3])
{
    if (!g_initialized) return -1;
    return qspi_cmd_ind_read(QSPI_CMD_JEDEC_ID, 0u, 0, id, 3u);
}

int qspi_hal_erase_sector(uint32_t addr)
{
    if (!g_initialized) return -1;
    if (addr >= QSPI_FLASH_SIZE || (addr & (QSPI_SECTOR_SIZE - 1u))) return -1;
    if (qspi_write_enable()) return -1;
    int rc = qspi_cmd_ind_write(QSPI_CMD_SECTOR_ER, addr, 1, NULL, 0);
    if (rc) return rc;
    return qspi_hal_wait_ready();   /* flash-internal erase, up to ~400 ms */
}

int qspi_hal_erase_block(uint32_t addr)
{
    if (!g_initialized) return -1;
    if (addr >= QSPI_FLASH_SIZE || (addr & (QSPI_BLOCK_SIZE - 1u))) return -1;
    if (qspi_write_enable()) return -1;
    int rc = qspi_cmd_ind_write(QSPI_CMD_BLOCK_ER, addr, 1, NULL, 0);
    if (rc) return rc;
    return qspi_hal_wait_ready();   /* flash-internal erase, up to ~2 s */
}

int qspi_hal_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (!g_initialized) return -1;
    if (addr >= QSPI_FLASH_SIZE || !data || len == 0 || len > QSPI_PAGE_SIZE)
        return -1;
    if ((addr & (QSPI_PAGE_SIZE - 1u)) + len > QSPI_PAGE_SIZE)
        return -1;   /* page boundary crossing — caller must split */
    if (qspi_write_enable()) return -1;
    int rc = qspi_cmd_ind_write(QSPI_CMD_PAGE_PROG, addr, 1, data, len);
    if (rc) return rc;
    return qspi_hal_wait_ready();
}

int qspi_hal_read(uint32_t addr, uint8_t *data, uint32_t len)
{
    if (!g_initialized) return -1;
    if (addr >= QSPI_FLASH_SIZE || !data || len == 0) return -1;
    if (addr + len > QSPI_FLASH_SIZE) return -1;
    return qspi_cmd_ind_read(QSPI_CMD_READ, addr, 1, data, len);
}

#endif /* !JOC_RENODE */
