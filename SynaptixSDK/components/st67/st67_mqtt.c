#include "st67_mqtt.h"
#include "splc_opt.h"
#if SPLC_MQTT_TLS_CERT_ENABLE
#include "st67_cert_store.h"
#endif
#include "splc_log.h"
#include "logger.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "st67w611.h"
#include "w6x_api.h"
#include "w6x_types.h"

/* Receive buffer of the vendor: '"topic"' + NUL + payload of one received publish. 256 B
 * holds the longest topic sx_mqtt queues (96 + 2 quotes) plus a 128 B payload with room
 * to spare; a bigger message is dropped by the vendor itself. */
#ifndef ST67_MQTT_RX_BUF_SIZE
#define ST67_MQTT_RX_BUF_SIZE   256U
#endif

static uint8_t            s_rx[ST67_MQTT_RX_BUF_SIZE];
static W6X_MQTT_Data_t    s_rx_desc = { s_rx, ST67_MQTT_RX_BUF_SIZE };
static W6X_MQTT_Connect_t s_vendor_cfg;          /* ~0.6 KB, static: the ST67 stack is 4 KB */

static const char *TAG = "ST67_MQTT";

static volatile bool s_inited;
static volatile bool s_connected;
static volatile bool s_reset_seen;

static volatile st67_mqtt_rx_cb_t s_rx_cb;
static void * volatile            s_rx_ctx;
static volatile uint32_t          s_rx_malformed;

/* Vendor callback context (through the facade forwarder): flags only. */
static void on_event(uint32_t event_id, void *event_args, void *context)
{
    (void)context;
    if (event_id == W6X_MQTT_EVT_CONNECTED_ID) {
        s_connected = true;
    } else if (event_id == W6X_MQTT_EVT_DISCONNECTED_ID) {
        s_connected = false;
    } else if (event_id == W6X_MQTT_EVT_SUBSCRIPTION_RECEIVED_ID) {
        /* Vendor layout in s_rx: '"' topic '"' NUL payload. topic_length counts both
         * quotes; the payload starts at topic_length + 1 and is NOT terminated. */
        const st67_mqtt_rx_cb_t cb = s_rx_cb;
        if (cb == NULL) {
            return;
        }
        const W6X_MQTT_CbParamData_t *d = (const W6X_MQTT_CbParamData_t *)event_args;
        if (d == NULL) {
            s_rx_malformed++;
            return;
        }
        const uint32_t tl = d->topic_length;
        const uint32_t ml = d->message_length;
        if (tl < 2U || tl > ST67_MQTT_RX_BUF_SIZE || ml > ST67_MQTT_RX_BUF_SIZE ||
            (tl + 1U + ml) > ST67_MQTT_RX_BUF_SIZE ||
            s_rx[0] != (uint8_t)'"' || s_rx[tl - 1U] != (uint8_t)'"') {
            s_rx_malformed++;
            return;
        }
        s_rx[tl - 1U] = 0U;                  /* closing quote -> NUL: the topic is s_rx + 1 */
        cb((const char *)&s_rx[1], (const char *)&s_rx[tl + 1U], (size_t)ml, s_rx_ctx);
    }
}

static st67_mqtt_status_t copy_str(uint8_t *dst, size_t dst_size, const char *src)
{
    const size_t n = strlen(src);
    if (n >= dst_size) {
        return ST67_MQTT_ERR_TOO_LONG;
    }
    memcpy(dst, src, n + 1U);
    return ST67_MQTT_OK;
}

#if SPLC_MQTT_TLS_CERT_ENABLE
/* Names of the files the vendor writes into the module's file system (<= 31 chars). None is a
 * prefix of another: the vendor matches existing files with strncmp(name, listed, strlen(name)). */
#define TLS_FILE_CA     "splc_ca.pem"
#define TLS_FILE_CERT   "splc_cert.pem"
#define TLS_FILE_KEY    "splc_key.pem"

/* Fill the TLS part of the vendor config from the certificate store. On ST67_MQTT_OK the store
 * is HELD (acquire) and the content pointers point into Flash: the caller must call
 * tls_detach() as soon as W6X_MQTT_Configure() returned (the vendor copies the PEM into the
 * module there), so the engine task can erase the sector again. */
static st67_mqtt_status_t tls_attach(W6X_MQTT_Connect_t *v, const char *host)
{
    if (!st67_cert_store_acquire()) {
        log_warn(TAG, "TLS: certificate store busy (being rewritten), try again later");
        return ST67_MQTT_ERR_CERT;
    }
    const char *ca   = st67_cert_store_get(ST67_CERT_ROOT_CA, NULL);
    const char *cert = st67_cert_store_get(ST67_CERT_CLIENT_CERT, NULL);
    const char *key  = st67_cert_store_get(ST67_CERT_CLIENT_KEY, NULL);

    if (ca == NULL) {
        st67_cert_store_release();
        log_warn(TAG, "TLS: no Root CA stored, refusing to connect (the server cannot be verified)");
        return ST67_MQTT_ERR_CERT;
    }
    if ((cert == NULL) != (key == NULL)) {
        st67_cert_store_release();
        log_warn(TAG, "TLS: client %s stored without the client %s, refusing to connect",
                 (cert != NULL) ? "certificate" : "key", (cert != NULL) ? "key" : "certificate");
        return ST67_MQTT_ERR_CERT;
    }
    if (copy_str(v->SNI, sizeof(v->SNI), host) != ST67_MQTT_OK ||
        copy_str(v->CACertificateName, sizeof(v->CACertificateName), TLS_FILE_CA) != ST67_MQTT_OK) {
        st67_cert_store_release();
        return ST67_MQTT_ERR_TOO_LONG;
    }
    v->CACertificateContent = (char *)(uintptr_t)ca;     /* the vendor only reads it */
    v->Scheme               = 2U;
    if (cert != NULL) {
        if (copy_str(v->CertificateName, sizeof(v->CertificateName), TLS_FILE_CERT) != ST67_MQTT_OK ||
            copy_str(v->PrivateKeyName, sizeof(v->PrivateKeyName), TLS_FILE_KEY) != ST67_MQTT_OK) {
            st67_cert_store_release();
            return ST67_MQTT_ERR_TOO_LONG;
        }
        v->CertificateContent = (char *)(uintptr_t)cert;
        v->PrivateKeyContent  = (char *)(uintptr_t)key;
        v->Scheme             = 4U;
    }
    return ST67_MQTT_OK;
}

static void tls_detach(W6X_MQTT_Connect_t *v)
{
    v->CACertificateContent = NULL;
    v->CertificateContent   = NULL;
    v->PrivateKeyContent    = NULL;
    st67_cert_store_release();
}
#endif /* SPLC_MQTT_TLS_CERT_ENABLE */

st67_mqtt_status_t st67_mqtt_init(void)
{
    s_connected = false;
    st67w611_set_mqtt_event_cb(on_event, NULL);
    if (W6X_MQTT_Init(&s_rx_desc) != W6X_STATUS_OK) {
        s_inited = false;
        return ST67_MQTT_ERR_VENDOR;
    }
    s_inited     = true;
    s_reset_seen = false;
    return ST67_MQTT_OK;
}

st67_mqtt_status_t st67_mqtt_connect(const st67_mqtt_config_t *cfg, uint32_t timeout_ms)
{
    if (cfg == NULL || cfg->host[0] == '\0' || cfg->port == 0U) {
        return ST67_MQTT_ERR_ARG;
    }
    if (!s_inited) {
        return ST67_MQTT_ERR_NOT_INIT;
    }

    memset(&s_vendor_cfg, 0, sizeof(s_vendor_cfg));
    if (copy_str(s_vendor_cfg.HostName,   sizeof(s_vendor_cfg.HostName),   cfg->host)      != ST67_MQTT_OK ||
        copy_str(s_vendor_cfg.MQClientId, sizeof(s_vendor_cfg.MQClientId), cfg->client_id) != ST67_MQTT_OK ||
        copy_str(s_vendor_cfg.MQUserName, sizeof(s_vendor_cfg.MQUserName), cfg->username)  != ST67_MQTT_OK ||
        copy_str(s_vendor_cfg.MQUserPwd,  sizeof(s_vendor_cfg.MQUserPwd),  cfg->password)  != ST67_MQTT_OK) {
        return ST67_MQTT_ERR_TOO_LONG;
    }
    s_vendor_cfg.HostPort            = cfg->port;
    s_vendor_cfg.Scheme              = (cfg->username[0] != '\0') ? 1U : 0U;
    s_vendor_cfg.KeepAlive           = cfg->keepalive_sec;
    s_vendor_cfg.DisableCleanSession = cfg->clean_session ? 0U : 1U;
    /* Last Will: left empty (decision: later). */

    s_connected = false;
#if SPLC_MQTT_TLS_CERT_ENABLE
    if (cfg->use_tls) {
        const st67_mqtt_status_t t = tls_attach(&s_vendor_cfg, cfg->host);
        if (t != ST67_MQTT_OK) {
            return t;
        }
    }
#else
    if (cfg->use_tls) {
        log_warn(TAG, "TLS requested but SPLC_MQTT_TLS_CERT_ENABLE is 0");
        return ST67_MQTT_ERR_CERT;
    }
#endif
    const W6X_Status_t conf = W6X_MQTT_Configure(&s_vendor_cfg);
#if SPLC_MQTT_TLS_CERT_ENABLE
    if (cfg->use_tls) {
        tls_detach(&s_vendor_cfg);       /* the PEM is in the module now; free the sector */
    }
#endif
    if (conf != W6X_STATUS_OK) {
        return ST67_MQTT_ERR_VENDOR;
    }
    if (W6X_MQTT_Connect(&s_vendor_cfg) != W6X_STATUS_OK) {
        return ST67_MQTT_ERR_VENDOR;
    }

    const TickType_t start = xTaskGetTickCount();
    while (!s_connected) {
        if ((TickType_t)(xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            (void)W6X_MQTT_Disconnect();
            s_connected = false;
            return ST67_MQTT_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    return ST67_MQTT_OK;
}

st67_mqtt_status_t st67_mqtt_wipe_certs(void)
{
#if SPLC_MQTT_TLS_CERT_ENABLE
    static const char *const names[3] = { TLS_FILE_CA, TLS_FILE_CERT, TLS_FILE_KEY };
    W6X_FS_FilesListFull_t *list = NULL;
    st67_mqtt_status_t      st   = ST67_MQTT_OK;

    if (W6X_FS_ListFiles(&list) != W6X_STATUS_OK || list == NULL) {
        return ST67_MQTT_ERR_VENDOR;
    }
    for (uint32_t k = 0U; k < 3U; k++) {
        bool found = false;
        for (uint32_t i = 0U; i < list->ncp_files_list.nb_files && i < W6X_SYS_FS_MAX_FILES; i++) {
            /* same match rule as the vendor's own overwrite check */
            if (strncmp(list->ncp_files_list.filename[i], names[k], strlen(names[k])) == 0) {
                found = true;
                break;
            }
        }
        if (found) {
            char name[W6X_SYS_FS_FILENAME_SIZE];
            (void)memset(name, 0, sizeof(name));
            (void)strncpy(name, names[k], sizeof(name) - 1U);
            if (W6X_FS_DeleteFile(name) != W6X_STATUS_OK) {
                st = ST67_MQTT_ERR_VENDOR;
            }
        }
    }
    /* Do not trust the delete status alone: look again. */
    list = NULL;
    if (W6X_FS_ListFiles(&list) != W6X_STATUS_OK || list == NULL) {
        return ST67_MQTT_ERR_VENDOR;
    }
    for (uint32_t k = 0U; k < 3U; k++) {
        for (uint32_t i = 0U; i < list->ncp_files_list.nb_files && i < W6X_SYS_FS_MAX_FILES; i++) {
            if (strncmp(list->ncp_files_list.filename[i], names[k], strlen(names[k])) == 0) {
                st = ST67_MQTT_ERR_VENDOR;
            }
        }
    }
    return st;
#else
    return ST67_MQTT_OK;
#endif
}

st67_mqtt_status_t st67_mqtt_disconnect(void)
{
    if (!s_inited) {
        return ST67_MQTT_ERR_NOT_INIT;
    }
    const W6X_Status_t st = W6X_MQTT_Disconnect();
    s_connected = false;
    return (st == W6X_STATUS_OK) ? ST67_MQTT_OK : ST67_MQTT_ERR_VENDOR;
}

st67_mqtt_status_t st67_mqtt_publish(const char *topic, const char *payload, size_t len,
                                     uint8_t qos, bool retain)
{
    if (topic == NULL || topic[0] == '\0' || (payload == NULL && len != 0U) || qos > 2U) {
        return ST67_MQTT_ERR_ARG;
    }
    if (!s_inited) {
        return ST67_MQTT_ERR_NOT_INIT;
    }
    /* The vendor prototype takes non-const pointers but only reads them. */
    if (W6X_MQTT_Publish((uint8_t *)(uintptr_t)topic, (uint8_t *)(uintptr_t)payload,
                         (uint32_t)len, qos, retain ? 1U : 0U) != W6X_STATUS_OK) {
        return ST67_MQTT_ERR_VENDOR;
    }
    return ST67_MQTT_OK;
}

static st67_mqtt_status_t check_topic(const char *topic)
{
    if (topic == NULL || topic[0] == '\0') {
        return ST67_MQTT_ERR_ARG;
    }
    for (size_t n = 0U; topic[n] != '\0'; n++) {
        const char c = topic[n];
        if (c == '"' || c == '\r' || c == '\n') {
            return ST67_MQTT_ERR_ARG;
        }
        if (n >= ST67_MQTT_TOPIC_MAX) {
            return ST67_MQTT_ERR_TOO_LONG;
        }
    }
    return ST67_MQTT_OK;
}

st67_mqtt_status_t st67_mqtt_subscribe(const char *topic)
{
    const st67_mqtt_status_t c = check_topic(topic);
    if (c != ST67_MQTT_OK) {
        return c;
    }
    if (!s_inited) {
        return ST67_MQTT_ERR_NOT_INIT;
    }
    return (W6X_MQTT_Subscribe((uint8_t *)(uintptr_t)topic) == W6X_STATUS_OK) ? ST67_MQTT_OK
                                                                              : ST67_MQTT_ERR_VENDOR;
}

st67_mqtt_status_t st67_mqtt_unsubscribe(const char *topic)
{
    const st67_mqtt_status_t c = check_topic(topic);
    if (c != ST67_MQTT_OK) {
        return c;
    }
    if (!s_inited) {
        return ST67_MQTT_ERR_NOT_INIT;
    }
    return (W6X_MQTT_Unsubscribe((uint8_t *)(uintptr_t)topic) == W6X_STATUS_OK) ? ST67_MQTT_OK
                                                                                : ST67_MQTT_ERR_VENDOR;
}

void st67_mqtt_set_rx_cb(st67_mqtt_rx_cb_t cb, void *context)
{
    s_rx_ctx = context;
    s_rx_cb  = cb;
}

uint32_t st67_mqtt_rx_malformed(void)
{
    return s_rx_malformed;
}

bool st67_mqtt_is_connected(void)
{
    return s_connected;
}

void st67_mqtt_on_module_reset(void)
{
    s_connected  = false;
    s_reset_seen = true;
}

bool st67_mqtt_needs_init(void)
{
    return !s_inited || s_reset_seen;
}