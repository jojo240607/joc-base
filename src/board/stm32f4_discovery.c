/*
 * BOARD: STM32F4 Discovery (STM32F407VGT6)
 *
 * This is the ONLY file (besides the HAL) that knows the real silicon:
 * ADC1, USART1, GPIOD and the factory temperature calibration words. The
 * hardware assignment is expressed as DATA: each device is an instance of its
 * driver's OWN config struct, filled here. board_init() walks the node array,
 * calls each node's create fn (which takes ONLY the config pointer) and
 * registers the device by the name the driver set. There is NO switch and NO
 * per-driver probe / build wrapper in this file — the config + its create fn
 * IS the dispatch.
 *
 * To port to another board you write a new src/board/<board>.c with a different
 * config array + the matching HAL; main.c and the drv/ sources are untouched.
 */
#include "board.h"
#include "devmgr/device_manager.h"

#include "stm32f4xx.h"          /* real peripherals — board layer only */
#include "irq.h"                /* platform-independent interrupt API */

#include "drv/clock.h"
#include "clock_hal.h"   /* clock_hal_configure()：board_tick_init 需先把时钟带到 168M */

#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"
#include "drv/pinmux.h"
#include "drv/timer.h"
#include "drv/pwm.h"
#include "drv/exti.h"
#include "drv/i2c.h"
#include "drv/spi.h"
#include "drv/sdio.h"
#include "drv/sd_card.h"
#include "drv/dac.h"
#include "drv/rtc.h"
#include "drv/rng.h"
#include "drv/crc.h"
#include "drv/iwdg.h"
#include "drv/wwdg.h"
#include "drv/flash.h"
#include "drv/i2s.h"
#include "drv/usb.h"
#include "drv/dma.h"
#include "drv/can.h"

#include "temp_hal.h"

/* ---- board-level SysTick tick service, built as an EVENT device ------------
 * SysTick is modeled as an `event_device` (see drv/systick.c): the driver
 * configures the core timer and registers its ISR through the GENERIC irq
 * framework; the board subscribes a tick callback. This proves the four-class
 * model + the irq framework work together. main.c still calls board_tick_init()
 * / board_ticks(), so its TICKS command is unchanged. */
#include "drv/systick.h"

static volatile uint32_t g_board_ticks;

static void board_tick_cb(void *ctx, device_event_type_t ev, void *data)
{
    (void)ctx; (void)ev; (void)data;
    g_board_ticks++;
}

void board_tick_init(void)
{
    /* 先把核心时钟带到 PLL@168MHz，使 SystemCoreClock 此刻即为真实值、CPU
     * 也确实跑在 168 MHz 上。否则 system_early_init 在 board_tick_init 之后才
     * 由 app_main_task 打开 clk 设备（clock_hal_configure），这里读到的
     * SystemCoreClock 仍是 .data 初值 16M，而 CPU 实际还跑在 8M(HSE)，导致
     * SysTick 重载用错基准、节拍率严重失准（RTOS 的 delay/timeout 全错）。
     * clock_hal_configure() 幂等，app_main_task 再次 open 无副作用。 */
    clock_hal_configure();

    systick_config_t c;
    c.name    = "systick";
    /* 用 HAL 提供的设计常量（= CLOCK_SYSCLK_HZ = 168 MHz），不再依赖可变全局
     * SystemCoreClock（其 .data 初值为 16M，需被 clock_hal_configure 改写才对）。
     * 板子“知道”的真实核心时钟就是 168M，直接取显式值，不随全局改动而失准。 */
    c.cpu_hz  = clock_hal_sysclk_hz();
    c.tick_hz = 1000;              /* 1 ms tick */
    device *d = systick_create(&c);
    if (d)
        device_manager_register("systick", d);
    event_device *e = device_as_event(d);
    if (e)
        e->vtable->set_event_callback(e, DEVICE_EVENT_TICK, board_tick_cb, NULL);
}

uint32_t board_ticks(void) { return g_board_ticks; }

/* ---- board devices as DATA (each driver's own config, filled by the board) */
static const pinmux_config_t g_pinmux = { "pinmux" };
static const adc_config_t  g_adc0  = { "adc0",  (void *)ADC1, 0, 3300,
                                       "ADC1_IN0",            /* PA0, af=0 */
                                       DMA_REQ_ADC1 };        /* ADC1 -> DMA2_Stream0 CH0 */
static const uart_config_t g_uart0 = {
    .name       = "uart0",
    .periph     = (void *)USART1,
    .baud       = 115200,
    .is_console = 1,
    .tx_signal  = "USART1_TX_PA9",        /* TX = PA9, AF7 */
    .rx_signal  = "USART1_RX_PA10",       /* RX = PA10, AF7 */
    .dma_tx_req = DMA_REQ_USART1_TX,      /* TX -> DMA2_Stream7 CH4 */
    .dma_rx_req = DMA_REQ_USART1_RX,      /* RX -> DMA2_Stream5 CH4 */
    .engine     = STREAM_MODE_DMA,        /* RX engine: circular DMA */
    .framing    = UART_FRAME_IDLE,        /* framing: IDLE marks frame end (zero per-byte ISR) */
};
static const gpio_config_t g_led   = { "led",   "GPIOD_12", 1 }; /* D12, output */
static const clock_config_t g_clk  = { "clk" };
static const temp_config_t g_temp0 = { "temp0", "adc0", 3300 };   /* adc0 must precede temp0 */
static const timer_config_t g_timer0 = { "timer0", (void *)TIM2,  84000000, 20,
                                     DMA_REQ_TIM2_UP }; /* TIM2_UP -> DMA1_Stream7 CH3 */
static const timer_config_t g_timer1 = { "timer1", (void *)TIM1,  168000000, 20 }; /* TIM1,  APB2 168MHz, 20Hz, IRQ25(TIM1_UP) */
static const timer_config_t g_timer2 = { "timer2", (void *)TIM6,  84000000, 20 }; /* TIM6,  APB1 84MHz, 20Hz, IRQ54 */
static const timer_config_t g_timer3 = { "timer3", (void *)TIM7,  84000000, 20 }; /* TIM7,  APB1 84MHz, 20Hz, IRQ55 */
static const timer_config_t g_timer4 = { "timer4", (void *)TIM8,  168000000, 20 }; /* TIM8,  APB2 168MHz, 20Hz, IRQ44(TIM8_UP) */
static const timer_config_t g_timer5 = { "timer5", (void *)TIM9,  168000000, 20 }; /* TIM9,  APB2 168MHz, 20Hz, IRQ24 */
static const timer_config_t g_timer6 = { "timer6", (void *)TIM11, 168000000, 20 }; /* TIM11, APB2 168MHz, 20Hz, IRQ26 */
static const timer_config_t g_timer7 = { "timer7", (void *)TIM12, 84000000, 20 }; /* TIM12, APB1 84MHz, 20Hz, IRQ43 */
static const timer_config_t g_timer8 = { "timer8", (void *)TIM14, 84000000, 20 }; /* TIM14, APB1 84MHz, 20Hz, IRQ45 */
/* TIM10 and TIM13 deliberately SHARE an IRQ line with TIM1 and TIM8 to exercise
 * the multi-handler irq framework: TIM10 -> IRQ25 (same as TIM1_UP), TIM13 ->
 * IRQ44 (same as TIM8_UP). Both must fire on the shared line simultaneously. */
static const timer_config_t g_timer9  = { "timer9",  (void *)TIM10, 168000000, 20 }; /* TIM10, APB2 168MHz, 20Hz, IRQ25 (shares w/ TIM1) */
static const timer_config_t g_timer10 = { "timer10", (void *)TIM13, 84000000,  20 }; /* TIM13, APB1 84MHz, 20Hz, IRQ44 (shares w/ TIM8) */
/* TIM3/4/5 are general-purpose timers on APB1 (84MHz) with their OWN dedicated
 * IRQ lines (29/30/50), so they exercise the single-handler path (no sharing). */
static const timer_config_t g_timer11 = { "timer11", (void *)TIM3, 84000000, 20 }; /* TIM3,  APB1 84MHz, 20Hz, IRQ29 */
static const timer_config_t g_timer12 = { "timer12", (void *)TIM4, 84000000, 20 }; /* TIM4,  APB1 84MHz, 20Hz, IRQ30 */
static const timer_config_t g_timer13 = { "timer13", (void *)TIM5, 84000000, 20 }; /* TIM5,  APB1 84MHz, 20Hz, IRQ50 */
/* PWM demo: pwm0 is CH1 of TIM3, COORDINATING with timer11 (which owns TIM3's
 * period + counter as a 20 Hz TICK source). pwm0 only configures the channel
 * and duty on the SAME TIM3, so one peripheral emits BOTH a periodic event AND
 * a PWM waveform — this is the timer<->pwm "配合". Output pin PA6 (AF2). */
static const pwm_config_t g_pwm0 = { "pwm0", (void *)TIM3, 84000000, 0, 1,
                                      "TIM3_CH1_PA6", NULL, 0, 0, 0, 0 };
/* Advanced-timer PWM demo: pwm1 is CH1 of TIM8 (an ADVANCED TIM), COORDINATING
 * with timer4 (which owns TIM8's 20 Hz period + counter as a TICK source). TIM8
 * is advanced, so its PWM pins stay dead until BDTR.MOE=1 — the driver sets it.
 * pwm1 also exercises the advanced-only features: a COMPLEMENTARY output
 * (CH1N on PA7) and a 64-tick DEAD-TIME inserted between the two switches.
 * Output pins PC6 (CH1) + PA7 (CH1N), both AF3. */
static const pwm_config_t g_pwm1 = { "pwm1", (void *)TIM8, 168000000, 0, 1,
                                      "TIM8_CH1_PC6", "TIM8_CH1N", 64, 1, 0, 0 };
/* External interrupt demo. exti0 (PE5) and exti1 (PE6) SHARE EXTI9_5 (IRQ23) —
 * same port E, different pin fields in SYSCFG EXTICR, so no conflict — which
 * exercises the multi-handler irq framework's sibling-guard on a shared line.
 * exti2 (PE1) uses a DEDICATED line (EXTI1, IRQ7). The self-test software-
 * triggers each to prove the ISR fires and siblings don't cross-trigger. */
static const exti_config_t g_exti0 = { "exti0", "GPIOE_5", EXTI_EDGE_RISING, 2 };
static const exti_config_t g_exti1 = { "exti1", "GPIOE_6", EXTI_EDGE_RISING, 2 };
static const exti_config_t g_exti2 = { "exti2", "GPIOE_1", EXTI_EDGE_RISING, 2 }; /* 注意：须避开 line0(PA0 USER 按钮=btn)，故放 line1 */
/* 按键示例设备：供 src/task/task_button.c 演示“上半部 ISR -> 下半部 BH 任务”。
 * 注：板载真实 USER 按钮在 PA0，但 PA0 已被 adc0 的 ADC1_IN0 占用（pinmux 冲突），
 * 故此处改用一个空闲引脚 PA2（EXTI line2 / IRQ8，不与 exti0/1/2 的 line 冲突）。
 * 若要用真实 USER 按钮，需先把 adc0 通道 0 改到别的脚（见 README / companion_test）。
 * 用 BTN 命令软件触发边沿即可演示，无需物理按键。 */
static const exti_config_t g_btn  = { "btn",   "GPIOA_2", EXTI_EDGE_RISING, 2 };
/* 按键示例(工作队列版)设备：供 src/task/task_button_wq.c 演示"上半部 ISR ->
 * 下半部由共享 wq worker 执行"（不新建专属任务）。用 PA3（EXTI line3 / IRQ9），
 * 与 btn(PA2/line2)、exti0/1/2 的 line 都不冲突；PA3 未被任何驱动占用。 */
static const exti_config_t g_btn2 = { "btn2",  "GPIOA_3", EXTI_EDGE_RISING, 2 };
/* I2C master demo: i2c0 is I2C1 on PB6(SCL)/PB7(SDA), 100 kHz. There is no I2C
 * slave on the Discovery board, so this node exists to prove the driver + HAL
 * configure the CORRECT F4 I2C registers and that the polling state machine runs
 * (and bus-scans) without hanging. PCLK1 = 42 MHz (APB1). */
static const i2c_config_t g_i2c0 = { "i2c0", (void *)I2C1, 42000000, 100000,
                                     "I2C1_SCL_PB6", "I2C1_SDA_PB7",
                                     DMA_REQ_I2C1_TX, DMA_REQ_I2C1_RX };
/* SPI master demo: spi0 is SPI1 on PA5(SCK)/PA6(MISO)/PA7(MOSI), ~1 MHz SCK. */
static const spi_config_t g_spi0 = { "spi0", (void *)SPI1, 84000000, 1000000,
                                     "SPI1_SCK_PA5", "SPI1_MISO_PA6",
                                     "SPI1_MOSI_PA7",
                                     DMA_REQ_SPI1_TX,      /* TX -> DMA2_Stream3 CH3 */
                                     DMA_REQ_SPI1_RX };    /* RX -> DMA2_Stream2 CH3 */
/* SDIO host: sdio0 on PC8-PC12(4-bit) + PD2(CMD), AF12. */
static const sdio_config_t g_sdio0 = { "sdio0", (void *)SDIO,
                                     "SDIO_CK", "SDIO_CMD",
                                     "SDIO_D0", "SDIO_D1",
                                     "SDIO_D2", "SDIO_D3",
                                     DMA_REQ_SDIO };
/* SD Card: uses sdio0 bus device (no direct pin/HAL access). */
static const sd_card_config_t g_sd_card0 = { "sd_card0", "sdio0", 0 };
/* DAC: dac0 is DAC1 channel 1 on PA4 (DAC_OUT1), 12-bit analog output.
 * DAC+DMA requires a trigger source (static mode never raises a DMA request),
 * so TIM6 (the silicon's dedicated DAC trigger timer) clocks the burst. */
static const dac_config_t g_dac0 = { "dac0", (void *)DAC, 1, "DAC_OUT1",
                                     DMA_REQ_DAC1,              /* DAC1 -> DMA1_Stream5 CH7 */
                                     (void *)TIM6 };           /* TIM6 TRGO (DAC trigger) */
/* RTC: rtc0 is the on-chip RTC, clocked by the internal LSI oscillator. The RTC
 * lives in the backup domain and needs no external pins, so there is no pinmux
 * signal — only the peripheral base (RTC). */
static const rtc_config_t g_rtc0 = { "rtc0", (void *)RTC };
/* RNG: rng0 is the on-chip true random number generator. It needs no external
 * pins and no clock configuration beyond the AHB2 bus gate, so there is no
 * pinmux signal — only the peripheral base (RNG). */
static const rng_config_t g_rng0 = { "rng0", (void *)RNG };
/* CRC: crc0 is the on-chip CRC calculation unit. It needs no external pins and
 * no clock configuration beyond the AHB1 bus gate, so there is no pinmux
 * signal — only the peripheral base (CRC). */
static const crc_config_t g_crc0 = { "crc0", (void *)CRC };
/* IWDG: iwdg0 is the on-chip independent watchdog. It is clocked by the
 * internal LSI oscillator, needs no external pins and no clock gate beyond LSI,
 * so there is no pinmux signal — only the peripheral base (IWDG). */
static const iwdg_config_t g_iwdg0 = { "iwdg0", (void *)IWDG };
/* WWDG: wwdg0 is the on-chip window watchdog. It is clocked by PCLK1 (APB1) and
 * needs no external pins, so there is no pinmux signal — only the peripheral
 * base (WWDG). Unlike the IWDG it is NOT in the backup domain, so it does not
 * survive a reset. */
static const wwdg_config_t g_wwdg0 = { "wwdg0", (void *)WWDG };
/* FLASH: flash0 manages SECTOR 7 (0x08060000, 128 KB) — a SPARE sector that
 * sits well above the ~57 KB firmware image (sectors 0-3), so the BIST can
 * safely erase/program it without any risk of corrupting the running code. */
static const flash_config_t g_flash0 = { "flash0", 7 };
/* I2S: i2s0 is the I2S2 block (hosted inside SPI2, APB1), configured as a 48 kHz
 * master transmitter (Philips standard, 16-bit). The three signals map to the
 * SPI2 AF5 pads WS=PB12, CK=PB13, SD=PB15 — none of which the Discovery board
 * wires to the CS43L22 codec, so this node ONLY exercises the on-chip I2S logic
 * (PLLI2S clock + prescaler + data shift) with NO external codec attached. */
static const i2s_config_t g_i2s0 = {
    "i2s0", (void *)SPI2, 42000000, 48000,
    "SPI2_NSS_PB12",   /* WS  (LRCLK) */
    "SPI2_SCK_PB13",   /* CK  (bit clock) */
    "SPI2_MOSI_PB15",  /* SD  (data out, TX) */
    NULL,              /* extSD (RX) unused for a TX instance */
    1, 1, 0,           /* master, transmit, 16-bit */
    DMA_REQ_SPI2_TX    /* I2S2 TX -> DMA1_Stream4 CH3 */
};
/* CAN: can0 is the bxCAN controller (CAN1). The Discovery board wires PA11/PA12
 * to the USB-OTG connector, so the default CAN1_RX (PA11) is held DOMINANT by
 * board hardware and bxCAN can never count 11 recessive bits to leave init mode.
 * We therefore route CAN1 to its alternate free pins PB9(TX)/PB8(RX), AF9 — on
 * STM32F4 the CAN pins are selected purely by the GPIO AF matrix (no SYSCFG
 * remap exists); PB8/PB9 are free here (I2C1 is on PB6/PB7). The node runs in
 * LOOPBACK mode: the controller transmits its own frame internally and echoes it
 * back into the receive FIFO (proving TX + RX data paths) with zero external
 * hardware. SILENT must stay OFF — in silent mode bxCAN cannot START a
 * transmission, so there would be nothing to loop back. With the RX pin (PB8)
 * pulled up to a recessive idle the bus is idle-recessive, so the controller CAN
 * leave init mode (INAK clears). ~656 kbps (42 MHz / (4*(1+11+4))). */
static const can_config_t g_can0 = {
    "can0", (void *)CAN1, 42000000,
    4,                          /* prescaler */
    1, 11, 4,                   /* sjw, bs1, bs2 */
    "CAN1_TX_PB9", "CAN1_RX_PB8",
    1, 0,                       /* loopback=1, silent=0: loopback-only is the self-test
                                   mode — the controller echoes its own transmitted frame
                                   into FIFO0. (silent=1 would block all transmission, so
                                   the loopback would receive nothing.) */
    0x123,                      /* default TX id for plain stream writes */
    0                           /* remap flag unused on F4 (pins chosen via AF9) */
};

/* USB: usb0 is the OTG FS device controller, wired to the Discovery's CN5
 * USB-OTG micro-AB connector on PA11 (DM) / PA12 (DP), AF10. It presents a
 * CDC-ACM Virtual COM Port to a host PC — PA11/PA12 are exactly the pins we
 * steered CAN away from, because the Discovery hard-wires them to USB. The
 * driver ignores VBUS sensing (NOVBUSSENS) so the device "connects" even when
 * the board's VBUS detect is not populated. Full enumeration requires a host on
 * CN5; the BIST validates the core bring-up + control protocol without one. */
static const usb_config_t g_usb0 = {
    "usb0", (void *)USB_OTG_FS, "USB_OTG_FS_DM", "USB_OTG_FS_DP", 0,
    0   /* dma_enable: OTG FS built-in DMA is OPT-IN. The F4 OTG FS internal
         * DMA is unreliable for CDC enumeration on the embedded PHY (no COM9
         * appears), so the proven slave/FIFO mode is the default. Set 1 to use
         * the OTG's built-in DMA for the byte stream. */
};

/* DMA controllers: dma1 / dma2 each expose an 8-stream pool to other drivers
 * (UART/SPI/SDIO/ADC ... can acquire a stream via device_manager_get("dma1")).
 * The driver is CONTROL-class (resource manager), not a byte stream. */
static const dma_config_t g_dma1 = { "dma1", (void *)DMA1 };
static const dma_config_t g_dma2 = { "dma2", (void *)DMA2 };

/* the board is just a list of (create-fn, config) pairs — no type switch.
 * pinmux is listed FIRST so it is registered before any driver claims pins.
 * Each driver claims and configures its own pins through the pinmux at open()
 * time, using the SIGNAL NAMES supplied above. The pinmux resolves each name
 * to its exact (port, pin, af) — so a pin conflict is rejected before any GPIO
 * register is touched, instead of being logged here. The board never lists a
 * raw (port, pin, af) triple: the AF database is the single source of truth. */
static const board_node_t g_nodes[] = {
    { pinmux_create,      &g_pinmux },
    { clock_create,       &g_clk },
    { uart_create,        &g_uart0 },
    { gpio_pin_create,    &g_led },
    { adc_create,         &g_adc0 },
    { temp_sensor_create, &g_temp0 },
    { timer_create,       &g_timer0 },
    { timer_create,       &g_timer1 },
    { timer_create,       &g_timer2 },
    { timer_create,       &g_timer3 },
    { timer_create,       &g_timer4 },
    { timer_create,       &g_timer5 },
    { timer_create,       &g_timer6 },
    { timer_create,       &g_timer7 },
    { timer_create,       &g_timer8 },
    { timer_create,       &g_timer9 },
    { timer_create,       &g_timer10 },
    { timer_create,       &g_timer11 },
    { timer_create,       &g_timer12 },
    { timer_create,       &g_timer13 },
    { pwm_create,         &g_pwm0 },
    { pwm_create,         &g_pwm1 },
    { exti_create,        &g_exti0 },
    { exti_create,        &g_exti1 },
    { exti_create,        &g_exti2 },
    { exti_create,        &g_btn },
    { exti_create,        &g_btn2 },
    { i2c_create,         &g_i2c0 },
    { spi_create,         &g_spi0 },
    { sdio_create,        &g_sdio0 },
    { sd_card_create,     &g_sd_card0 },
    { dac_create,         &g_dac0 },
    { rtc_create,         &g_rtc0 },
    { rng_create,         &g_rng0 },
    { crc_create,         &g_crc0 },
    { iwdg_create,        &g_iwdg0 },
    { wwdg_create,        &g_wwdg0 },
    { flash_create,       &g_flash0 },
    { i2s_create,         &g_i2s0 },
    { can_create,         &g_can0 },
    { usb_create,         &g_usb0 },
    { dma_create,         &g_dma1 },
    { dma_create,         &g_dma2 },
};

/* generic dispatcher — forwards ONLY the config pointer, no switch */
static device *board_build(const board_node_t *n)
{
    return n->create(n->config);
}

void board_init(void)
{
    for (uint32_t i = 0; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
        device *dev = board_build(&g_nodes[i]);
        if (dev) device_manager_register(device_get_name(dev), dev);
    }
}
