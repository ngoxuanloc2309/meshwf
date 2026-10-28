#include "st67w611.h"
#include "st67w611_port.h"
#include "st67w611_backend.h"
#include "w6x_api.h"
#include "w6x_types.h"
#include "w61_at_api.h"
#include <stddef.h>
#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"

static st67w611_config_t config;
static st67w611_state_t state = ST67W611_STATE_OFF;
static st67w611_diagnostics_t diagnostics;
static W6X_App_Cb_t vendor_callbacks;
static uint8_t started;
static st67w611_scan_callback_t scan_callback;
static void *scan_user_context;
static volatile uint8_t scan_active;
static SemaphoreHandle_t operation_lock;

_Static_assert(offsetof(W6X_WiFi_Scan_Result_t, Count) ==
               offsetof(W61_WiFi_Scan_Result_t, Count),
               "vendor scan Count layout changed");
_Static_assert(offsetof(W6X_WiFi_Scan_Result_t, AP) ==
               offsetof(W61_WiFi_Scan_Result_t, AP),
               "vendor scan AP layout changed");
_Static_assert(offsetof(W6X_WiFi_Ap_t, SSID) ==
               offsetof(W61_WiFi_AP_t, SSID),
               "vendor scan SSID layout changed");
_Static_assert(offsetof(W6X_WiFi_Ap_t, RSSI) ==
               offsetof(W61_WiFi_AP_t, RSSI),
               "vendor scan RSSI layout changed");
_Static_assert(offsetof(W6X_WiFi_Ap_t, MAC) ==
               offsetof(W61_WiFi_AP_t, MAC),
               "vendor scan MAC layout changed");
_Static_assert(offsetof(W6X_WiFi_Ap_t, Channel) ==
               offsetof(W61_WiFi_AP_t, Channel),
               "vendor scan channel layout changed");

static size_t bounded_strlen(const char *value, size_t capacity)
{
    size_t length = 0U;
    if (value == NULL) return 0U;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length;
}

static int lock_operation(void)
{
    if (operation_lock == NULL) operation_lock = xSemaphoreCreateMutex();
    return operation_lock != NULL && xSemaphoreTake(operation_lock, 0U) == pdTRUE;
}

static void unlock_operation(void)
{
    if (operation_lock != NULL) (void)xSemaphoreGive(operation_lock);
}

static void set_state(st67w611_state_t next, st67w611_status_t error)
{
    if (state != next) {
        diagnostics.state_transition_count++;
    }
    state = next;
    diagnostics.state = next;
    if (error != ST67W611_OK) {
        diagnostics.last_error = error;
    }
}

static st67w611_status_t map_status(W6X_Status_t status)
{
    if (status == W6X_STATUS_OK) return ST67W611_OK;
    if (status == W6X_STATUS_BUSY) return ST67W611_ERR_BUSY;
    if (status == W6X_STATUS_TIMEOUT) {
        diagnostics.timeout_count++;
        return ST67W611_ERR_TIMEOUT;
    }
    if (status == W6X_STATUS_NOT_SUPPORTED) return ST67W611_ERR_UNSUPPORTED;
    diagnostics.transport_error_count++;
    return ST67W611_ERR_VENDOR;
}

static void publish(st67w611_event_t event, void *data)
{
    diagnostics.callback_count++;
    if (config.event_cb != NULL) {
        config.event_cb(event, data, config.user_context);
    }
}

static void vendor_wifi_callback(W6X_event_id_t event_id, void *event_args)
{
    if (event_id == W6X_WIFI_EVT_CONNECTING_ID) {
        set_state(ST67W611_STATE_CONNECTING, ST67W611_OK);
        publish(ST67W611_EVENT_CONNECTING, event_args);
    } else if (event_id == W6X_WIFI_EVT_CONNECTED_ID ||
               event_id == W6X_WIFI_EVT_STA_CONNECTED_ID) {
        set_state(ST67W611_STATE_CONNECTED, ST67W611_OK);
        publish(ST67W611_EVENT_CONNECTED, event_args);
    } else if (event_id == W6X_WIFI_EVT_GOT_IP_ID) {
        set_state(ST67W611_STATE_CONNECTED, ST67W611_OK);
        publish(ST67W611_EVENT_GOT_IP, event_args);
    } else if (event_id == W6X_WIFI_EVT_DISCONNECTED_ID ||
               event_id == W6X_WIFI_EVT_STA_DISCONNECTED_ID) {
        set_state(ST67W611_STATE_READY, ST67W611_OK);
        diagnostics.disconnect_count++;
        publish(ST67W611_EVENT_DISCONNECTED, event_args);
    }
}

static void vendor_error_callback(W6X_Status_t status, const char *function_name)
{
    (void)function_name;
    diagnostics.vendor_error_count++;
    diagnostics.last_error = map_status(status);
    set_state(ST67W611_STATE_ERROR, diagnostics.last_error);
    publish(ST67W611_EVENT_ERROR, (void *)function_name);
}

static void vendor_scan_callback(int32_t status, W6X_WiFi_Scan_Result_t *entry)
{
    st67w611_scan_result_t result;
    uint32_t i;
    if (scan_callback == NULL) return;
    if (status != 0 || entry == NULL || entry->AP == NULL) {
        scan_active = 0U;
        scan_callback(ST67W611_ERR_VENDOR, NULL, scan_user_context);
        scan_callback = NULL;
        scan_user_context = NULL;
        return;
    }
    for (i = 0U; i < entry->Count; ++i) {
        memset(&result, 0, sizeof(result));
        (void)strncpy(result.ssid, (const char *)entry->AP[i].SSID,
                      sizeof(result.ssid) - 1U);
        result.rssi = entry->AP[i].RSSI;
        memcpy(result.mac, entry->AP[i].MAC, sizeof(result.mac));
        result.channel = entry->AP[i].Channel;
        scan_callback(ST67W611_OK, &result, scan_user_context);
    }
    scan_callback(ST67W611_OK, NULL, scan_user_context);
    scan_active = 0U;
    scan_callback = NULL;
    scan_user_context = NULL;
}

st67w611_status_t st67w611_init(const st67w611_config_t *new_config)
{
    if (!lock_operation()) return ST67W611_ERR_BUSY;
    if (new_config == NULL || state != ST67W611_STATE_OFF ||
        new_config->port_ops == NULL) {
        st67w611_status_t result = (new_config == NULL ||
                                    new_config->port_ops == NULL)
                                       ? ST67W611_ERR_INVALID_ARG
                                       : ST67W611_ERR_INVALID_STATE;
        unlock_operation();
        return result;
    }

    if (new_config->port_ops->init == NULL ||
        new_config->port_ops->deinit == NULL ||
        new_config->port_ops->transfer == NULL ||
        new_config->port_ops->transfer_dma == NULL ||
        new_config->port_ops->cancel_transfer == NULL ||
        new_config->port_ops->set_cs == NULL ||
        new_config->port_ops->read_ready == NULL) {
        unlock_operation();
        return ST67W611_ERR_INVALID_ARG;
    }

    config = *new_config;
    st67w611_port_configure(config.port_ops, config.port_context);
    memset(&diagnostics, 0, sizeof(diagnostics));
    diagnostics.state = ST67W611_STATE_OFF;
    diagnostics.last_error = ST67W611_OK;
    set_state(ST67W611_STATE_INITIALIZING, ST67W611_OK);
    diagnostics.init_count++;

    memset(&vendor_callbacks, 0, sizeof(vendor_callbacks));
    vendor_callbacks.APP_wifi_cb = vendor_wifi_callback;
    vendor_callbacks.APP_error_cb = vendor_error_callback;
    if (W6X_RegisterAppCb(&vendor_callbacks) != W6X_STATUS_OK) {
        set_state(ST67W611_STATE_ERROR, ST67W611_ERR_VENDOR);
        unlock_operation();
        return ST67W611_ERR_VENDOR;
    }
    unlock_operation();
    return ST67W611_OK;
}

static st67w611_status_t start_locked(void)
{
    W6X_Status_t status;
    if (state != ST67W611_STATE_INITIALIZING) {
        return ST67W611_ERR_INVALID_STATE;
    }
    status = W6X_Init();
    if (status == W6X_STATUS_OK) status = W6X_WiFi_Init();
    if (status == W6X_STATUS_OK) status = W6X_WiFi_Station_Start();
    if (status == W6X_STATUS_OK) {
        st67w611_status_t backend_status = st67w611_backend_start();
        if (backend_status != ST67W611_OK) {
            set_state(ST67W611_STATE_ERROR, backend_status);
            return backend_status;
        }
    }
    if (status != W6X_STATUS_OK) {
        diagnostics.vendor_error_count++;
        set_state(ST67W611_STATE_ERROR, map_status(status));
        st67w611_status_t result = diagnostics.last_error;
        return result;
    }
    started = 1U;
    set_state(ST67W611_STATE_READY, ST67W611_OK);
    return ST67W611_OK;
}

st67w611_status_t st67w611_start(void)
{
    st67w611_status_t result;
    if (!lock_operation()) return ST67W611_ERR_BUSY;
    result = start_locked();
    unlock_operation();
    if (result == ST67W611_OK) publish(ST67W611_EVENT_READY, NULL);
    return result;
}

st67w611_status_t st67w611_stop(void)
{
    if (!lock_operation()) return ST67W611_ERR_BUSY;
    if (state == ST67W611_STATE_OFF) { unlock_operation(); return ST67W611_ERR_NOT_INITIALIZED; }
    if (scan_active) { unlock_operation(); return ST67W611_ERR_BUSY; }
    if (started) {
        (void)W6X_WiFi_Disconnect(0U);
        (void)st67w611_backend_stop();
        W6X_DeInit();
    }
    started = 0U;
    set_state(ST67W611_STATE_OFF, ST67W611_OK);
    unlock_operation();
    return ST67W611_OK;
}

st67w611_status_t st67w611_reset(void)
{
    st67w611_status_t result;
    int reset_result;
    if (!lock_operation()) return ST67W611_ERR_BUSY;
    if (state == ST67W611_STATE_OFF) { unlock_operation(); return ST67W611_ERR_NOT_INITIALIZED; }
    if (scan_active) { unlock_operation(); return ST67W611_ERR_BUSY; }
    diagnostics.reset_count++;
    set_state(ST67W611_STATE_RESETTING, ST67W611_OK);
    if (config.lifecycle != NULL &&
        config.lifecycle->before_transport_restart != NULL) {
        result = config.lifecycle->before_transport_restart(config.lifecycle->context);
        if (result != ST67W611_OK) {
            set_state(ST67W611_STATE_ERROR, result);
            unlock_operation();
            return result;
        }
    }
    if (started) {
        (void)W6X_WiFi_Disconnect(0U);
        (void)st67w611_backend_stop();
        W6X_DeInit();
        started = 0U;
    }
    if (config.port_ops != NULL && config.port_ops->hardware_reset != NULL) {
        reset_result = config.port_ops->hardware_reset(
            config.port_context, config.recovery.reset_timeout_ms);
        if (reset_result != 0) {
            diagnostics.transport_error_count++;
            set_state(ST67W611_STATE_ERROR, ST67W611_ERR_TRANSPORT);
            unlock_operation(); return ST67W611_ERR_TRANSPORT;
        }
    } else {
        set_state(ST67W611_STATE_ERROR, ST67W611_ERR_UNSUPPORTED);
        unlock_operation(); return ST67W611_ERR_UNSUPPORTED;
    }
    set_state(ST67W611_STATE_INITIALIZING, ST67W611_OK);
    memset(&vendor_callbacks, 0, sizeof(vendor_callbacks));
    vendor_callbacks.APP_wifi_cb = vendor_wifi_callback;
    vendor_callbacks.APP_error_cb = vendor_error_callback;
    if (W6X_RegisterAppCb(&vendor_callbacks) != W6X_STATUS_OK) {
        set_state(ST67W611_STATE_ERROR, ST67W611_ERR_VENDOR);
        unlock_operation(); return ST67W611_ERR_VENDOR;
    }
    result = start_locked();
    if (result == ST67W611_OK && config.lifecycle != NULL &&
        config.lifecycle->after_transport_restart != NULL) {
        result = config.lifecycle->after_transport_restart(config.lifecycle->context);
        if (result != ST67W611_OK) {
            set_state(ST67W611_STATE_ERROR, result);
        }
    }
    unlock_operation();
    if (result == ST67W611_OK) publish(ST67W611_EVENT_READY, NULL);
    return result;
}

st67w611_status_t st67w611_connect(const char *ssid, const char *password)
{
    W6X_WiFi_Connect_Opts_t options = {0};
    W6X_Status_t connect_status;
    uint8_t attempt;
    size_t len;
    if (!lock_operation()) return ST67W611_ERR_BUSY;
    if (ssid == NULL) { unlock_operation(); return ST67W611_ERR_INVALID_ARG; }
    if (state != ST67W611_STATE_READY) { unlock_operation(); return ST67W611_ERR_INVALID_STATE; }
    if (scan_active) { unlock_operation(); return ST67W611_ERR_BUSY; }
    len = bounded_strlen(ssid, sizeof(options.SSID));
    if (len >= sizeof(options.SSID)) { unlock_operation(); return ST67W611_ERR_INVALID_ARG; }
    memcpy(options.SSID, ssid, len);
    if (password != NULL) {
        len = bounded_strlen(password, sizeof(options.Password));
        if (len >= sizeof(options.Password)) { unlock_operation(); return ST67W611_ERR_INVALID_ARG; }
        memcpy(options.Password, password, len);
    }
    set_state(ST67W611_STATE_CONNECTING, ST67W611_OK);
    diagnostics.connect_count++;
    connect_status = W6X_WiFi_Connect(&options);
    for (attempt = 0U;
         connect_status != W6X_STATUS_OK &&
         attempt < config.recovery.max_transport_retries;
         ++attempt) {
        diagnostics.retry_count++;
        if (config.recovery.retry_delay_ms != 0U) {
            vTaskDelay(pdMS_TO_TICKS(config.recovery.retry_delay_ms));
        }
        connect_status = W6X_WiFi_Connect(&options);
    }
    if (connect_status != W6X_STATUS_OK) {
        diagnostics.vendor_error_count++;
        set_state(ST67W611_STATE_ERROR, map_status(connect_status));
        st67w611_status_t result = diagnostics.last_error;
        unlock_operation();
        publish(ST67W611_EVENT_ERROR, NULL);
        return result;
    }
    unlock_operation();
    return ST67W611_OK;
}

st67w611_status_t st67w611_disconnect(void)
{
    if (!lock_operation()) return ST67W611_ERR_BUSY;
    if (!started) { unlock_operation(); return ST67W611_ERR_NOT_READY; }
    if (scan_active) { unlock_operation(); return ST67W611_ERR_BUSY; }
    if (W6X_WiFi_Disconnect(0U) != W6X_STATUS_OK) { unlock_operation(); return ST67W611_ERR_VENDOR; }
    set_state(ST67W611_STATE_READY, ST67W611_OK);
    diagnostics.disconnect_count++;
    unlock_operation();
    return ST67W611_OK;
}

st67w611_status_t st67w611_get_state(st67w611_state_t *out_state)
{
    if (out_state == NULL) return ST67W611_ERR_INVALID_ARG;
    if (!lock_operation()) return ST67W611_ERR_BUSY;
    *out_state = state;
    unlock_operation();
    return ST67W611_OK;
}

st67w611_status_t st67w611_get_mac(uint8_t mac[6])
{
    if (mac == NULL) return ST67W611_ERR_INVALID_ARG;
    if (!started) return ST67W611_ERR_NOT_READY;
    return map_status(W6X_WiFi_Station_GetMACAddress(mac));
}

st67w611_status_t st67w611_get_rssi(int8_t *rssi)
{
    if (rssi == NULL) return ST67W611_ERR_INVALID_ARG;
    return ST67W611_ERR_UNSUPPORTED;
}

st67w611_status_t st67w611_scan(st67w611_scan_callback_t callback, void *user_context)
{
    W6X_WiFi_Scan_Opts_t options = {0};
    st67w611_status_t result;
    if (callback == NULL) return ST67W611_ERR_INVALID_ARG;
    if (!lock_operation()) return ST67W611_ERR_BUSY;
    if (!started) {
        unlock_operation();
        return ST67W611_ERR_NOT_READY;
    }
    if (scan_active) {
        unlock_operation();
        return ST67W611_ERR_BUSY;
    }
    scan_callback = callback;
    scan_user_context = user_context;
    scan_active = 1U;
    options.Scan_type = 0U;
    options.MaxCnt = 32U;
    result = map_status(W6X_WiFi_Scan(&options, vendor_scan_callback));
    if (result != ST67W611_OK) {
        scan_active = 0U;
        scan_callback = NULL;
        scan_user_context = NULL;
    }
    unlock_operation();
    return result;
}

st67w611_status_t st67w611_get_ip(uint8_t ip[4], uint8_t gateway[4], uint8_t netmask[4])
{
    if (ip == NULL || gateway == NULL || netmask == NULL) return ST67W611_ERR_INVALID_ARG;
    if (!started) return ST67W611_ERR_NOT_READY;
#if ST67_ARCH == 1
    return st67w611_backend_get_ip(ip, gateway, netmask);
#else
    if (config.capabilities == NULL || config.capabilities->get_ip == NULL) {
        return ST67W611_ERR_UNSUPPORTED;
    }
    return config.capabilities->get_ip(config.capabilities->context,
                                       ip, gateway, netmask);
#endif
}

st67w611_status_t st67w611_ping(const char *host, uint32_t timeout_ms)
{
    uint32_t average = 0U;
    uint16_t received = 0U;
    size_t length;
    if (host == NULL || *host == '\0' || timeout_ms > UINT16_MAX) return ST67W611_ERR_INVALID_ARG;
    if (!started) return ST67W611_ERR_NOT_READY;
    length = strlen(host);
    if (length > UINT16_MAX) return ST67W611_ERR_INVALID_ARG;
#if ST67_ARCH == 1
    (void)average; (void)received; (void)length;
    return st67w611_backend_ping(host, timeout_ms);
#else
    (void)average;
    (void)received;
    if (config.capabilities == NULL || config.capabilities->ping == NULL) {
        return ST67W611_ERR_UNSUPPORTED;
    }
    return config.capabilities->ping(config.capabilities->context,
                                     host, timeout_ms);
#endif
}

st67w611_status_t st67w611_get_diagnostics(st67w611_diagnostics_t *out)
{
    if (out == NULL) return ST67W611_ERR_INVALID_ARG;
    if (!lock_operation()) return ST67W611_ERR_BUSY;
    {
        st67w611_port_diagnostics_t port_diag;
        st67w611_port_get_diagnostics(&port_diag);
        diagnostics.spi_transfer_count = port_diag.transfer_count;
        diagnostics.spi_timeout_count = port_diag.timeout_count;
        diagnostics.spi_error_count = port_diag.error_count;
        diagnostics.dma_error_count = port_diag.dma_error_count;
        diagnostics.ready_rising_count = port_diag.ready_rising_count;
        diagnostics.ready_falling_count = port_diag.ready_falling_count;
    }
    diagnostics.state = state;
    *out = diagnostics;
    unlock_operation();
    return ST67W611_OK;
}

const char *st67w611_status_string(st67w611_status_t status)
{
    switch (status) {
    case ST67W611_OK: return "OK";
    case ST67W611_ERR_INVALID_ARG: return "INVALID_ARG";
    case ST67W611_ERR_NOT_INITIALIZED: return "NOT_INITIALIZED";
    case ST67W611_ERR_INVALID_STATE: return "INVALID_STATE";
    case ST67W611_ERR_NOT_READY: return "NOT_READY";
    case ST67W611_ERR_BUSY: return "BUSY";
    case ST67W611_ERR_TIMEOUT: return "TIMEOUT";
    case ST67W611_ERR_TRANSPORT: return "TRANSPORT";
    case ST67W611_ERR_VENDOR: return "VENDOR";
    case ST67W611_ERR_UNSUPPORTED: return "UNSUPPORTED";
    default: return "UNKNOWN";
    }
}

const char *st67w611_state_string(st67w611_state_t value)
{
    switch (value) {
    case ST67W611_STATE_OFF: return "OFF";
    case ST67W611_STATE_INITIALIZING: return "INITIALIZING";
    case ST67W611_STATE_READY: return "READY";
    case ST67W611_STATE_CONNECTING: return "CONNECTING";
    case ST67W611_STATE_CONNECTED: return "CONNECTED";
    case ST67W611_STATE_RESETTING: return "RESETTING";
    case ST67W611_STATE_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}
