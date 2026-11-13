/*
 * st67w611_ready_recover.c - recover a missing "ready" banner WITHOUT the EN pin
 * and WITHOUT touching vendor code.
 *
 * Problem: W61_AT_ModemInit() (vendor w61_at_common.c) needs the one-shot "ready"
 * message the module sends after it boots. If the MCU is reset while the module
 * keeps its power, that message was consumed by the previous run and never comes
 * again, so W61_WaitForReady() times out and W6X_Init() fails.
 *
 * Fix: the linker option -Wl,--wrap=W61_WaitForReady (app/CMakeLists.txt) routes
 * every call that crosses a translation unit to __wrap_W61_WaitForReady() below.
 * The only such caller is W61_AT_ModemInit() (w61_at_common.c -> w61_at_sys.c).
 * Calls inside w61_at_sys.c itself (W61_Reset, W61_SetClockSource) are NOT wrapped,
 * so AT+RST done by W61_Reset() is never retried twice.
 *
 * On timeout: send AT+RST (the same call W61_Reset() uses), then wait again.
 * If the module does not answer AT+RST either, the original error is returned.
 *
 * Observed on the board (log of a failed bring-up): AT+RST is answered "OK" but the "ready"
 * banner never comes in the second wait (st67w611_start() fails with -8 after ~8 s); pressing
 * reset again then works, i.e. the same recovery succeeds on another try. So the recovery is
 * repeated up to ST67_READY_RECOVER_ROUNDS times, and after a round without "ready" the module is
 * probed with a plain "AT": if it answers, the banner was lost, not the module, and the bring-up
 * goes on (result 4). Worst case adds (ROUNDS - 1) * time_ms to the start.
 *
 * Logs go through the vendor SYS_LOG_* macros; the port forwards them to log_debug
 * ("vendor: ..." in splc_port.c, via st67w611_vendor_log_init()).
 */
#include <stdint.h>
#include <stddef.h>

#include "FreeRTOS.h"
#include "task.h"
#include "w61_at_api.h"
#include "w61_at_common.h"

extern W61_Status_t __real_W61_WaitForReady(W61_Object_t *Obj, uint32_t time_ms);

/* AT+RST rounds before giving up (each: AT+RST, then one wait of time_ms). */
#ifndef ST67_READY_RECOVER_ROUNDS
#define ST67_READY_RECOVER_ROUNDS 3U
#endif

/* Number of times the AT+RST recovery ran (read it from a debugger / diag). */
volatile uint32_t g_st67_ready_recover_count;
/* Last result: 0 = never ran, 1 = recovered (ready banner), 2 = AT+RST failed,
 * 3 = still no ready after all rounds, 4 = no banner but the module answers AT (continued). */
volatile uint32_t g_st67_ready_recover_result;
/* Rounds used by the last recovery (1..ST67_READY_RECOVER_ROUNDS). */
volatile uint32_t g_st67_ready_recover_rounds;

W61_Status_t __wrap_W61_WaitForReady(W61_Object_t *Obj, uint32_t time_ms)
{
    W61_Status_t ret = __real_W61_WaitForReady(Obj, time_ms);

    if (ret == W61_STATUS_OK || Obj == NULL) {
        return ret;
    }

    g_st67_ready_recover_count++;

    for (uint32_t round = 1U; round <= ST67_READY_RECOVER_ROUNDS; round++) {
        g_st67_ready_recover_rounds = round;
        SYS_LOG_INFO("ready_recover: round %u/%u: no ready in %u ms, module kept power -> AT+RST\n",
                     (unsigned)round, (unsigned)ST67_READY_RECOVER_ROUNDS, (unsigned)time_ms);

        ret = W61_AT_Common_SetExecute(Obj, (uint8_t *)"AT+RST\r\n", W61_NCP_TIMEOUT);
        if (ret != W61_STATUS_OK) {
            g_st67_ready_recover_result = 2U;
            SYS_LOG_ERROR("ready_recover: AT+RST failed (%d), module not answering\n", (int)ret);
            return W61_STATUS_ERROR;
        }

        ret = __real_W61_WaitForReady(Obj, time_ms);
        if (ret == W61_STATUS_OK) {
            g_st67_ready_recover_result = 1U;
            SYS_LOG_INFO("ready_recover: ready received after AT+RST (round %u)\n", (unsigned)round);
            return W61_STATUS_OK;
        }
        SYS_LOG_ERROR("ready_recover: still no ready after AT+RST (round %u)\n", (unsigned)round);

        /* The module took the AT+RST. Does it talk at all? If so the banner was lost. */
        if (W61_AT_Common_SetExecute(Obj, (uint8_t *)"AT\r\n", W61_NCP_TIMEOUT) == W61_STATUS_OK) {
            g_st67_ready_recover_result = 4U;
            SYS_LOG_INFO("ready_recover: no ready banner but the module answers AT (round %u): continuing\n",
                         (unsigned)round);
            vTaskDelay(pdMS_TO_TICKS(100U));   /* same settle time as after a real ready */
            return W61_STATUS_OK;
        }
    }

    g_st67_ready_recover_result = 3U;
    return ret;
}