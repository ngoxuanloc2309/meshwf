#include "splc_indicators_port.h"
#include "splc_opt.h"

#if SPLC_INDICATOR_ENABLE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "main.h"                /* CubeMX: LED1/LED2/LED3 pin labels, HAL GPIO */
#include "splc_log.h"
#include "plc_indicator.h"       /* library: splc_indicator_init(), splc_indicator_register() */
#include "splc_wifi_port.h"
#if SPLC_MQTT_ENABLE
#include "st67_mqtt.h"
#include "sx_mqtt.h"
#endif

/* Polarity: 0 = pin HIGH lights the LED (assumption, not confirmed on the schematic). */
#ifndef SPLC_IND_LED_ACTIVE_LOW
#define SPLC_IND_LED_ACTIVE_LOW 0
#endif

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} led_pin_t;

/* Indexed by SPLC_LED_ID. Same wiring as LED_*_PORT / LED_*_PIN in board_wifi_io.h:
 * POWER = PC15 (LED3), STATUS = PC14 (LED2), LINK = PC13 (LED1). */
static const led_pin_t s_led[SPLC_LED_COUNT] = {
    [SPLC_LED_POWER]  = { LED3_GPIO_Port, LED3_Pin },
    [SPLC_LED_STATUS] = { LED2_GPIO_Port, LED2_Pin },
    [SPLC_LED_LINK]   = { LED1_GPIO_Port, LED1_Pin },
};

/* GPIO write only: no waiting, no logging (library contract, plc_indicator.h). */
static void led_set(SPLC_LED_ID led, bool on, void *ctx)
{
    (void)ctx;
    if ((unsigned)led >= (unsigned)SPLC_LED_COUNT) {
        return;
    }
    HAL_GPIO_WritePin(s_led[led].port, s_led[led].pin,
                      (on != (SPLC_IND_LED_ACTIVE_LOW != 0)) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ---- Wi-Fi ------------------------------------------------------------------------------ */

/* The scan runs in the engine task: the state comes from the cache the ST67 task keeps. */
static SPLC_LINK_STATE wifi_state(void *ctx)
{
    (void)ctx;
    return (SPLC_LINK_STATE)splc_wifi_port_link_state();
}

#if SPLC_MQTT_ENABLE
/* ---- MQTT (the client inside the ST67 module) ------------------------------------------- */

static SPLC_LINK_STATE mqtt_state(void *ctx)
{
    (void)ctx;
    if (st67_mqtt_is_connected()) {
        return SPLC_LINK_STATE_CONNECTED;
    }
    return sx_mqtt_has_error() ? SPLC_LINK_STATE_FAILED : SPLC_LINK_STATE_CONNECTING;
}
#endif

/* ---- init ------------------------------------------------------------------------------- */

static const char *TAG = "IND_PORT";

void splc_indicators_port_init(void)
{
    static const splc_ind_ops_t ops = { .set = led_set, .ctx = NULL };

    if (!splc_indicator_init(&ops)) {
        log_error(TAG, "indicator init FAILED");
        return;
    }

    static const splc_ind_proto_t wifi = {
        .name       = "wifi",
        .pattern    = { .on_ms = 100U, .off_ms = 900U, .count = 1U, .pause_ms = 0U },   /* 1 flash / s */
        .get_state  = wifi_state,
        .ctx        = NULL,
        .depends_on = 0U,
    };
    const int wifi_row = splc_indicator_register(&wifi);
    if (wifi_row < 0) {
        log_error(TAG, "register wifi FAILED");
    }

#if SPLC_MQTT_ENABLE
    static const splc_ind_proto_t mqtt = {
        .name       = "mqtt",
        .pattern    = { .on_ms = 100U, .off_ms = 200U, .count = 2U, .pause_ms = 1000U }, /* 2 flashes, pause */
        .get_state  = mqtt_state,
        .ctx        = NULL,
        .depends_on = 1U,        /* row index + 1 of "wifi" (the first registered row) */
    };
    if (wifi_row >= 0 && splc_indicator_register(&mqtt) < 0) {
        log_error(TAG, "register mqtt FAILED");
    }
#endif

    log_info(TAG, "indicator ready (POWER PC15, STATUS PC14, LINK PC13, active %s)",
             SPLC_IND_LED_ACTIVE_LOW ? "low" : "high");
}

#endif /* SPLC_INDICATOR_ENABLE */
