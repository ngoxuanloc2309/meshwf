#ifndef ST67W611_VENDOR_BRIDGE_H
#define ST67W611_VENDOR_BRIDGE_H

#include <stdint.h>
#include "st67w611.h"

void st67w611_vendor_bridge_configure(const st67w611_port_ops_t *ops,
                                      void *context);
void st67w611_vendor_bridge_ready_rising(void);
void st67w611_vendor_bridge_ready_falling(void);

/* Vendor SPI engine counters (for bring-up diagnosis, task context). */
typedef struct {
    uint32_t tx_pkts;
    uint32_t rx_pkts;
    uint32_t rx_drop;            /* messages discarded (e.g. no RX queue bound yet) */
    uint32_t io_err;
    uint32_t hdr_err;
    uint32_t wait_txn_timeouts;
    uint32_t wait_hdr_ack_timeouts;
    uint32_t mem_err;
} st67w611_vendor_spi_stats_t;
int st67w611_vendor_bridge_get_spi_stats(st67w611_vendor_spi_stats_t *out);

#endif