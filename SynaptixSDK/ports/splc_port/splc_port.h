#ifndef SPLC_PORT_H
#define SPLC_PORT_H

/*
 * splc_port - product glue of the Wi-Fi IO board (meshwf): starts SimplePLC and
 * the ST67W611 bring-up under FreeRTOS WITHOUT editing any CubeMX file.
 *
 * How it is started: the root CMakeLists.txt links the firmware with
 * -Wl,--wrap=MX_FREERTOS_Init. main.c still calls MX_FREERTOS_Init(); the
 * linker routes that call to __wrap_MX_FREERTOS_Init() below, which runs the
 * CubeMX original first and then calls splc_host_start() (before
 * osKernelStart(), so tasks are only created here, never run).
 *
 * Tasks (SimplePLC is single-threaded by design: only the SPLC task may touch
 * tags, rules and the Modbus draft):
 *   SPLC   plc_engine_init() then plc_engine_poll() + 1 ms delay.
 *   ST67   st67w611_init()/start() bring-up (SPLC_HOST_WIFI_TASK = 1).
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPLC_HOST_TASK_STACK_WORDS
#define SPLC_HOST_TASK_STACK_WORDS   1536U   /* 6 KB; measure with the stack high-water mark */
#endif
#ifndef SPLC_HOST_TASK_PRIO
#define SPLC_HOST_TASK_PRIO          6U
#endif

/* Wi-Fi task: ST67 bring-up (init + start). Glue of splc_wifi_ops_t comes later. */
#ifndef SPLC_HOST_WIFI_TASK
#define SPLC_HOST_WIFI_TASK          1
#endif
#ifndef SPLC_HOST_WIFI_STACK_WORDS
#define SPLC_HOST_WIFI_STACK_WORDS   1024U   /* 4 KB; vendor calls run on this stack */
#endif
#ifndef SPLC_HOST_WIFI_PRIO
#define SPLC_HOST_WIFI_PRIO          5U      /* below the engine task */
#endif

/* Bring-up aid: log FreeRTOS heap (free / minimum ever) and the Wi-Fi task's stack
 * headroom every this many ms from the Wi-Fi task. 0 = off. */
#ifndef SPLC_HOST_HEAP_LOG_MS
#define SPLC_HOST_HEAP_LOG_MS        30000U
#endif

/* Creates the tasks. Returns false (and logs) when a task cannot be created. */
bool splc_host_start(void);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_PORT_H */