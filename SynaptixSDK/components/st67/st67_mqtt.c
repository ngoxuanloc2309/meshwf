#include "st67_mqtt.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "st67w611.h"
#include "w6x_api.h"
#include "w6x_types.h"

/* Receive buffer the vendor needs even when nothing is subscribed (topic + message of a
 * received publish). Subscribe is not used yet; when it is, size it for the longest
 * topic + payload SimplePLC accepts (95 + 128). */
#ifndef ST67_MQTT_RX_BUF_SIZE
#define ST67_MQTT_RX_BUF_SIZE   256U
#endif

static uint8_t            s_rx[ST67_MQTT_RX_BUF_SIZE];
static W6X_MQTT_Data_t    s_rx_desc = { s_rx, ST67_MQTT_RX_BUF_SIZE };
static W6X_MQTT_Connect_t s_vendor_cfg;          /* ~0.6 KB, static: the ST67 stack is 4 KB */

static volatile bool s_inited;
static volatile bool s_connected;
static volatile bool s_reset_seen;

/* Vendor callback context (through the facade forwarder): flags only. */
static void on_event(uint32_t event_id, void *event_args, void *context)
{
    (void)event_args;
    (void)context;
    if (event_id == W6X_MQTT_EVT_CONNECTED_ID) {
        s_connected = true;
    } else if (event_id == W6X_MQTT_EVT_DISCONNECTED_ID) {
        s_connected = false;
    }
    /* W6X_MQTT_EVT_SUBSCRIPTION_RECEIVED_ID: ignored until Subscribe is implemented. */
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
    if (W6X_MQTT_Configure(&s_vendor_cfg) != W6X_STATUS_OK) {
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