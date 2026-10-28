#include "st67w611_backend.h"

/* T02 host lwIP netif setup is owned by the network adapter. */
st67w611_status_t st67w611_backend_start(void)
{
    return ST67W611_OK;
}

st67w611_status_t st67w611_backend_stop(void)
{
    return ST67W611_OK;
}

st67w611_status_t st67w611_backend_get_ip(uint8_t ip[4], uint8_t gateway[4],
                                          uint8_t netmask[4])
{
    (void)ip; (void)gateway; (void)netmask;
    return ST67W611_ERR_UNSUPPORTED;
}

st67w611_status_t st67w611_backend_ping(const char *host, uint32_t timeout_ms)
{
    (void)host; (void)timeout_ms;
    return ST67W611_ERR_UNSUPPORTED;
}
