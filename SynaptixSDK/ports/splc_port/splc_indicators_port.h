#ifndef SPLC_INDICATORS_PORT_H
#define SPLC_INDICATORS_PORT_H
/*
 * splc_indicators_port.h - product port (outside the simple_plc library)
 *
 * Wires the library's connection indicator (services/plc_indicator, docs/PLC_Indicator_design.md)
 * to the three LEDs of the Wi-Fi IO board (names as in board_wifi_io.h; CubeMX labels in main.h):
 *
 *     POWER   LED_POWER_PIN   PC15  (CubeMX LED3)
 *     STATUS  LED_STATUS_PIN  PC14  (CubeMX LED2)
 *     LINK    LED_LINK_PIN    PC13  (CubeMX LED1)
 *
 * The library decides what the LEDs show (docs: POWER lit; STATUS flashes while the protocol shown
 * on LINK is CONNECTING / FAILED; both steady when every enabled protocol is CONNECTED). This
 * port only gives the pin write and one get_state() per protocol, in priority order:
 *     1. Wi-Fi   LINK pattern: one flash          (splc_wifi_port_link_state)
 *     2. MQTT    LINK pattern: two flashes        (st67_mqtt_is_connected / sx_mqtt_has_error)
 *                depends on Wi-Fi: hidden while Wi-Fi is CONNECTING / FAILED
 *                (only when SPLC_MQTT_ENABLE = 1)
 *
 * Pins are push-pull outputs set up by CubeMX; this port only writes them. Default polarity is
 * ACTIVE HIGH; define SPLC_IND_LED_ACTIVE_LOW=1 (splcopts.h or compiler flag) for LEDs on 3V3.
 * PC13..PC15 are weak STM32 pins (a few mA): size the LED resistor accordingly.
 *
 * Empty unit when SPLC_INDICATOR_ENABLE = 0.
 */
#ifdef __cplusplus
extern "C" {
#endif
/* Call once from the engine task after plc_engine_init() and splc_wifi_port_init(), before the
 * first plc_engine_poll() (the library closes its table at the first scan). Lights POWER. Only
 * call when SPLC_INDICATOR_ENABLE = 1 (the symbol does not exist otherwise). */
void splc_indicators_port_init(void);
#ifdef __cplusplus
}
#endif
#endif /* SPLC_INDICATORS_PORT_H */
