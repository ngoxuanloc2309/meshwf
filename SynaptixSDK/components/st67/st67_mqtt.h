/**
 * @file st67_mqtt.h
 * @brief Thin wrapper of the vendor MQTT client that runs INSIDE the ST67W611 module (AT).
 *
 * Why here and not in libs/st67w611: the facade stays Wi-Fi only. It owns the single vendor
 * callback table and forwards APP_mqtt_cb to st67w611_set_mqtt_event_cb(); this component
 * hooks that and calls W6X_MQTT_*.
 *
 * Threading: every function except st67_mqtt_is_connected() must be called from ONE task
 * (the ST67 task): they run AT commands over SPI and block for as long as the module needs.
 * Nothing here calls SimplePLC.
 *
 * Limits of the vendor API (read in w6x_types.h, not assumed):
 *   host 63 chars, client id / user / password 31 chars each (a longer string is refused,
 *   never truncated), one topic per subscribe and ALWAYS at QoS 0 (W61_MQTT_Subscribe
 *   hard-codes it), no retain/QoS flag on received messages, MQTT 3.1.1 only, TLS needs PEM
 *   strings in memory the MCU can address: this wrapper takes them straight from
 *   st67_cert_store (memory-mapped Flash, NUL terminated), see use_tls below.
 *   A received message must fit ST67_MQTT_RX_BUF_SIZE as '"topic"' + NUL + payload; the vendor
 *   drops a bigger one before the application sees it.
 */
#ifndef ST67_MQTT_H
#define ST67_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST67_MQTT_HOST_MAX   63U
#define ST67_MQTT_CRED_MAX   31U
#define ST67_MQTT_TOPIC_MAX  95U     /* longest topic accepted by subscribe / unsubscribe */

typedef enum {
    ST67_MQTT_OK           =  0,
    ST67_MQTT_ERR_ARG      = -1,   /* NULL / empty host / port 0 */
    ST67_MQTT_ERR_TOO_LONG = -2,   /* a string is longer than the module accepts */
    ST67_MQTT_ERR_NOT_INIT = -3,   /* st67_mqtt_init() did not succeed yet */
    ST67_MQTT_ERR_VENDOR   = -4,   /* the module / vendor driver refused the command */
    ST67_MQTT_ERR_TIMEOUT  = -5,   /* no CONNECTED event in time */
    ST67_MQTT_ERR_CERT     = -6    /* use_tls but the certificate set is missing / inconsistent /
                                    * temporarily unreadable (reason logged) */
} st67_mqtt_status_t;

typedef struct {
    char     host[ST67_MQTT_HOST_MAX + 1U];
    uint16_t port;
    char     client_id[ST67_MQTT_CRED_MAX + 1U];   /* may be empty: the module picks one */
    char     username[ST67_MQTT_CRED_MAX + 1U];    /* empty = no authentication (scheme 0) */
    char     password[ST67_MQTT_CRED_MAX + 1U];
    uint16_t keepalive_sec;                        /* 0 = module default (120 s) */
    bool     clean_session;
    /* TLS (the certificates come from st67_cert_store, not from this struct):
     *   Root CA stored, no client cert/key  -> vendor scheme 2 (server verified);
     *   Root CA + client cert + client key  -> scheme 4 (mutual TLS);
     * anything else (no Root CA, or only one of cert/key) -> ST67_MQTT_ERR_CERT, nothing is sent.
     * A Root CA is REQUIRED: scheme 3 (client cert without verifying the server) is never used.
     * The host name doubles as SNI. Username/password are still passed to the module. */
    bool     use_tls;
} st67_mqtt_config_t;

/* Hook the facade event and give the vendor its receive buffer. Call after
 * st67w611_start() succeeded, and again after a module reset. */
st67_mqtt_status_t st67_mqtt_init(void);

/* Configure + connect (with TLS when cfg->use_tls). Blocks until the module reports CONNECTED or timeout_ms elapsed
 * (then it disconnects again). The caller must have an IP address. */
st67_mqtt_status_t st67_mqtt_connect(const st67_mqtt_config_t *cfg, uint32_t timeout_ms);
st67_mqtt_status_t st67_mqtt_disconnect(void);

/* qos 0..2. payload need not be NUL-terminated. */
st67_mqtt_status_t st67_mqtt_publish(const char *topic, const char *payload, size_t len,
                                     uint8_t qos, bool retain);

/* Subscribe / unsubscribe one topic (filters with + and # are fine). Requires a live session.
 * ST67 task only. ST67_MQTT_ERR_ARG for an empty topic or one with '"', CR or LF (it would
 * corrupt the AT command); ST67_MQTT_ERR_TOO_LONG above ST67_MQTT_TOPIC_MAX. The vendor
 * subscribes at QoS 0 only. */
st67_mqtt_status_t st67_mqtt_subscribe(const char *topic);
st67_mqtt_status_t st67_mqtt_unsubscribe(const char *topic);

/* Received-message hook. Runs in the VENDOR callback context (the AT receive task): it must
 * not block and must copy what it needs, because topic and payload point into the vendor
 * buffer that the next message overwrites. topic is NUL-terminated and without quotes; the
 * payload is NOT terminated. There is no retain flag (the vendor does not report it).
 * NULL removes the hook. Safe to call before st67_mqtt_init(). */
typedef void (*st67_mqtt_rx_cb_t)(const char *topic, const char *payload, size_t len, void *context);
void st67_mqtt_set_rx_cb(st67_mqtt_rx_cb_t cb, void *context);

/* Messages the vendor reported but this layer could not hand over (bad framing). Any task. */
uint32_t st67_mqtt_rx_malformed(void);

/* Last connection state from the module events. Any task. */
bool st67_mqtt_is_connected(void);

/* The facade reports a module reset: the session is gone and the vendor MQTT object must
 * be initialised again. Only sets flags (safe from a lifecycle hook); the next
 * st67_mqtt_init() clears them. */
void st67_mqtt_on_module_reset(void);
bool st67_mqtt_needs_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ST67_MQTT_H */