#ifndef ST67W611_LWIP_INTERNAL_H
#define ST67W611_LWIP_INTERNAL_H

#include "st67w611_lwip.h"

typedef struct {
    uint64_t rx_packets;
    uint64_t rx_drops;
    uint64_t io_errors;
    uint64_t header_errors;
    uint64_t rx_stalls;
} st67w611_lwip_transport_diagnostics_t;

/* Internal packet bridge used by the consumer's netif worker. */
int32_t st67w611_lwip_input(void **buffer, uint8_t **payload);
int32_t st67w611_lwip_free(void *buffer);
int32_t st67w611_lwip_flush_rx(void);
int32_t st67w611_lwip_output(const uint8_t *data, uint32_t length);

st67w611_status_t st67w611_lwip_get_ip_capability(void *context,
                                                  uint8_t ip[4],
                                                  uint8_t gateway[4],
                                                  uint8_t netmask[4]);
st67w611_status_t st67w611_lwip_ping_capability(void *context,
                                                const char *host,
                                                uint32_t timeout_ms);

int st67w611_lwip_get_transport_diagnostics(
    st67w611_lwip_transport_diagnostics_t *diagnostics);
void st67w611_lwip_dump_transport_queues(void);

#endif
