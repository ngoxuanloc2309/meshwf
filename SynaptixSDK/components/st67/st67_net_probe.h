/**
 * @file st67_net_probe.h
 * @brief TEMPORARY test: does the ST67 NCP firmware accept a TCP server (AT+CIPSERVER)?
 *
 * Once the module has an IP, opens a TCP listener on ST67_NET_PROBE_PORT through the vendor
 * socket API (W6X_Net_Socket/Bind/Listen). It never calls accept/recv/send, so it never
 * blocks. Each step is logged with log_info. Remove it when the Modbus TCP slave exists.
 *
 * Called from the ST67 task only (the same task that runs the Wi-Fi worker and MQTT).
 */
#ifndef ST67_NET_PROBE_H
#define ST67_NET_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#ifndef ST67_NET_PROBE
#define ST67_NET_PROBE        0      /* 0 = compiles to nothing (OFF: Modbus TCP slave owns port 5020) */
#endif
#ifndef ST67_NET_PROBE_PORT
#define ST67_NET_PROBE_PORT   5020U
#endif

#if ST67_NET_PROBE
/* ST67 task, every loop pass. Cheap when there is nothing to do. */
void st67_net_probe_service(uint32_t now_ms, bool has_ip);
#else
static inline void st67_net_probe_service(uint32_t now_ms, bool has_ip)
{
    (void)now_ms;
    (void)has_ip;
}
#endif

#endif /* ST67_NET_PROBE_H */