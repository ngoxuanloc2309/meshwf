/**
 * @file st67_modbus_tcp.h
 * @brief Modbus TCP slave transport for SimplePLC over the ST67W611 vendor sockets (T01, AT).
 *
 * Plugs into plc_modbus_cfg_add_transport() (needs SPLC_MODBUS_MAX_TRANSPORTS >= 2). It does
 * not modify simple_plc or the vendor code.
 *
 * Why two tasks: the vendor socket calls block. W6X_Net_Accept() never returns while nobody
 * connects, and W6X_Net_Recv() waits up to 100 ms when there is no data, so none of them may
 * run in the SPLC engine task that calls read()/write() of the transport.
 *
 *   MBTCP_ACC : creates the listener, blocks in W6X_Net_Accept(), hands new sockets to MBTCP_IO.
 *   MBTCP_IO  : polls every client socket, assembles complete MBAP frames, gives ONE frame at
 *               a time to the engine, waits for the answer and sends it to the SAME client.
 *   engine    : read()/write() only copy bytes to/from a RAM buffer and never block.
 *
 * Several clients may be connected; their requests are served one after the other (the
 * library serves one request per scan and shares all state, so this is the same as the USB
 * App link: one App configures the device at a time).
 *
 * Modbus TCP has no authentication: anyone on the LAN can write rules or reset the device.
 */
#ifndef ST67_MODBUS_TCP_H
#define ST67_MODBUS_TCP_H

#include <stdbool.h>
#include <stdint.h>

#include "modbus_transport.h"

#ifndef ST67_MBTCP_PORT
#define ST67_MBTCP_PORT              5020U
#endif
/* Simultaneous clients, 1..4. The vendor has 5 module connections and 6 socket slots; the
 * listener and the pending accept use two slots. An idle client costs up to 100 ms of extra
 * latency for the clients polled after it (W6X_Net_Recv waits 100 ms without data). */
#ifndef ST67_MBTCP_MAX_CLIENTS
#define ST67_MBTCP_MAX_CLIENTS       2U
#endif
/* A client that sends nothing for this long is closed (0 = never). */
#ifndef ST67_MBTCP_IDLE_MS
#define ST67_MBTCP_IDLE_MS           120000U
#endif
/* A frame that stays incomplete for this long closes the client. */
#ifndef ST67_MBTCP_PARTIAL_MS
#define ST67_MBTCP_PARTIAL_MS        2000U
#endif
/* How long the I/O task waits for the engine's answer to one request. */
#ifndef ST67_MBTCP_RESP_MS
#define ST67_MBTCP_RESP_MS           500U
#endif
/* Below the engine task (SPLC) and the ST67 task, above nothing the engine depends on. */
#ifndef ST67_MBTCP_IO_PRIO
#define ST67_MBTCP_IO_PRIO           4U
#endif
#ifndef ST67_MBTCP_ACC_PRIO
#define ST67_MBTCP_ACC_PRIO          3U
#endif
#ifndef ST67_MBTCP_IO_STACK_WORDS
#define ST67_MBTCP_IO_STACK_WORDS    1024U
#endif
#ifndef ST67_MBTCP_ACC_STACK_WORDS
#define ST67_MBTCP_ACC_STACK_WORDS   768U
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Engine task, once, after plc_engine_init() and after the ST67 task exists.
 * Creates the two tasks (they wait until link_up() is true) and fills *out, which the caller
 * passes to plc_modbus_cfg_add_transport(). link_up: true while Wi-Fi has an IP address
 * (splc_wifi_port_has_ip). Returns false (and logs) when a task or queue cannot be created. */
bool st67_modbus_tcp_create(modbus_transport_t *out, bool (*link_up)(void));

#ifdef __cplusplus
}
#endif

#endif /* ST67_MODBUS_TCP_H */