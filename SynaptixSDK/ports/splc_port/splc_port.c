#include "splc_port.h"

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

    plc_engine_init();   /* board_hw_init() runs inside: log UART, DI/DO, RS485 link */

    for (;;) {
        plc_engine_poll();
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}

/* ------------------------------------------------------------------------ */
/* Wi-Fi bring-up task                                                       */
/* ------------------------------------------------------------------------ */

#if SPLC_HOST_WIFI_TASK
static void st67_event_cb(st67w611_event_t event, void *event_data, void *user_context)
{
    (void)event_data;
    (void)user_context;
    /* Runs in the vendor callback context: no blocking, no logging of long text. */
    (void)event;
}

static void wifi_task(void *arg)
{
    (void)arg;

    static const st67w611_config_t cfg = {
        .port_ops     = &meshwf_st67_port_ops,
        .port_context = NULL,
        .event_cb     = st67_event_cb,
        .user_context = NULL,
        .capabilities = NULL,
        .lifecycle    = NULL,
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
    }

    /* Wi-Fi ops of SimplePLC (connect/disconnect/status/scan/load/save) are
     * the next step; until then the task only keeps the module alive and
     * (bring-up aid) reports heap and stack headroom, because CubeMX gives the
     * FreeRTOS heap only configTOTAL_HEAP_SIZE bytes for every task + vendor
     * buffers. */
    for (;;) {
#if SPLC_HOST_HEAP_LOG_MS
        log_info(TAG, "heap free=%u min_ever=%u, ST67 stack min free=%u words",
                 (unsigned)xPortGetFreeHeapSize(),
                 (unsigned)xPortGetMinimumEverFreeHeapSize(),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(pdMS_TO_TICKS(SPLC_HOST_HEAP_LOG_MS));
#else
        vTaskDelay(pdMS_TO_TICKS(1000U));
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