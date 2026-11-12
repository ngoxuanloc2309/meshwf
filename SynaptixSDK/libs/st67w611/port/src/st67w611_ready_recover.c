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
 * Logs go through the vendor SYS_LOG_* macros; the port forwards them to log_debug
 * ("vendor: ..." in splc_port.c, via st67w611_vendor_log_init()).
 */
#include <stdint.h>
#include <stddef.h>

#include "w61_at_api.h"
#include "w61_at_common.h"

extern W61_Status_t __real_W61_WaitForReady(W61_Object_t *Obj, uint32_t time_ms);

/* Number of times the AT+RST recovery ran (read it from a debugger / diag). */
volatile uint32_t g_st67_ready_recover_count;
/* Last result: 0 = never ran, 1 = recovered, 2 = AT+RST failed, 3 = still no ready. */
volatile uint32_t g_st67_ready_recover_result;

W61_Status_t __wrap_W61_WaitForReady(W61_Object_t *Obj, uint32_t time_ms)
{
    W61_Status_t ret = __real_W61_WaitForReady(Obj, time_ms);

    if (ret == W61_STATUS_OK || Obj == NULL) {
        return ret;
    }

    g_st67_ready_recover_count++;
    SYS_LOG_INFO("ready_recover: no ready in %u ms, module kept power -> AT+RST\n",
                 (unsigned)time_ms);

    ret = W61_AT_Common_SetExecute(Obj, (uint8_t *)"AT+RST\r\n", W61_NCP_TIMEOUT);
    if (ret != W61_STATUS_OK) {
        g_st67_ready_recover_result = 2U;
        SYS_LOG_ERROR("ready_recover: AT+RST failed (%d), module not answering\n", (int)ret);
        return W61_STATUS_ERROR;
    }

    ret = __real_W61_WaitForReady(Obj, time_ms);
    if (ret != W61_STATUS_OK) {
        g_st67_ready_recover_result = 3U;
        SYS_LOG_ERROR("ready_recover: still no ready after AT+RST\n");
        return ret;
    }

    g_st67_ready_recover_result = 1U;
    SYS_LOG_INFO("ready_recover: ready received after AT+RST\n");
    return W61_STATUS_OK;
}