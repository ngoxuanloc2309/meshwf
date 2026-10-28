#include "splc_port.h"
#include "splc_wifi_port.h"
#include "sx_mqtt.h"

#include <stdint.h>
#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"

#include "plc_engine.h"
#include "sx_adc.h"
#include "splc_opt.h"
#include "splc_log.h"
#include "logger.h"

#if SPLC_HOST_WIFI_TASK
#include "st67w611.h"
#include "st67w611_spi_port.h"
#endif

static const char *TAG = "SPLC_PORT";

/* Wait of the ST67 task loop. With MQTT on, the same loop sends the queued publishes (one per
 * pass), so it runs faster; the Wi-Fi worker works off timestamps, not pass counts. */
#ifndef SPLC_HOST_ST67_LOOP_MS
#if SPLC_MQTT_ENABLE
#define SPLC_HOST_ST67_LOOP_MS   50U
#else
#define SPLC_HOST_ST67_LOOP_MS   250U
#endif
#endif

/* ------------------------------------------------------------------------ */
/* Start hook (see splc_port.h)                                              */
/* ------------------------------------------------------------------------ */

extern void MX_FREERTOS_Init(void);
extern void __real_MX_FREERTOS_Init(void);

void __wrap_MX_FREERTOS_Init(void)
{
    __real_MX_FREERTOS_Init();
    (void)splc_host_start();
}

/* ------------------------------------------------------------------------ */
/* No ADC on this board (0 AI, no hadc1 in CubeMX): SPLC_ADC_BUILTIN = 0      */
/* makes the library expect the host to supply these two.                    */
/* ------------------------------------------------------------------------ */

void sx_adc_init(sx_adc_config_t *config, sx_adc_resolution_t resolution)
{
    (void)config;
    (void)resolution;
}

uint32_t sx_adc_read(sx_adc_config_t *config)
{
    (void)config;
    return 0U;
}

/* ------------------------------------------------------------------------ */
/* Engine task                                                               */
/* ------------------------------------------------------------------------ */

static void splc_task(void *arg)
{
    (void)arg;

    sx_mqtt_init();      /* register the publish callback BEFORE the engine starts (plc_mqtt.h) */
    plc_engine_init();   /* board_hw_init() runs inside: log UART, DI/DO, RS485 link */
    splc_wifi_port_init();   /* register the Wi-Fi ops (0x1300 block); the ST67 task does the work */

    for (;;) {
        plc_engine_poll();
        sx_mqtt_pump((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}

/* ------------------------------------------------------------------------ */
/* Wi-Fi bring-up task                                                       */
/* ------------------------------------------------------------------------ */

#if SPLC_HOST_WIFI_TASK
static void st67_event_cb(st67w611_event_t event, void *event_data, void *user_context)
{
    (void)user_context;
    /* Runs in the vendor callback context: no blocking, no logging of long text.
     * The Wi-Fi glue only copies a few values and wakes this task. */
    splc_wifi_port_on_event(event, event_data);
}

/* The module restarted (soft reset / reset): its MQTT session is gone. The hook runs under the
 * facade's lock, so it only sets a flag. */
static st67w611_status_t st67_after_restart(void *context)
{
    (void)context;
    sx_mqtt_notify_module_reset();
    return ST67W611_OK;
}

static void wifi_task(void *arg)
{
    (void)arg;

    static const st67w611_lifecycle_hooks_t hooks = {
        .before_transport_restart = NULL,
        .after_transport_restart  = st67_after_restart,
        .context                  = NULL,
    };

    static const st67w611_config_t cfg = {
        .port_ops     = &meshwf_st67_port_ops,
        .port_context = NULL,
        .event_cb     = st67_event_cb,
        .user_context = NULL,
        .capabilities = NULL,
        .lifecycle    = &hooks,
        .recovery     = {
            .max_transport_retries = 2U,
            .retry_delay_ms        = 100U,
            .reset_timeout_ms      = 3000U,
        },
    };

    st67w611_status_t st = st67w611_init(&cfg);
    if (st == ST67W611_OK) {
        st = st67w611_start();
    }
    if (st != ST67W611_OK) {
        log_error(TAG, "ST67 bring-up failed: %s", st67w611_status_string(st));
    } else {
        log_info(TAG, "ST67 READY");
        splc_wifi_port_set_ready(true);   /* from now on connect / scan are accepted */
    }

    /* This task is also the worker of the SimplePLC Wi-Fi block: connect, scan and
     * IP polling run in splc_wifi_port_service() (no extra task: the FreeRTOS heap
     * is only configTOTAL_HEAP_SIZE). Bring-up aid: report heap and stack headroom. */
#if SPLC_HOST_HEAP_LOG_MS
    TickType_t next_log = xTaskGetTickCount();
#endif
    for (;;) {
        splc_wifi_port_service(SPLC_HOST_ST67_LOOP_MS);
        if (st == ST67W611_OK) {   /* MQTT client of the module: only after the bring-up */
            sx_mqtt_service((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                            splc_wifi_port_has_ip());
        }
#if !SPLC_WIFI_ENABLE
        vTaskDelay(pdMS_TO_TICKS(250U));   /* the service call is a no-op without the Wi-Fi block */
#endif
#if SPLC_HOST_HEAP_LOG_MS
        if ((int32_t)(xTaskGetTickCount() - next_log) >= 0) {
            log_info(TAG, "heap free=%u min_ever=%u, ST67 stack min free=%u words",
                     (unsigned)xPortGetFreeHeapSize(),
                     (unsigned)xPortGetMinimumEverFreeHeapSize(),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            next_log = xTaskGetTickCount() + pdMS_TO_TICKS(SPLC_HOST_HEAP_LOG_MS);
        }
#endif
    }
}
#endif /* SPLC_HOST_WIFI_TASK */

bool splc_host_start(void)
{
    static bool started;
    if (started) {
        return false;
    }

    if (xTaskCreate(splc_task, "SPLC", (configSTACK_DEPTH_TYPE)SPLC_HOST_TASK_STACK_WORDS,
                    NULL, SPLC_HOST_TASK_PRIO, NULL) != pdPASS) {
        return false;   /* logger is not up yet (board_hw_init runs in the task) */
    }

#if SPLC_HOST_WIFI_TASK
    if (xTaskCreate(wifi_task, "ST67", (configSTACK_DEPTH_TYPE)SPLC_HOST_WIFI_STACK_WORDS,
                    NULL, SPLC_HOST_WIFI_PRIO, NULL) != pdPASS) {
        return false;
    }
#endif

    started = true;
    return true;
}