#ifndef ST67W611_SPI_PORT_CONF_H
#define ST67W611_SPI_PORT_CONF_H

/*
 * st67w611_spi_port_conf.h - board wiring and tuning of the ST67W611 SPI port.
 *
 * Defaults match the meshwf board (STM32H523CCU6, RS485_IO_RF_V2.ioc):
 *   SPI1   SCK PA5, MISO PA6, MOSI PA7 (initialised by CubeMX, hspi1)
 *   CS     PA4  "ST67CS"        GPIO output, software NSS
 *   RDY    PB0  "ST67W_RDY"     EXTI0 (the port re-arms it, see below)
 *   WAKEUP PB2  "ST67W_Wakeup"  GPIO output
 *   EN     (none in the .ioc)   optional, see ST67_EN_*
 *   BOOT   (none in the .ioc)   optional, see ST67_BOOT_*
 *
 * Every option can be overridden by defining it BEFORE this header is
 * included (compiler -D, or a file named by ST67_PORT_USER_CONF). Nothing here
 * edits a CubeMX-generated file.
 */

#include "main.h"      /* ST67CS_*, ST67W_RDY_*, ST67W_Wakeup_* (CubeMX labels) */
#include "spi.h"       /* extern SPI_HandleTypeDef hspi1 */

#ifdef ST67_PORT_USER_CONF
#include ST67_PORT_USER_CONF
#endif

/* --- SPI ----------------------------------------------------------------- */
#ifndef ST67_SPI_HANDLE
#define ST67_SPI_HANDLE            (&hspi1)
#endif

/* Largest single transfer (must cover the Wi-Fi MTU frame). Two static buffers
 * of this size are kept (zero source for RX-only transfers, sink for TX-only). */
#ifndef ST67_SPI_MAX_XFER
#define ST67_SPI_MAX_XFER          2048U
#endif

/* 0 (default): transfer_dma() runs the transfer polled and reports completion
 *              at once (the vendor engine accepts completion from task or ISR
 *              context). Needs nothing beyond the CubeMX SPI1 setup.
 * 1          : real DMA. In CubeMX link two GPDMA channels to SPI1_TX/SPI1_RX
 *              (channels 0/1 are taken by USART2 in the current .ioc) and enable
 *              their interrupts; the port then uses HAL_SPI_TransmitReceive_DMA
 *              and completes from HAL_SPI_TxRxCpltCallback. */
#ifndef ST67_SPI_USE_DMA
#define ST67_SPI_USE_DMA           0
#endif

/* CubeMX leaves SPI1 "master keep IO state" OFF: SCK/MOSI then float between
 * transfers while CS is active. 1 = the port sets SPI_CFG2.AFCNTR (while the
 * SPI is disabled) so the peripheral keeps driving them. */
#ifndef ST67_KEEP_SPI_IO_STATE
#define ST67_KEEP_SPI_IO_STATE     1
#endif

/* --- CS ------------------------------------------------------------------ */
/* ST's reference port (and the running wg_ue gateway) drive the CS GPIO HIGH
 * to select the module. Set 0 only if your PCB inverts it. */
#ifndef ST67_CS_ACTIVE_HIGH
#define ST67_CS_ACTIVE_HIGH        1
#endif
#ifndef ST67_CS_MIN_GAP_US
#define ST67_CS_MIN_GAP_US         2U     /* min time CS stays inactive (ST reference: 2 us) */
#endif
#ifndef ST67_CS_SETUP_US
#define ST67_CS_SETUP_US           20U    /* wait after asserting CS (wg_ue uses ~20 us) */
#endif

/* --- Pins ---------------------------------------------------------------- */
#define ST67SPI_CS_PORT               ST67CS_GPIO_Port
#define ST67SPI_CS_PIN                ST67CS_Pin
#define ST67SPI_RDY_PORT              ST67W_RDY_GPIO_Port
#define ST67SPI_RDY_PIN               ST67W_RDY_Pin
#define ST67SPI_WAKEUP_PORT           ST67W_Wakeup_GPIO_Port
#define ST67SPI_WAKEUP_PIN            ST67W_Wakeup_Pin

/* EN / BOOT exist only if the pin has a CubeMX label ST67_EN / ST67_BOOT
 * (then main.h defines ST67_EN_GPIO_Port ...). Without EN the port cannot
 * reset the module: hardware_reset() fails with -1 and nothing else changes. */
#if defined(ST67_EN_GPIO_Port) && defined(ST67_EN_Pin)
#define ST67_HAS_EN                1
#define ST67SPI_EN_PORT               ST67_EN_GPIO_Port
#define ST67SPI_EN_PIN                ST67_EN_Pin
#else
#define ST67_HAS_EN                0
#endif
#if defined(ST67_BOOT_GPIO_Port) && defined(ST67_BOOT_Pin)
#define ST67_HAS_BOOT              1
#define ST67SPI_BOOT_PORT             ST67_BOOT_GPIO_Port
#define ST67SPI_BOOT_PIN              ST67_BOOT_Pin
#else
#define ST67_HAS_BOOT              0
#endif

#ifndef ST67_EN_LOW_MS
#define ST67_EN_LOW_MS             50U    /* EN held low during a reset */
#endif
#ifndef ST67_BOOT_WAIT_MS
#define ST67_BOOT_WAIT_MS          100U   /* wait after EN goes high */
#endif

/* --- RDY interrupt ------------------------------------------------------- */
/* The vendor engine needs BOTH edges of RDY (rising = data ready, falling =
 * header ack). CubeMX configured PB0 rising-only and generated no EXTI0 IRQ
 * handler, so by default the port re-arms the pin (rising+falling), sets the
 * NVIC priority and defines the handler itself. */
#ifndef ST67_RDY_RECONFIGURE
#define ST67_RDY_RECONFIGURE       1
#endif
#ifndef ST67_RDY_EXTI_IRQn
#define ST67_RDY_EXTI_IRQn         EXTI0_IRQn     /* PB0 -> EXTI line 0 */
#endif
#ifndef ST67_RDY_EXTI_IRQHandler
#define ST67_RDY_EXTI_IRQHandler   EXTI0_IRQHandler
#endif
/* Must be numerically >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (the
 * vendor ISR path calls FreeRTOS *FromISR functions). Checked at compile time. */
#ifndef ST67_RDY_IRQ_PRIORITY
#define ST67_RDY_IRQ_PRIORITY      5U
#endif
/* Set to 0 if CubeMX later generates EXTI0_IRQHandler (otherwise: duplicate
 * symbol at link time), or if another file owns the HAL EXTI callbacks; in the
 * second case call meshwf_st67_port_on_rdy_edge() from your callbacks. */
#ifndef ST67_DEFINE_RDY_IRQ_HANDLER
#define ST67_DEFINE_RDY_IRQ_HANDLER 1
#endif
#ifndef ST67_DEFINE_EXTI_CALLBACKS
#define ST67_DEFINE_EXTI_CALLBACKS 1
#endif

/* --- Logging (default: none) ---------------------------------------------- */
#ifndef ST67_LOG
#define ST67_LOG(...)              ((void)0)
#endif

#endif /* ST67W611_SPI_PORT_CONF_H */