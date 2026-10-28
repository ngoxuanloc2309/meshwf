/**
 * @file sx_mqtt.h
 * @brief MQTT publish service of the meshwf board: bridges SimplePLC (plc_mqtt) and
 *        the MQTT client inside the ST67W611 module (components/st67, st67_mqtt.h).
 *
 * SimplePLC only decides WHEN to publish and renders topic + payload; it has no network
 * client (decision 31). This service is the "host" of that contract:
 *   - registers the publish callback (non-blocking: copies into a small static queue),
 *   - tells the library which broker connection is live (splc_mqtt_set_status),
 *   - reads the broker the App wrote (splc_mqtt_get_broker); before the App configured one
 *     it uses the defaults of app/app_config.h,
 *   - connects, reconnects with backoff and sends from the ST67 task.
 *
 * Subscribe (SPLC_MQTT_SUB_ENABLE): the pump copies the topics the library wants (it may only
 * be asked from the engine task) into a mailbox; the ST67 task subscribes / unsubscribes the
 * difference after every (re)connect and whenever the set changes; received messages are
 * copied in the vendor callback context into a small queue and fed to splc_mqtt_input() by
 * the pump. The module subscribes at QoS 0 only and reports no retain flag.
 *
 * Two contexts, never mixed (SimplePLC is single-threaded, the AT driver blocks):
 *   engine task (the one running plc_engine_poll()):  sx_mqtt_init(), sx_mqtt_pump()
 *   ST67 task                                       :  sx_mqtt_service(), sx_mqtt_notify_module_reset()
 * They exchange data only through a config mailbox and the publish queue, both guarded by
 * short critical sections.
 *
 * Compiles to nothing when SPLC_MQTT_ENABLE = 0.
 */
#ifndef SX_MQTT_H
#define SX_MQTT_H

#include <stdbool.h>
#include <stdint.h>

#include "splc_opt.h"

#ifndef SX_MQTT_QUEUE_DEPTH
#define SX_MQTT_QUEUE_DEPTH      4U       /* ~0.35 KB each; FreeRTOS heap is only 32 KB */
#endif
#ifndef SX_MQTT_TOPIC_MAX
#define SX_MQTT_TOPIC_MAX        96U
#endif
#ifndef SX_MQTT_BACKOFF_MIN_MS
#define SX_MQTT_BACKOFF_MIN_MS   5000U
#endif
#ifndef SX_MQTT_BACKOFF_MAX_MS
#define SX_MQTT_BACKOFF_MAX_MS   60000U
#endif
#ifndef SX_MQTT_CONNECT_TIMEOUT_MS
#define SX_MQTT_CONNECT_TIMEOUT_MS 15000U
#endif
/* After connecting to the DEFAULT broker (App has not sent one), publish
 * DEFAULT_MESSAGE on DEFAULT_TOPIC (app_config.h) once. Bring-up aid; 0 = off. */
#ifndef SX_MQTT_HELLO_ON_DEFAULT
#define SX_MQTT_HELLO_ON_DEFAULT 1
#endif

/* Subscribe (SPLC_MQTT_SUB_ENABLE = 1). */
#ifndef SX_MQTT_RX_DEPTH
#define SX_MQTT_RX_DEPTH         2U       /* ~0.23 KB each; vendor thread -> engine task */
#endif
#ifndef SX_MQTT_SUB_RETRY_MS
#define SX_MQTT_SUB_RETRY_MS     5000U    /* wait before a failed (un)subscribe is tried again */
#endif

#if SPLC_MQTT_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

/* Engine task, once, BEFORE plc_engine_init() (plc_mqtt keeps the registered callback). */
void sx_mqtt_init(void);

/* Engine task, every loop. Cheap: does its work every 100 ms (broker mailbox + status). */
void sx_mqtt_pump(uint32_t now_ms);

/* ST67 task, every loop, after the ST67 bring-up succeeded. link_up = Wi-Fi has an IP.
 * May block for several seconds while connecting (AT commands over SPI). */
void sx_mqtt_service(uint32_t now_ms, bool link_up);

/* Facade lifecycle hook (module reset): only sets a flag. */
void sx_mqtt_notify_module_reset(void);

#ifdef __cplusplus
}
#endif

#else  /* SPLC_MQTT_ENABLE == 0 */

static inline void sx_mqtt_init(void) {}
static inline void sx_mqtt_pump(uint32_t now_ms) { (void)now_ms; }
static inline void sx_mqtt_service(uint32_t now_ms, bool link_up) { (void)now_ms; (void)link_up; }
static inline void sx_mqtt_notify_module_reset(void) {}

#endif /* SPLC_MQTT_ENABLE */

#endif /* SX_MQTT_H */