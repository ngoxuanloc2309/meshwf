#ifndef ST67W611_BACKEND_H
#define ST67W611_BACKEND_H

#include "st67w611.h"

st67w611_status_t st67w611_backend_start(void);
st67w611_status_t st67w611_backend_stop(void);
st67w611_status_t st67w611_backend_get_ip(uint8_t ip[4], uint8_t gateway[4],
                                           uint8_t netmask[4]);
st67w611_status_t st67w611_backend_ping(const char *host, uint32_t timeout_ms);

#endif
