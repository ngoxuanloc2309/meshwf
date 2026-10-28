#ifndef ST67W611_H
#define ST67W611_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ST67W611_OK = 0,
    ST67W611_ERR_INVALID_ARG = -1,
    ST67W611_ERR_NOT_INITIALIZED = -2,
    ST67W611_ERR_INVALID_STATE = -3,
    ST67W611_ERR_NOT_READY = -4,
    ST67W611_ERR_BUSY = -5,
    ST67W611_ERR_TIMEOUT = -6,
    ST67W611_ERR_TRANSPORT = -7,
    ST67W611_ERR_VENDOR = -8,
    ST67W611_ERR_UNSUPPORTED = -9
} st67w611_status_t;

typedef enum {
    ST67W611_STATE_OFF = 0,
    ST67W611_STATE_INITIALIZING,
    ST67W611_STATE_READY,
    ST67W611_STATE_CONNECTING,
    ST67W611_STATE_CONNECTED,
    ST67W611_STATE_RESETTING,
    ST67W611_STATE_ERROR
} st67w611_state_t;

typedef enum {
    ST67W611_EVENT_READY = 0,
    ST67W611_EVENT_CONNECTING,
    ST67W611_EVENT_CONNECTED,
    ST67W611_EVENT_GOT_IP,
    ST67W611_EVENT_DISCONNECTED,
    ST67W611_EVENT_ERROR
} st67w611_event_t;

typedef struct {
    uint8_t max_transport_retries;
    uint32_t retry_delay_ms;
    uint32_t reset_timeout_ms;
} st67w611_recovery_config_t;

typedef void (*st67w611_transfer_complete_cb_t)(void *context, int status);

typedef struct {
    uint32_t transfer_count;
    uint32_t timeout_count;
    uint32_t error_count;
    uint32_t dma_error_count;
    uint32_t ready_rising_count;
    uint32_t ready_falling_count;
} st67w611_port_diagnostics_t;

typedef struct {
    int (*init)(void *context,
                st67w611_transfer_complete_cb_t complete_cb,
                void *complete_context);
    int (*deinit)(void *context);
    int (*transfer)(void *context, const void *tx, void *rx,
                    uint16_t length, uint32_t timeout_ms);
    int (*transfer_dma)(void *context, const void *tx, void *rx,
                        uint16_t length);
    int (*cancel_transfer)(void *context);
    int (*set_cs)(void *context, bool asserted);
    int (*set_enable)(void *context, bool enabled);
    int (*set_boot)(void *context, bool enabled);
    int (*set_wakeup)(void *context, bool enabled);
    bool (*read_ready)(void *context);
    int (*hardware_reset)(void *context, uint32_t timeout_ms);
    void (*delay_ms)(void *context, uint32_t delay_ms);
    int (*get_diagnostics)(void *context,
                           st67w611_port_diagnostics_t *diagnostics);
} st67w611_port_ops_t;

typedef struct {
    st67w611_state_t state;
    st67w611_status_t last_error;
    uint32_t init_count;
    uint32_t reset_count;
    uint32_t connect_count;
    uint32_t disconnect_count;
    uint32_t spi_transfer_count;
    uint32_t spi_timeout_count;
    uint32_t spi_error_count;
    uint32_t dma_error_count;
    uint32_t ready_rising_count;
    uint32_t ready_falling_count;
    uint32_t retry_count;
    uint32_t state_transition_count;
    uint32_t callback_count;
    uint32_t vendor_error_count;
    uint32_t timeout_count;
    uint32_t transport_error_count;
} st67w611_diagnostics_t;

typedef void (*st67w611_event_cb_t)(st67w611_event_t event,
                                    void *event_data,
                                    void *user_context);

typedef st67w611_status_t (*st67w611_get_ip_cb_t)(void *context,
                                                  uint8_t ip[4],
                                                  uint8_t gateway[4],
                                                  uint8_t netmask[4]);
typedef st67w611_status_t (*st67w611_ping_cb_t)(void *context,
                                                const char *host,
                                                uint32_t timeout_ms);

typedef struct {
    st67w611_get_ip_cb_t get_ip;
    st67w611_ping_cb_t ping;
    void *context;
} st67w611_capabilities_t;

typedef st67w611_status_t (*st67w611_lifecycle_cb_t)(void *context);

typedef struct {
    st67w611_lifecycle_cb_t before_transport_restart;
    st67w611_lifecycle_cb_t after_transport_restart;
    void *context;
} st67w611_lifecycle_hooks_t;

typedef struct {
    const st67w611_port_ops_t *port_ops;
    void *port_context;
    st67w611_event_cb_t event_cb;
    void *user_context;
    const st67w611_capabilities_t *capabilities;
    const st67w611_lifecycle_hooks_t *lifecycle;
    st67w611_recovery_config_t recovery;
} st67w611_config_t;

typedef struct {
    char ssid[33];
    int16_t rssi;
    uint8_t mac[6];
    uint8_t channel;
} st67w611_scan_result_t;

typedef void (*st67w611_scan_callback_t)(st67w611_status_t status,
                                         const st67w611_scan_result_t *result,
                                         void *user_context);

st67w611_status_t st67w611_init(const st67w611_config_t *config);
st67w611_status_t st67w611_start(void);
st67w611_status_t st67w611_stop(void);
st67w611_status_t st67w611_reset(void);
st67w611_status_t st67w611_connect(const char *ssid, const char *password);
st67w611_status_t st67w611_disconnect(void);
st67w611_status_t st67w611_get_state(st67w611_state_t *state);
st67w611_status_t st67w611_get_mac(uint8_t mac[6]);
st67w611_status_t st67w611_get_rssi(int8_t *rssi);
st67w611_status_t st67w611_scan(st67w611_scan_callback_t callback,
                                void *user_context);
st67w611_status_t st67w611_get_ip(uint8_t ip[4], uint8_t gateway[4],
                                  uint8_t netmask[4]);
st67w611_status_t st67w611_ping(const char *host, uint32_t timeout_ms);
st67w611_status_t st67w611_get_diagnostics(st67w611_diagnostics_t *diagnostics);
const char *st67w611_status_string(st67w611_status_t status);
const char *st67w611_state_string(st67w611_state_t state);

#ifdef __cplusplus
}
#endif

#endif
