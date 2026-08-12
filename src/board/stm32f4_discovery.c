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
     * 板子"知道"的真实核心时钟就是 168M，直接取显式值，不随全局改动而失准。 */
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
    /* 控制台 TX 走 DMA(DMA2_Stream7 CH4),RX 走 DMA(DMA2_Stream5)。
     * 注意：USART1_TX 与 USART6_TX 在 F4 上共享 DMA2_Stream7(仅 channel 不同),
     * 但 DMA 流物理独享 —— 故 uart3(USART6) 放弃 TX DMA(改 IRQ TX),把
     * DMA2_Stream7 让给本 uart0 控制台。Rust App 经 dev_write(uart0) 走
     * uart_dma_write,必须依赖此 TX DMA 通道,否则 App 日志会全部静默丢弃。
     * uart0 RX 用 DMA2_Stream5,无冲突。 */
    .dma_tx_req = DMA_REQ_USART1_TX,      /* TX -> DMA2_Stream7 CH4 */
    .dma_rx_req = DMA_REQ_USART1_RX,      /* RX -> DMA2_Stream5 CH4 */
    .engine     = STREAM_MODE_DMA,        /* RX engine: circular DMA */
    .framing    = UART_FRAME_IDLE,        /* framing: IDLE marks frame end (zero per-byte ISR) */
};
/* uart1: GPS 串口（飞控 Rust 应用层经此收 NMEA/UBX）。USART2@PA2/PA3，非控制台。
 * RTOS 总线/外设提供到 uart 这一层，具体 GPS 协议解析由 Rust 应用层完成。 */
static const uart_config_t g_uart1 = {
    .name       = "uart1",
    .periph     = (void *)USART2,
    .baud       = 57600,                  /* 常见 UBLOX GPS 默认波特 */
    .is_console = 0,
    .tx_signal  = "USART2_TX_PA2",        /* TX = PA2, AF7 */
    .rx_signal  = "USART2_RX_PA3",        /* RX = PA3, AF7 */
    .dma_tx_req = DMA_REQ_USART2_TX,      /* TX -> DMA1_Stream6 CH4 */
    .dma_rx_req = DMA_REQ_USART2_RX,      /* RX -> DMA1_Stream5 CH4 */
    .engine     = STREAM_MODE_DMA,        /* RX engine: circular DMA */
    .framing    = UART_FRAME_IDLE,        /* framing: IDLE marks frame end */
};
/* uart2: USART3 @ PD8(TX)/PD9(RX)，AF7，非控制台。TX->DMA1_Stream3 CH4，
 * RX->DMA1_Stream1 CH4。PD8/PD9 在 Discovery 上空闲，未与其它驱动冲突。 */
static const uart_config_t g_uart2 = {
    .name       = "uart2",
    .periph     = (void *)USART3,
    .baud       = 115200,
    .is_console = 0,
    .tx_signal  = "USART3_TX_PD8",        /* TX = PD8, AF7 */
    .rx_signal  = "USART3_RX_PD9",        /* RX = PD9, AF7 */
    .dma_tx_req = DMA_REQ_USART3_TX,      /* TX -> DMA1_Stream3 CH4 */
    .dma_rx_req = DMA_REQ_USART3_RX,      /* RX -> DMA1_Stream1 CH4 */
    .engine     = STREAM_MODE_DMA,
    .framing    = UART_FRAME_IDLE,
};
/* uart3: USART6 @ PC6(TX)/PC7(RX)，AF8，非控制台（飞控遥测下行）。
 * TX 放弃 DMA2_Stream7 CH5 —— 该流已被 uart0(USART1_TX CH4) 占用(F4 上
 * USART1/USART6 的 TX 硬件都绑在 DMA2_Stream7,物理独享),故 uart3 TX 改走
 * IRQ( uart_tx_blocking )。RX 也随 engine=IRQ 走每字节中断(RX 原 DMA2_Stream2
 * 不再申请,无冲突)。pwm1 已改用 TIM1_CH2_PA9,不再占用 PC6,与本 uart3 无冲突。 */
static const uart_config_t g_uart3 = {
    .name       = "uart3",
    .periph     = (void *)USART6,
    .baud       = 115200,
    .is_console = 0,
    .tx_signal  = "USART6_TX",            /* TX = PC6, AF8 (USART6 has a single TX pad, no _PC6 suffix) */
    .rx_signal  = "USART6_RX",            /* RX = PC7, AF8 */
    .dma_tx_req = DMA_REQ_NONE,           /* TX -> IRQ (DMA2_Stream7 让给 uart0) */
    .dma_rx_req = DMA_REQ_NONE,           /* RX -> IRQ (每字节中断) */
    .engine     = STREAM_MODE_IRQ,
    .framing    = UART_FRAME_NONE,        /* TX/RX 均 IRQ,无需 IDLE 帧判定 */
};
static const gpio_config_t g_led   = { "led",   "GPIOD_14", 1 }; /* D14 (Discovery LD5), output — PD12 让给 pwm3(TIM4_CH1) */
/* 通用 GPIO 引脚，对外暴露给 Rust 应用层做任意数字 IO（经 device vtable 的
 * open/read/write/ioctl）。均为 Discovery 上空闲脚，避免与已占用脚冲突：
 *   PB0  -> 输出，默认 0
 *   PC0  -> 输入（无上拉下拉，由调用方经 ioctl 设置）
 *   PD13 -> 输出，默认 1
 *   PE3  -> 输入（exti 用 PE1/5/6，PE3 空闲）
 */
static const gpio_config_t g_gpio_pb0  = { "gpiob0",  "GPIOB_0",  0 }; /* B0,  output */
static const gpio_config_t g_gpio_pc0  = { "gpioc0",  "GPIOC_0",  0 }; /* C0,  input  */
static const gpio_config_t g_gpio_pd13 = { "gpiod13", "GPIOD_13", 1 }; /* D13, output */
static const gpio_config_t g_gpio_pe3  = { "gpioe3",  "GPIOE_3",  0 }; /* E3,  input  */
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
/* PWM (飞控 4 路 ESC): pwm0..3 均为 INDEPENDENT 400 Hz 模式（freq_hz=400），
 * 由本驱动自管 PSC/ARR 并启动计数器，不依赖任何 COORDINATE timer。ESC 要求
 * ~400 Hz 更新率，不能用 20 Hz 的 COORDINATE 模式。 */
/* pwm0: CH1 of TIM3 (GP TIM, 84 MHz APB1). Output pin PA6 (AF2). */
static const pwm_config_t g_pwm0 = { "pwm0", (void *)TIM3, 84000000, 400, 1,
                                      "TIM3_CH1_PA6", NULL, 0, 0, 0, 0 };
/* pwm1: CH1 of TIM2 (GP TIM, 84 MHz APB1), 400 Hz INDEPENDENT.
 * Output pin PA15 (AF1)。PA15 在 Discovery 上空闲（spi0 未配 NSS 脚，无其他
 * 驱动占用），与 pwm0(TIM3)/pwm2(TIM1)/pwm3(TIM4) 分属四个独立 TIM，无同 TIM
 * 频率耦合、无引脚冲突。注意：原方案 TIM1_CH2_PA9 与 uart0 控制台(USART1_TX_PA9)
 * 冲突被 pinmux 拒绝，故改到 PA15。 */
static const pwm_config_t g_pwm1 = { "pwm1", (void *)TIM2, 84000000, 400, 1,
                                      "TIM2_CH1_PA15", NULL, 0, 0, 0, 0 };
/* pwm2: CH1 of TIM1 (ADVANCED TIM, 168 MHz APB2), 400 Hz INDEPENDENT.
 * Output pin PA8 (AF1)。PA8 亦为 i2c2 SCL，二者不会同时 open。 */
static const pwm_config_t g_pwm2 = { "pwm2", (void *)TIM1, 168000000, 400, 1,
                                      "TIM1_CH1_PA8", NULL, 0, 0, 0, 0 };
/* pwm3: CH1 of TIM4 (GP TIM, 84 MHz APB1), 400 Hz INDEPENDENT.
 * Output pin PD12 (AF2)。注意避开 PB6（i2c0 SCL）与 PB7。 */
static const pwm_config_t g_pwm3 = { "pwm3", (void *)TIM4, 84000000, 400, 1,
                                      "TIM4_CH1_PD12", NULL, 0, 0, 0, 0 };
/* pwm4: CH1 of TIM12 (GP TIM, 无 CH1N/死区), COORDINATING 与 timer7 (owns TIM12's
 * 20 Hz period)。Output pin PB14 (AF9)。PB14 在 Discovery 上空闲（i2s0 用 PB15）。 */
static const pwm_config_t g_pwm4 = { "pwm4", (void *)TIM12, 84000000, 0, 1,
                                      "TIM12_CH1_PB14", NULL, 0, 1, 0, 0 };
/* External interrupt demo. exti0 (PE5) and exti1 (PE6) SHARE EXTI9_5 (IRQ23) —
 * same port E, different pin fields in SYSCFG EXTICR, so no conflict — which
 * exercises the multi-handler irq framework's sibling-guard on a shared line.
 * exti2 (PE1) uses a DEDICATED line (EXTI1, IRQ7). The self-test software-
 * triggers each to prove the ISR fires and siblings don't cross-trigger. */
static const exti_config_t g_exti0 = { "exti0", "GPIOE_5", EXTI_EDGE_RISING, 2 };
static const exti_config_t g_exti1 = { "exti1", "GPIOE_6", EXTI_EDGE_RISING, 2 };
static const exti_config_t g_exti2 = { "exti2", "GPIOE_1", EXTI_EDGE_RISING, 2 }; /* 注意：须避开 line0(PA0 USER 按钮=btn)，故放 line1 */
/* 按键示例设备：供 src/task/task_button.c 演示"上半部 ISR -> 下半部 BH 任务"。
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
/* I2C master demo: i2c1 is I2C2 on PB10(SCL)/PB11(SDA), 100 kHz. PB10/11 在
 * Discovery 上空闲（USART3 的备用脚在 PD8/9，故此处无冲突）。PCLK1 = 42 MHz。 */
static const i2c_config_t g_i2c1 = { "i2c1", (void *)I2C2, 42000000, 100000,
                                     "I2C2_SCL_PB10", "I2C2_SDA_PB11",
                                     DMA_REQ_I2C2_TX, DMA_REQ_I2C2_RX };
/* I2C master demo: i2c2 is I2C3 on PA8(SCL)/PC9(SDA), 100 kHz. PA8/PC9 在
 * Discovery 上空闲（PA8 亦为 pwm2 的 TIM1_CH1 脚，二者不会同时 open）。PCLK1 = 42 MHz。 */
static const i2c_config_t g_i2c2 = { "i2c2", (void *)I2C3, 42000000, 100000,
                                     "I2C3_SCL_PA8", "I2C3_SDA_PC9",
                                     DMA_REQ_I2C3_TX, DMA_REQ_I2C3_RX };
/* SPI master demo: spi0 is SPI1 on PA5(SCK)/PA6(MISO)/PA7(MOSI), ~1 MHz SCK. */
static const spi_config_t g_spi0 = { "spi0", (void *)SPI1, 84000000, 1000000,
                                     "SPI1_SCK_PA5", "SPI1_MISO_PA6",
                                     "SPI1_MOSI_PA7",
                                     DMA_REQ_SPI1_TX,      /* TX -> DMA2_Stream3 CH3 */
                                     DMA_REQ_SPI1_RX };    /* RX -> DMA2_Stream2 CH3 */
/* SPI master demo: spi1 is SPI2 on PI1(SCK)/PI2(MISO)/PI3(MOSI), ~1 MHz SCK.
 * Discovery 上 SPI2 的 PB13/PB15 已被 i2s0 占用，故 spi1 用 SPI2 的备用脚
 * PI1/PI2/PI3（AF5），与 i2s0 互不冲突。TX->DMA1_Stream4 CH0，RX->DMA1_Stream3 CH0。 */
static const spi_config_t g_spi1 = { "spi1", (void *)SPI2, 42000000, 1000000,
                                     "SPI2_SCK_PI1", "SPI2_MISO_PI2",
                                     "SPI2_MOSI_PI3",
                                     DMA_REQ_SPI2_TX,      /* TX -> DMA1_Stream4 CH0 */
                                     DMA_REQ_SPI2_RX };    /* RX -> DMA1_Stream3 CH0 */
/* SPI master demo: spi2 is SPI3 on PB3(SCK)/PB4(MISO)/PB5(MOSI), ~1 MHz SCK.
 * PB3/4/5 在 Discovery 上空闲（JTAG 默认已禁用，仅 SWD 用 PA13/14），AF6。
 * TX->DMA1_Stream5 CH0，RX->DMA1_Stream2 CH0。 */
static const spi_config_t g_spi2 = { "spi2", (void *)SPI3, 42000000, 1000000,
                                     "SPI3_SCK_PB3", "SPI3_MISO_PB4",
                                     "SPI3_MOSI_PB5",
                                     DMA_REQ_SPI3_TX,      /* TX -> DMA1_Stream5 CH0 */
                                     DMA_REQ_SPI3_RX };    /* RX -> DMA1_Stream2 CH0 */
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
/* FLASH: flash0 manages SECTOR 11 (0x080E0000, 128 KB) — a SPARE sector that
 * sits well above the ~57 KB firmware image (sectors 0-3) AND above the
 * APP_FLASH app partition (sectors 7/8/9 @0x08060000), so the BIST can safely
 * erase/program it without any risk of corrupting the running code or the app.
 * (Previously sector 7, but that now hosts the stage-2 app partition.) */
static const flash_config_t g_flash0 = { "flash0", 11 };
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
    0   /* dma_enable: OTG FS built-in DMA is DISABLED (slave/FIFO mode).
         * STM32F407 erratum ES0206 "USB OTG FS DMA transfer corruption":
         * the OTG FS internal DMA corrupts SRAM reads/writes whenever
         * HPRE (AHB) != PPRE1 (APB1). Our clock config (HCLK=168MHz/HPRE=1,
         * PCLK1=42MHz/PPRE1=4) hits this condition exactly, so DMA is
         * unusable on this silicon. The proven slave/FIFO mode is required. */
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
    { uart_create,        &g_uart1 },
    { uart_create,        &g_uart2 },
    { uart_create,        &g_uart3 },
    { gpio_pin_create,    &g_led },
    { gpio_pin_create,    &g_gpio_pb0 },
    { gpio_pin_create,    &g_gpio_pc0 },
    { gpio_pin_create,    &g_gpio_pd13 },
    { gpio_pin_create,    &g_gpio_pe3 },
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
    { pwm_create,         &g_pwm2 },
    { pwm_create,         &g_pwm3 },
    { pwm_create,         &g_pwm4 },
    { exti_create,        &g_exti0 },
    { exti_create,        &g_exti1 },
    { exti_create,        &g_exti2 },
    { exti_create,        &g_btn },
    { exti_create,        &g_btn2 },
    { i2c_create,         &g_i2c0 },
    { i2c_create,         &g_i2c1 },
    { i2c_create,         &g_i2c2 },
    { spi_create,         &g_spi0 },
    { spi_create,         &g_spi1 },
    { spi_create,         &g_spi2 },
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
