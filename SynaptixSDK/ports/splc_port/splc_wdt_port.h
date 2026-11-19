/*
 * splc_wdt_port.h - watchdog glue of the meshwf Wi-Fi IO board.
 *
 * Library side: SynaptixSDK/libs/simple_plc/services/plc_wdt (software supervisor, no HAL).
 * This file: the IWDG (CubeMX handle `hiwdg`), the supervisor task, the reset-cause log and the
 * "NET" channel of the ST67 task. Design: docs/SPLC_Watchdog_Design.md.
 *
 * All of it is compiled out with SPLC_WDG_ENABLE = 0 (default in splcopts.h): nothing in the
 * firmware changes until the option is set to 1 AND the IWDG is configured in CubeMX.
 */
#ifndef SPLC_WDT_PORT_H
#define SPLC_WDT_PORT_H

#include <stdbool.h>
#include <stdint.h>

#include "splc_opt.h"
#include "plc_wdt.h"

/* Channel of the ST67 (Wi-Fi) task. */
#define SPLC_WDT_CH_NET                  PLC_WDT_CH_USER0

/* Longest gap between two passes of the ST67 task loop. The loop also runs the Wi-Fi connect and
 * scan and the MQTT service, which can block for a while: start generous, measure, tighten. */
#ifndef SPLC_WDG_NET_TIMEOUT_MS
#define SPLC_WDG_NET_TIMEOUT_MS          45000U
#endif

/* Extra time for channel NET around st67w611_init() + st67w611_start() (observed ~5.8 s). */
#ifndef SPLC_WDG_ST67_START_GRACE_MS
#define SPLC_WDG_ST67_START_GRACE_MS     30000U
#endif

/* Extra time for channel NET around ONE blocking Wi-Fi connect (splc_wifi_port.c, do_connect).
 * Worst case, from the vendor constants and the facade retry (port: max_transport_retries = 2):
 *   switch: disconnect up to 5 s, then (connect 10 s + wait for the IP 15 s) x 3 attempts = 80 s,
 *   plus 2 x 100 ms retry delay. 95 s leaves ~15 s margin. Only a connect that blocks longer than
 *   this, a real hang, lets the NET channel expire. */
#ifndef SPLC_WDG_WIFI_CONNECT_GRACE_MS
#define SPLC_WDG_WIFI_CONNECT_GRACE_MS   95000U
#endif

/* Extra time for channel NET around ONE soft reset (AT+RST) of the ST67 (meshwf_st67_soft_reset).
 * The facade reset timeout is 3 s per step and the module needs ~6 s to boot (init+start observed
 * ~5.8 s); 30 s keeps a wide margin and still lets a real hang expire the channel. */
#ifndef SPLC_WDG_ST67_RESET_GRACE_MS
#define SPLC_WDG_ST67_RESET_GRACE_MS     30000U
#endif

/* Extra time for the one light AT query (st67w611_get_mac) the connect diagnostics send to a
 * module that gave no reason: vendor W61_WIFI_TIMEOUT is 6 s, x (no retry in the query) = 6 s. */
#ifndef SPLC_WDG_WIFI_PROBE_GRACE_MS
#define SPLC_WDG_WIFI_PROBE_GRACE_MS     15000U
#endif

/* The supervisor runs in its own task, above the project tasks (SPLC 6, ST67 5) and the vendor
 * tasks (spi_xfer_engine 7, Modem_Process 8), so a spinning task cannot starve it; only the
 * ST67dbg task (20) is higher. Stack in words. */
#ifndef SPLC_WDG_TASK_PRIO
#define SPLC_WDG_TASK_PRIO               12U
#endif
#ifndef SPLC_WDG_TASK_STACK_WORDS
#define SPLC_WDG_TASK_STACK_WORDS        512U
#endif

/* 1 = stop the IWDG counter while the debugger has the core halted (otherwise every breakpoint
 * resets the board). Has no effect without a debugger. */
#ifndef SPLC_WDG_FREEZE_IN_DEBUG
#define SPLC_WDG_FREEZE_IN_DEBUG         1
#endif

#if SPLC_WDG_ENABLE

/* Registers the kick / fault functions and channel NET. No logging (the logger is not up yet):
 * call from splc_host_start(), BEFORE the tasks are created. */
void splc_wdt_port_init(void);

/* Creates the supervisor task. Returns false if the task could not be created. */
bool splc_wdt_port_start_task(void);

/* Logs the reset cause and the IWDG settings found in `hiwdg`, then clears the reset flags.
 * Needs the logger: call from splc_task after plc_engine_init(). */
void splc_wdt_port_boot_report(void);

#else  /* no code */

static inline void splc_wdt_port_init(void) {}
static inline bool splc_wdt_port_start_task(void) { return true; }
static inline void splc_wdt_port_boot_report(void) {}

#endif

#endif /* SPLC_WDT_PORT_H */