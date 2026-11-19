/*
 * splc_wdt_port.c - see splc_wdt_port.h and docs/SPLC_Watchdog_Design.md.
 *
 * Needs, in CubeMX: IWDG activated (software start), window = maximum (no window mode), handle
 * `hiwdg` (Core/Inc/iwdg.h). Without that the file does not build with SPLC_WDG_ENABLE = 1.
 */
#include "splc_wdt_port.h"

#if SPLC_WDG_ENABLE

#include "main.h"
#include "iwdg.h"          /* CubeMX: extern IWDG_HandleTypeDef hiwdg; */

#include "FreeRTOS.h"
#include "task.h"

#include "splc_log.h"
#include "splc_port.h"      /* SPLC_HOST_WIFI_TASK */

static const char *TAG = "WDT_PORT";

/* What the supervisor reported last (read with a debugger; the log line is the normal way). */
static volatile const char *s_fault_name;
static volatile uint32_t    s_fault_late_ms;

/* The only place that feeds the hardware watchdog. Called from plc_wdt_service() only. */
static void wdt_kick(void)
{
    (void)HAL_IWDG_Refresh(&hiwdg);
}

/* plc_wdt_service() has already logged the cause. Nothing survives the reset yet (decision on
 * .noinit / TAMP is open in the design document), so keep it in RAM for a debugger. */
static void wdt_fault(const char *name, uint32_t late_ms)
{
    s_fault_name    = name;
    s_fault_late_ms = late_ms;
}

static void wdt_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SPLC_WDG_SERVICE_MS));
        plc_wdt_service();
    }
}

void splc_wdt_port_init(void)
{
#if SPLC_WDG_FREEZE_IN_DEBUG
    __HAL_DBGMCU_FREEZE_IWDG();   /* counter stops while the debugger halts the core */
#endif
    plc_wdt_set_kick_fn(wdt_kick);
    plc_wdt_set_fault_fn(wdt_fault);
    /* Channel PLC is registered by plc_engine_init(). Channel NET starts here so that the ST67
     * task, which is created right after, is covered from its first instruction. */
#if SPLC_HOST_WIFI_TASK
    (void)plc_wdt_register(SPLC_WDT_CH_NET, "NET", SPLC_WDG_NET_TIMEOUT_MS, SPLC_WDG_BOOT_GRACE_MS);
#endif
}

bool splc_wdt_port_start_task(void)
{
    return xTaskCreate(wdt_task, "WDT", (configSTACK_DEPTH_TYPE)SPLC_WDG_TASK_STACK_WORDS, NULL,
                       SPLC_WDG_TASK_PRIO, NULL) == pdPASS;
}

void splc_wdt_port_boot_report(void)
{
    /* RCC reset flags. BOR is also set after a power-up. */
    log_info(TAG, "reset cause: IWDG=%d WWDG=%d SOFT=%d PIN=%d BOR=%d LPWR=%d",
             (int)(__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != 0U),
             (int)(__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST) != 0U),
             (int)(__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST) != 0U),
             (int)(__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST) != 0U),
             (int)(__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST) != 0U),
             (int)(__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST) != 0U));
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != 0U) {
        log_warn(TAG, "the previous run ended with a WATCHDOG RESET");
    }
    __HAL_RCC_CLEAR_RESET_FLAGS();

    /* IWDG timeout from the CubeMX values: (reload + 1) * divider / LSI. The prescaler field
     * holds the register code 0..8 = /4 .. /1024, so divider = 4 << code. LSI_VALUE is the
     * typical 32 kHz; the real LSI spread is wide, so this is an estimate. */
    const uint32_t divider = 4UL << hiwdg.Init.Prescaler;
    const uint32_t est_ms  = (uint32_t)(((uint64_t)(hiwdg.Init.Reload + 1UL) * divider * 1000ULL) / LSI_VALUE);
    log_debug(TAG, "IWDG: prescaler /%u reload %u window 0x%X -> about %u ms (design value %u ms)",
             (unsigned)divider, (unsigned)hiwdg.Init.Reload, (unsigned)hiwdg.Init.Window,
             (unsigned)est_ms, (unsigned)SPLC_WDG_HW_TIMEOUT_MS);
    if (hiwdg.Init.Window != IWDG_WINDOW_DISABLE) {
        log_error(TAG, "IWDG window mode is on: a kick before the window opens resets the board. "
                       "Set the window to the maximum in CubeMX");
    }
    if ((est_ms * 5UL < (uint32_t)SPLC_WDG_HW_TIMEOUT_MS * 4UL) ||
        (est_ms * 4UL > (uint32_t)SPLC_WDG_HW_TIMEOUT_MS * 5UL)) {
        log_warn(TAG, "IWDG timeout differs by more than about 20 percent from SPLC_WDG_HW_TIMEOUT_MS: "
                      "fix the CubeMX values or the option");
    }
    log_info(TAG, "supervisor: service %u ms, channel PLC %u ms, channel NET %u ms, boot grace %u ms",
             (unsigned)SPLC_WDG_SERVICE_MS, (unsigned)SPLC_WDG_PLC_TIMEOUT_MS,
             (unsigned)SPLC_WDG_NET_TIMEOUT_MS, (unsigned)SPLC_WDG_BOOT_GRACE_MS);
}

#endif /* SPLC_WDG_ENABLE */