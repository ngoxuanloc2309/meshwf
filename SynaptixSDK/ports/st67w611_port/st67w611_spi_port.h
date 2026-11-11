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

/* Module presence probe on the RDY line (call BEFORE st67w611_init(), task context,
 * scheduler running). The module drives RDY, so its level does not follow the
 * MCU's internal pull: the pin is read with a pull-down and then a pull-up, and the
 * module counts as present only when both reads agree on every round. A floating
 * pin (module not fitted / not powered) follows the pull and reports absent; it is
 * then left with the pull-down, so it cannot pick up noise. Returns true when
 * something drives RDY. A board with an external pull on RDY always reports
 * present (the old behaviour). A module still booting can read absent: re-probe. */
bool meshwf_st67_module_present(void);

/* Counters for bring-up diagnosis (task context, no ISR use needed). Ticks are
 * FreeRTOS ticks; 0 = the event has not happened yet. */
typedef struct {
    uint32_t rdy_rising;
    uint32_t rdy_falling;
    uint32_t transfers;
    uint32_t timeouts;
    uint32_t errors;
    uint32_t en_release_tick;   /* deferred EN release */
    uint32_t first_rise_tick;   /* first RDY rising edge after that release */
    uint32_t gate_open_tick;    /* first-transfer gate opened (0 = still closed) */
    uint32_t gate_wait_ms;      /* how long the engine task waited at the gate */
    /* Vendor SPI engine counters (valid only when vendor_ok != 0, i.e. after the
     * engine exists). rx_drop > 0 with rx_pkts == 0 means a message was read and
     * thrown away (e.g. "ready" received before the RX queue was bound). */
    uint8_t  vendor_ok;
    uint32_t v_tx_pkts, v_rx_pkts, v_rx_drop, v_io_err, v_hdr_err;
    uint32_t v_wait_txn_to, v_wait_ack_to, v_mem_err;
} meshwf_st67_diag_t;
void meshwf_st67_port_get_diag(meshwf_st67_diag_t *d);

#ifdef __cplusplus
}
#endif

#endif /* ST67W611_SPI_PORT_H */