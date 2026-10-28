/**
 * @file splc_wifi_port.h
 * @brief Glue between SimplePLC's Wi-Fi block (plc_wifi, ICD WIFI-004, 0x1300..0x13FF)
 *        and the ST67W611 facade (libs/st67w611, AT mode T01) of the meshwf board.
 *
 * plc_wifi owns no radio: it keeps what the App wrote and calls the NON-BLOCKING
 * functions registered here (engine task). Everything slow (connect, scan, IP query)
 * runs in the EXISTING "ST67" task of splc_port.c - no extra task, no extra stack
 * (the FreeRTOS heap of this board is only 32 KB). The callbacks only set a flag or
 * copy a cached value and return at once.
 *
 * Wiring (see splc_port.c):
 *   engine task : plc_engine_init();  splc_wifi_port_init();
 *   ST67 task   : st67w611_init/start OK -> splc_wifi_port_set_ready(true);
 *                 then loop { splc_wifi_port_service(250); ... }
 *   st67 event_cb: splc_wifi_port_on_event(event, data);
 *
 * What the ST67 facade can and cannot report (read in libs/st67w611, not assumed):
 *   - AUTH_FAILED / SSID_NOT_FOUND are not distinguished: a failed connect is reported as
 *     DISCONNECTED with error_code 3 (driver).
 *   - RSSI: st67w611_get_rssi() = UNSUPPORTED, so rssi_dbm is 0 ("unknown").
 *   - Scan results: ssid, rssi, channel, bssid. No security mode, so auth_mode = 0.
 *     Only the 8 strongest networks are kept.
 *   - Static IP, SoftAP and STA+AP are NOT in the facade: a CONNECT asking for them is
 *     refused (error 3) instead of silently falling back to DHCP / station.
 *   - st67w611_get_state() takes the facade's operation lock (it fails while a connect
 *     runs), so the engine-side status() never calls the facade: it reads a cache that
 *     the events and the ST67 task keep up to date.
 *
 * Persistence (SPLC_HOST_WIFI_PERSIST): 0 (default) = none, the credentials are lost at
 * power-off and the library logs a warning. 1 = the product supplies the three
 * meshwf_wifi_store_*() functions below. They are called SYNCHRONOUSLY from the engine
 * task (the only task allowed to touch SimplePLC Flash), so a Flash implementation must
 * wrap its erase/program in retain_flash_op_begin()/retain_flash_op_end().
 *
 * Compiles to nothing when SPLC_WIFI_ENABLE = 0.
 */
#ifndef SPLC_WIFI_PORT_H
#define SPLC_WIFI_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "splc_opt.h"
#include "st67w611.h"

#ifndef SPLC_HOST_WIFI_PERSIST
#define SPLC_HOST_WIFI_PERSIST        0
#endif
/* How long after a connect the ST67 task keeps asking the module for the IP (DHCP). */
#ifndef SPLC_HOST_WIFI_IP_WAIT_MS
#define SPLC_HOST_WIFI_IP_WAIT_MS     15000U
#endif
/* A scan whose results never arrive is declared FAILED after this time. */
#ifndef SPLC_HOST_WIFI_SCAN_TIMEOUT_MS
#define SPLC_HOST_WIFI_SCAN_TIMEOUT_MS 15000U
#endif

#if SPLC_WIFI_ENABLE

#include "plc_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the host functions with the library. Engine task, once, after plc_engine_init().
 * Logs and returns when registration fails. */
void splc_wifi_port_init(void);

/* ST67 task: tell the glue whether the module finished st67w611_init() + start(). Until it
 * is true, connect/scan answer error 4 (hardware not ready) and the library retries. */
void splc_wifi_port_set_ready(bool ready);

/* ST67 task: call in a loop. Blocks up to wait_ms for work, then runs it (connect,
 * disconnect, scan, IP polling). Never returns an error; failures end up in status(). */
void splc_wifi_port_service(uint32_t wait_ms);

/* Facade event hook (st67w611_config_t.event_cb). Vendor callback context: it only copies
 * a few values and wakes the ST67 task. Safe to call before splc_wifi_port_init(). */
void splc_wifi_port_on_event(st67w611_event_t event, void *event_data);

#if SPLC_HOST_WIFI_PERSIST
/* Product-supplied storage. Engine task, synchronous. */
bool meshwf_wifi_store_load(splc_wifi_config_t *out);        /* false = nothing stored */
bool meshwf_wifi_store_save(const splc_wifi_config_t *cfg);  /* false = not stored */
void meshwf_wifi_store_erase(void);
#endif

#ifdef __cplusplus
}
#endif

#else  /* SPLC_WIFI_ENABLE == 0 */

static inline void splc_wifi_port_init(void) {}
static inline void splc_wifi_port_set_ready(bool ready) { (void)ready; }
static inline void splc_wifi_port_service(uint32_t wait_ms) { (void)wait_ms; }
static inline void splc_wifi_port_on_event(st67w611_event_t event, void *event_data)
{ (void)event; (void)event_data; }

#endif /* SPLC_WIFI_ENABLE */

#endif /* SPLC_WIFI_PORT_H */