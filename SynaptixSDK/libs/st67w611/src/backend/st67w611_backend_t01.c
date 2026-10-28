#include "st67w611_backend.h"
#include <string.h>
#include "w6x_api.h"

st67w611_status_t st67w611_backend_start(void)
{
    return W6X_Net_Init() == W6X_STATUS_OK ? ST67W611_OK : ST67W611_ERR_VENDOR;
}

st67w611_status_t st67w611_backend_stop(void)
{
    return ST67W611_OK;
}

st67w611_status_t st67w611_backend_get_ip(uint8_t ip[4], uint8_t gateway[4],
                                          uint8_t netmask[4])
{
    return W6X_Net_Station_GetIPAddress(ip, gateway, netmask) == W6X_STATUS_OK
        ? ST67W611_OK : ST67W611_ERR_VENDOR;
}

st67w611_status_t st67w611_backend_ping(const char *host, uint32_t timeout_ms)
{
    uint32_t average = 0U;
    uint16_t received = 0U;
    size_t length;
    if (host == NULL || *host == '\0' || timeout_ms > UINT16_MAX) {
        return ST67W611_ERR_INVALID_ARG;
    }
    length = strlen(host);
    if (length > UINT16_MAX) return ST67W611_ERR_INVALID_ARG;
    return W6X_Net_Ping((uint8_t *)host, (uint16_t)length, 4U, 1000U,
                        (uint16_t)timeout_ms, &average, &received) == W6X_STATUS_OK
        ? ST67W611_OK : ST67W611_ERR_VENDOR;
}