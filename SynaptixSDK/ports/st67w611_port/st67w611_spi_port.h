#ifndef ST67W611_SPI_PORT_H
#define ST67W611_SPI_PORT_H

/*
 * st67w611_spi_port.h - SPI / pin port of the ST67W611 driver for the meshwf
 * board (STM32H523, SPI1). Implements st67w611_port_ops_t of libs/st67w611.
 *
 * Usage:
 *   st67w611_config_t cfg = { .port_ops = &meshwf_st67_port_ops, ... };
 *   st67w611_init(&cfg);              // the driver calls ops->init() itself
 *
 * MX_GPIO_Init() and MX_SPI1_Init() must have run (CubeMX does that before the
 * scheduler starts). The ops run in the vendor transfer engine's task context;
 * only the RDY edge handlers run in an ISR.
 */

#include <stdbool.h>
#include <stdint.h>
#include "st67w611.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const st67w611_port_ops_t meshwf_st67_port_ops;

/* Use only when ST67_DEFINE_EXTI_CALLBACKS = 0 (you own HAL_GPIO_EXTI_*_Callback):
 * call it with the pin from your own callback. Ignores pins other than RDY.
 * ISR-safe. */
void meshwf_st67_port_on_rdy_edge(uint16_t gpio_pin, bool rising);

/* Pin snapshot for bring-up (1 = high). en/boot are -1 when the board has no
 * such pin. Safe to call from any task. */
typedef struct {
    int8_t cs;
    int8_t rdy;
    int8_t wakeup;
    int8_t en;
    int8_t boot;
} meshwf_st67_pins_t;
void meshwf_st67_port_get_pins(meshwf_st67_pins_t *pins);

#ifdef __cplusplus
}
#endif

#endif /* ST67W611_SPI_PORT_H */