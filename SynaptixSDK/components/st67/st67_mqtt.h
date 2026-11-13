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
#define ST67_MQTT_WILL_TOPIC_MAX    63U   /* vendor WillTopic[64] / WillMessage[64] */
#define ST67_MQTT_WILL_PAYLOAD_MAX  63U

typedef enum {
    ST67_MQTT_OK           =  0,
    ST67_MQTT_ERR_ARG      = -1,   /* NULL / empty host / port 0 */
    ST67_MQTT_ERR_TOO_LONG = -2,   /* a string is longer than the module accepts */
    ST67_MQTT_ERR_NOT_INIT = -3,   /* st67_mqtt_init() did not succeed yet */
    ST67_MQTT_ERR_VENDOR   = -4,   /* the module / vendor driver refused the command */
    ST67_MQTT_ERR_TIMEOUT  = -5,   /* no CONNECTED event in time */
    ST67_MQTT_ERR_CERT     = -6,   /* use_tls but the certificate set is missing / inconsistent /
                                    * temporarily unreadable (reason logged) */
    ST67_MQTT_ERR_BUSY     = -7    /* st67_mqtt_recover_module(): the facade is busy (scan or another
                                    * operation); nothing was done, try again later */
} st67_mqtt_status_t;

typedef struct {
    char     host[ST67_MQTT_HOST_MAX + 1U];
    uint16_t port;
    char     client_id[ST67_MQTT_CRED_MAX + 1U];   /* may be empty: the module picks one */
    char     username[ST67_MQTT_CRED_MAX + 1U];    /* empty = no authentication (scheme 0) */
    char     password[ST67_MQTT_CRED_MAX + 1U];
    uint16_t keepalive_sec;                        /* 0 = module default (120 s) */
    bool     clean_session;
    /* TLS (the certificates come from st67_cert_store, not from this struct). use_mtls (the App's
     * mutual-TLS switch, only meaningful with use_tls) decides the scheme, NOT what the store holds:
     *   use_mtls = false: Root CA stored          -> vendor scheme 2 (server verified); a client
     *                     cert/key that is still in the store is NOT sent;
     *   use_mtls = true:  Root CA + client cert + client key -> scheme 4 (mutual TLS);
     * anything else (no Root CA; use_mtls without the client cert or without the client key)
     * -> ST67_MQTT_ERR_CERT, nothing is sent (no silent fall-back to one-way TLS).
     * A Root CA is REQUIRED: scheme 3 (client cert without verifying the server) is never used.
     * The host name doubles as SNI. Username/password are still passed to the module. */
    bool     use_tls;
    bool     use_mtls;
    /* Last Will (vendor WillTopic / WillMessage / WillQos / WillRetain). will_enabled = false: no Will
     * is sent (the vendor strings stay empty). The vendor pastes both strings, UNESCAPED, between
     * double quotes into the AT command, so a '"', a backslash, CR, LF or any non-printable
     * character would corrupt it: st67_mqtt_will_text_ok() says which text can be sent, and
     * st67_mqtt_connect() refuses (ST67_MQTT_ERR_ARG) a Will that fails it. The payload may be
     * empty, the topic may not. */
    bool     will_enabled;
    bool     will_retain;
    uint8_t  will_qos;                                         /* 0..2 */
    char     will_topic[ST67_MQTT_WILL_TOPIC_MAX + 1U];
    char     will_payload[ST67_MQTT_WILL_PAYLOAD_MAX + 1U];
} st67_mqtt_config_t;

/* true when `text` (NUL-terminated) is printable ASCII (0x20..0x7E) without '"' and '\\', i.e. it can
 * go into the vendor's quoted AT parameter as it is. NULL is not ok. Any task. */
bool st67_mqtt_will_text_ok(const char *text);

/* Hook the facade event and give the vendor its receive buffer. Call after
 * st67w611_start() succeeded, and again after a module reset. */
st67_mqtt_status_t st67_mqtt_init(void);

/* Configure + connect (with TLS when cfg->use_tls). Blocks until the module reports CONNECTED or timeout_ms elapsed
 * (then it disconnects again). The caller must have an IP address. */
st67_mqtt_status_t st67_mqtt_connect(const st67_mqtt_config_t *cfg, uint32_t timeout_ms);
st67_mqtt_status_t st67_mqtt_disconnect(void);

/* Soft-reset the module (AT+RST through the facade) to get rid of MQTT state it may still hold from
 * an earlier session. HEAVY: the Wi-Fi link, every socket (Modbus TCP) and the MQTT session are lost
 * for several seconds. sx_mqtt uses it once after repeated connect failures, never routinely.
 * The facade's lifecycle hook marks the session as gone. ST67 task only, blocking.
 *   ST67_MQTT_OK       the module restarted;
 *   ST67_MQTT_ERR_BUSY the facade is busy: nothing changed;
 *   ST67_MQTT_ERR_VENDOR any other failure (the facade may be in ERROR now; reason logged). */
st67_mqtt_status_t st67_mqtt_recover_module(void);

/* Delete the three TLS files (splc_ca.pem, splc_cert.pem, splc_key.pem) the vendor wrote into the
 * module's file system when it connected with TLS; the private key stays there otherwise.
 * Lists the module's files, deletes ours, lists again and checks they are gone.
 * ST67_MQTT_OK = none of them is in the module any more (absent files count as gone);
 * ST67_MQTT_ERR_VENDOR = the module refused or a file survived (call again later).
 * ST67 task only, module up. Without SPLC_MQTT_TLS_CERT_ENABLE it does nothing and returns OK. */
st67_mqtt_status_t st67_mqtt_wipe_certs(void);

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

/* Ask the MODULE whether its MQTT session is still up (AT query: uses the AT channel, so call it
 * from the ST67 task like every other function here). *connected = true when the module reports
 * one of the three CONNECTED states; *state_out (may be NULL) = the raw vendor state.
 * Returns ST67_MQTT_OK when the state could be read; any other status means "unknown": do NOT
 * treat it as a lost session. Unlike st67_mqtt_is_connected() this does not rely on a DISCONNECTED
 * event having reached the host. */
st67_mqtt_status_t st67_mqtt_query_session(bool *connected, uint32_t *state_out);

/* The facade reports a module reset: the session is gone and the vendor MQTT object must
 * be initialised again. Only sets flags (safe from a lifecycle hook); the next
 * st67_mqtt_init() clears them. */
void st67_mqtt_on_module_reset(void);
bool st67_mqtt_needs_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ST67_MQTT_H */