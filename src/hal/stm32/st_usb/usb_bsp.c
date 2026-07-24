/**
 * usb_bsp.c — ST USB OTG FS board support, implemented on our framework.
 *
 * USB_OTG_BSP_Init enables the OTG FS clock and configures PA11 (DM) / PA12
 * (DP) as alternate-function 10 (OTG FS). The pinmux layer in usb_dev_open
 * also reserves these signals, so this is the physical pin configuration that
 * makes the ST silicon layer work without depending on the pinmux order.
 *
 * USB_OTG_BSP_EnableInterrupt is intentionally a no-op: the OTG FS interrupt
 * is wired through our irq_manager in usb_dev_open so the framework owns the
 * vector consistently with every other driver.
 */
#include "usb_bsp.h"
#include "stm32f4xx.h"

void USB_OTG_BSP_Init(USB_OTG_CORE_HANDLE *pdev)
{
    (void)pdev;

    /* Enable OTG FS and GPIOA clocks. */
    RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    for (volatile int i = 0; i < 32; i++) ;   /* let the clock settle */

    /* PA11 = DM, PA12 = DP, AF10, push-pull, high speed, no pull. */
    GPIOA->AFR[1] &= ~((0xFUL << 12) | (0xFUL << 16)); /* clear AF for pin 11,12 */
    GPIOA->AFR[1] |=  ((0xAUL << 12) | (0xAUL << 16)); /* AF10 */

    GPIOA->MODER  &= ~((3UL << 22) | (3UL << 24));
    GPIOA->MODER  |=  ((2UL << 22) | (2UL << 24));   /* alternate function */

    GPIOA->OTYPER &= ~((1UL << 11) | (1UL << 12));   /* push-pull */

    GPIOA->OSPEEDR|=  ((3UL << 22) | (3UL << 24));   /* very high speed */

    GPIOA->PUPDR  &= ~((3UL << 22) | (3UL << 24));   /* no pull */
}

void USB_OTG_BSP_EnableInterrupt(USB_OTG_CORE_HANDLE *pdev)
{
    (void)pdev;   /* IRQ wired via irq_manager in usb_dev_open */
}

void USB_OTG_BSP_TimerIRQ(void) { }

void USB_OTG_BSP_uDelay(const uint32_t usec)
{
    /* Rough busy delay; ~168 MHz core, calibrated loosely. */
    volatile uint32_t n = usec * 120UL;
    while (n--) ;
}

void USB_OTG_BSP_mDelay(const uint32_t msec)
{
    volatile uint32_t n = msec * 120000UL;
    while (n--) ;
}
