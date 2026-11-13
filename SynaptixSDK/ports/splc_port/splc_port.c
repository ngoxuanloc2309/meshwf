#include "splc_port.h"
#include "splc_wifi_port.h"
#include "splc_rtc_port.h"
#include "splc_indicators_port.h"
#include "splc_boot_button_port.h"   /* PA0 held 7 s -> system bootloader (DFU) */
#include "sx_mqtt.h"
#include "st67_modbus_tcp.h"   /* Modbus TCP slave over the ST67 sockets */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "plc_engine.h"
#include "plc_modbus_cfg.h"
#include "plc_systag.h"        /* System Tags 1000..1007 / Modbus 0x0820: SYS_COMM_OK source */
#include "sx_adc.h"
#include "splc_opt.h"
#if SPLC_MQTT_ENABLE
#include "st67_mqtt.h"         /* st67_mqtt_is_connected() for SYS_COMM_OK source 2 */
#endif
#include "splc_log.h"
#include "logger.h"

#if SPLC_HOST_WIFI_TASK
#include "st67w611.h"
#include "st67w611_spi_port.h"
#endif

static const char *TAG = "SPLC_PORT";

#if SPLC_HOST_WIFI_TASK
/* st67w611_ready_recover.c (libs/st67w611/port/src) */
extern volatile uint32_t g_st67_ready_recover_count;
extern volatile uint32_t g_st67_ready_recover_result;
extern volatile uint32_t g_st67_ready_recover_rounds;
#endif

/* ------------------------------------------------------------------------ */
/* configASSERT capture (see Core/Inc/FreeRTOSConfig.h, USER CODE 1)           */
/* Plain globals, no logging: the failing assert may itself be in the log      */
/* queue path. After a hang: nm | findstr g_splc_assert, then -r32 <addr> 4    */
/* gives {magic, file ptr, line, lr}; read the file string with -r8 <ptr> 64.  */
/* ------------------------------------------------------------------------ */

typedef struct {
    volatile uint32_t magic;       /* 0xA55E7700 once an assert has fired */
    const char * volatile file;
    volatile int32_t  line;
    volatile uint32_t lr;          /* return address of the failing function */
} splc_assert_info_t;

splc_assert_info_t g_splc_assert;

void splc_assert_failed(const char *file, int line)
{
    taskDISABLE_INTERRUPTS();
    g_splc_assert.file  = file;
    g_splc_assert.line  = (int32_t)line;
    g_splc_assert.lr    = (uint32_t)(uintptr_t)__builtin_return_address(0);
    g_splc_assert.magic = 0xA55E7700U;
    for (;;) {
    }
}

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
/* System Tags: SYS_COMM_OK source (tag 1007 / Modbus 0x0827)                   */
/* SPLC_SYSTAG_COMM_SRC: 0 = none registered, SYS_COMM_OK stays 1 (as before)  */
/*                       1 = Wi-Fi has an IP address                            */
/*                       2 = MQTT client of the module is connected             */
/* PENDING the App team's answer: the meaning of SYS_COMM_OK is not decided.   */
/* Both getters are safe to call from the SPLC task (critical section / header */
/* says st67_mqtt_is_connected() may be called from any task) and do not block.*/
/* ------------------------------------------------------------------------ */

#ifndef SPLC_SYSTAG_COMM_SRC
#define SPLC_SYSTAG_COMM_SRC   0
#endif

#if SPLC_SYSTAG_ENABLE && (SPLC_SYSTAG_COMM_SRC == 1)
static bool systag_comm_ok(void)
{
    return splc_wifi_port_has_ip();
}
#elif SPLC_SYSTAG_ENABLE && (SPLC_SYSTAG_COMM_SRC == 2)
#if !SPLC_MQTT_ENABLE
#error "SPLC_SYSTAG_COMM_SRC 2 needs SPLC_MQTT_ENABLE = 1"
#endif
static bool systag_comm_ok(void)
{
    return st67_mqtt_is_connected();
}
#endif

/* ------------------------------------------------------------------------ */
/* Engine task                                                               */
/* ------------------------------------------------------------------------ */

static void splc_task(void *arg)
{
    (void)arg;

    sx_mqtt_init();      /* register the publish callback BEFORE the engine starts (plc_mqtt.h) */
    plc_engine_init();   /* board_hw_init() runs inside: log UART, DI/DO, RS485 link */
#if SPLC_SYSTAG_ENABLE && (SPLC_SYSTAG_COMM_SRC != 0)
    plc_systag_set_comm_ok_fn(systag_comm_ok);   /* before the first scan; without it SYS_COMM_OK = 1 */
    log_info(TAG, "System Tags: SYS_COMM_OK source = %d (1 = Wi-Fi IP, 2 = MQTT connected)",
             (int)SPLC_SYSTAG_COMM_SRC);
#elif SPLC_SYSTAG_ENABLE
    log_info(TAG, "System Tags: no SYS_COMM_OK source registered, SYS_COMM_OK = 1");
#endif
    splc_rtc_port_init();    /* external RTC (I2C1): chip init + plc_rtc hooks, before the first scan */
    splc_wifi_port_init();   /* register the Wi-Fi ops (0x1300 block); the ST67 task does the work */
    splc_indicators_port_init();   /* 3 LEDs; the library closes its table at the first scan */

    /* Modbus TCP slave: second App-link transport next to USB. Must be called from this task,
     * after plc_engine_init() (plc_modbus_cfg is single-threaded). The two MBTCP tasks wait for
     * a Wi-Fi IP, so nothing happens on the network until the ST67 task has connected. */
    {
        modbus_transport_t tcp_tr;
        if (!st67_modbus_tcp_create(&tcp_tr, splc_wifi_port_has_ip)) {
            log_error(TAG, "Modbus TCP: create failed, TCP slave is off");
        } else if (!plc_modbus_cfg_add_transport(&tcp_tr)) {
            log_error(TAG, "Modbus TCP: plc_modbus_cfg_add_transport failed");
        } else {
            log_info(TAG, "Modbus TCP slave registered as App-link transport");
        }
    }

    for (;;) {
        plc_engine_poll();
        sx_mqtt_pump((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        splc_boot_button_poll((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}

/* ------------------------------------------------------------------------ */
/* Wi-Fi bring-up task                                                       */
/* ------------------------------------------------------------------------ */

#if SPLC_HOST_WIFI_TASK

/* ---- bring-up diagnosis without a debugger -----------------------------------------
 * wifi_task records where it is (s_st67_stage) and ST67dbg, a low priority task, prints
 * it once a second together with: pins, RDY edge counters, SPI transfer counters, the
 * vendor SPI engine counters and the state / stack headroom of the tasks involved.
 * It does not touch the driver and never blocks on it, so it keeps reporting while
 * the bring-up task is stuck inside the vendor code. */
#ifndef SPLC_HOST_ST67_DIAG
#define SPLC_HOST_ST67_DIAG        1
#endif
/* Above every ST67 / SPI task (Modem_Process 8, spi_xfer_engine 7) so that a task stuck in a
 * loop cannot starve it: if the DIAG lines keep coming, nothing is spinning at a higher
 * priority and a task showing READY with growing cpu= is the spinner. If they stop too,
 * interrupts are off (assert) or the core faulted. */
#ifndef SPLC_HOST_ST67_DIAG_PRIO
#define SPLC_HOST_ST67_DIAG_PRIO   20U
#endif
#ifndef SPLC_HOST_ST67_DIAG_MS
#define SPLC_HOST_ST67_DIAG_MS     1000U
#endif

enum {
    ST67_STAGE_IDLE = 0,
    ST67_STAGE_PROBE,      /* waiting for the module on RDY */
    ST67_STAGE_INIT,       /* st67w611_init() */
    ST67_STAGE_START,      /* st67w611_start(): W6X_Init + Wi-Fi init (blocking) */
    ST67_STAGE_READY,
    ST67_STAGE_FAILED,
};
static volatile uint8_t  s_st67_stage;
static volatile int32_t  s_st67_last_status;
static volatile uint32_t s_st67_start_t0;     /* ms tick when st67w611_start() was called */
static volatile uint32_t s_st67_start_ms;     /* how long it took (0 = still running) */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

#if SPLC_HOST_ST67_DIAG
static const char *task_state_str(eTaskState st)
{
    switch (st) {
    case eRunning:   return "RUN";
    case eReady:     return "READY";
    case eBlocked:   return "BLOCKED";
    case eSuspended: return "SUSP";
    case eDeleted:   return "DEL";
    default:         return "?";
    }
}

static void st67_diag_task(void *arg)
{
    (void)arg;
    static TaskStatus_t ts[24];
    static const char *const watch[] = { "Modem_Process", "spi_xfer_engine", "ST67", "SPLC" };
    uint32_t after_end = 0U;

    for (;;) {
        meshwf_st67_pins_t pins;
        meshwf_st67_diag_t d;
        memset(&d, 0, sizeof(d));
        meshwf_st67_port_get_pins(&pins);
        meshwf_st67_port_get_diag(&d);

        log_debug(TAG, "DIAG t=%u stage=%u st=%d start_t0=%u start_ms=%u | pins rdy=%d en=%d boot=%d cs=%d",
                 (unsigned)now_ms(), (unsigned)s_st67_stage, (int)s_st67_last_status,
                 (unsigned)s_st67_start_t0, (unsigned)s_st67_start_ms,
                 (int)pins.rdy, (int)pins.en, (int)pins.boot, (int)pins.cs);
        log_debug(TAG, "DIAG port edge r=%u f=%u xfer=%u to=%u err=%u en_rel=%u rise1=%u gate=%u wait=%u thr=%u where=%u len=%u",
                 (unsigned)d.rdy_rising, (unsigned)d.rdy_falling, (unsigned)d.transfers,
                 (unsigned)d.timeouts, (unsigned)d.errors,
                 (unsigned)d.en_release_tick, (unsigned)d.first_rise_tick,
                 (unsigned)d.gate_open_tick, (unsigned)d.gate_wait_ms,
                 (unsigned)d.throttled, (unsigned)d.where, (unsigned)d.last_len);
        if (d.vendor_ok != 0U) {
            log_debug(TAG, "DIAG vspi tx=%u rx=%u DROP=%u ioerr=%u hdr=%u txn_to=%u ack_to=%u mem=%u",
                     (unsigned)d.v_tx_pkts, (unsigned)d.v_rx_pkts, (unsigned)d.v_rx_drop,
                     (unsigned)d.v_io_err, (unsigned)d.v_hdr_err, (unsigned)d.v_wait_txn_to,
                     (unsigned)d.v_wait_ack_to, (unsigned)d.v_mem_err);
        }

        UBaseType_t n = uxTaskGetSystemState(ts, (UBaseType_t)(sizeof(ts) / sizeof(ts[0])), NULL);
        for (size_t w = 0U; w < sizeof(watch) / sizeof(watch[0]); w++) {
            const char *state = "absent";
            unsigned hwm = 0U;
            unsigned cpu = 0U;
            for (UBaseType_t i = 0U; i < n; i++) {
                if (strcmp(ts[i].pcTaskName, watch[w]) == 0) {
                    state = task_state_str(ts[i].eCurrentState);
                    hwm = (unsigned)ts[i].usStackHighWaterMark;
                    cpu = (unsigned)ts[i].ulRunTimeCounter;
                    break;
                }
            }
            log_debug(TAG, "DIAG task %s: %s, stack min free=%u words, cpu=%u", watch[w], state, hwm, cpu);
        }
        log_debug(TAG, "DIAG heap free=%u min_ever=%u",
                 (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize());

        /* Report every second during the bring-up, then back off once it is over. */
        uint32_t wait_ms = SPLC_HOST_ST67_DIAG_MS;
        if (s_st67_stage == ST67_STAGE_READY || s_st67_stage == ST67_STAGE_FAILED) {
            if (++after_end > 3U) {
                wait_ms = 30000U;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}
#endif /* SPLC_HOST_ST67_DIAG */

/* Vendor driver log -> the host logger, debug level. Runs in the vendor OutputTask.
 * TEMPORARY diagnostic: the raw AT traffic ("AT> command" / "AT< reply", W61_AT_LOG_ENABLE 1) is
 * raised to info so it is visible at SPLC_LOG_LEVEL 3. NOTE: this prints AT+MQTTUSERCFG and
 * AT+CWJAP IN CLEAR TEXT, including the MQTT password and the Wi-Fi password. Revert to log_debug
 * when done. */
static void st67_vendor_log_out(const char *message)
{
    if (strncmp(message, "AT", 2) == 0) {
        log_debug(TAG, "vendor: %s", message);
        return;
    }
    log_debug(TAG, "vendor: %s", message);
}

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

    /* Presence gate: with no module fitted (RDY floating) the vendor driver must not be
     * started: the board keeps running (USB link, rules, RS485) without Wi-Fi. The probe is
     * repeated every 5 s, so a module that is still booting, or fitted later, is picked up. */
    bool absent_logged = false;
    s_st67_stage = ST67_STAGE_PROBE;
    while (!meshwf_st67_module_present()) {
        if (!absent_logged) {
            log_warn(TAG, "ST67 module not detected on RDY: Wi-Fi/MQTT off, re-probing every 5 s");
            absent_logged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(5000U));
    }
    if (absent_logged) {
        log_info(TAG, "ST67 module detected, starting the driver");
    }

    /* Vendor logging must be set up before the driver runs: its log queue is NULL until then. */
    if (st67w611_vendor_log_init(st67_vendor_log_out) != ST67W611_OK) {
        log_error(TAG, "ST67 vendor log init failed (vendor messages will be lost)");
    }

    s_st67_stage = ST67_STAGE_INIT;
    st67w611_status_t st = st67w611_init(&cfg);
    if (st == ST67W611_OK) {
        s_st67_start_t0 = now_ms();
        s_st67_stage = ST67_STAGE_START;
        st = st67w611_start();
        s_st67_start_ms = now_ms() - s_st67_start_t0;
        log_info(TAG, "st67w611_start returned %d after %u ms", (int)st, (unsigned)s_st67_start_ms);
        log_info(TAG, "ready_recover: ran %u time(s), last result %u (1 ok, 2 AT+RST failed, 3 no ready, "
                      "4 AT answers), rounds %u",
                 (unsigned)g_st67_ready_recover_count, (unsigned)g_st67_ready_recover_result,
                 (unsigned)g_st67_ready_recover_rounds);
    }
    s_st67_last_status = (int32_t)st;
    if (st != ST67W611_OK) {
        s_st67_stage = ST67_STAGE_FAILED;
        log_error(TAG, "ST67 bring-up failed: %s", st67w611_status_string(st));
    } else {
        s_st67_stage = ST67_STAGE_READY;
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
#if SPLC_HOST_ST67_DIAG
    (void)xTaskCreate(st67_diag_task, "ST67dbg", (configSTACK_DEPTH_TYPE)768U, NULL,
                       SPLC_HOST_ST67_DIAG_PRIO, NULL);
#endif
#endif

    started = true;
    return true;
}