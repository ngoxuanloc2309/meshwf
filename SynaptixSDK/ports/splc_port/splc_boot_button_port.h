#ifndef SPLC_BOOT_BUTTON_PORT_H
#define SPLC_BOOT_BUTTON_PORT_H
/*
 * splc_boot_button_port.h - product port (outside the simple_plc library)
 *
 * Hold the board button (CubeMX label "Button", PA0) for SPLC_BOOT_BTN_HOLD_MS (7 s) and the MCU
 * jumps to the STM32H5 system bootloader (USB DFU). A power cycle / reset brings the application
 * back.
 *
 * Polarity: CubeMX configures PA0 as input with NO pull (gpio.c), so the idle level depends on the
 * board's external resistor, which is not confirmed. SPLC_BOOT_BTN_ACTIVE_LEVEL:
 *     -1 (default) AUTO: the level seen at the first poll is "released"; any other level is
 *                  "pressed". DO NOT hold the button while the board powers up. Fail-safe: a wrong
 *                  guess can never make the board enter the bootloader by itself.
 *      0 / 1       fixed: the pin level that means "pressed".
 *
 * Call splc_boot_button_poll() from the engine task loop (about every 1 ms); it samples the pin at
 * most every 10 ms and never blocks (except on the jump itself: ~100 ms to let the log out).
 *
 * Side effect to keep in mind on a PLC: HAL_DeInit() resets the GPIO ports, so every output goes
 * to its reset state (floating input) when the jump happens.
 *
 * Empty unit when SPLC_BOOT_BTN_ENABLE = 0.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SPLC_BOOT_BTN_ENABLE
#define SPLC_BOOT_BTN_ENABLE        1
#endif

#ifndef SPLC_BOOT_BTN_HOLD_MS
#define SPLC_BOOT_BTN_HOLD_MS       7000U
#endif

#ifndef SPLC_BOOT_BTN_ACTIVE_LEVEL
#define SPLC_BOOT_BTN_ACTIVE_LEVEL  (-1)
#endif

/* System memory (ROM bootloader) of the STM32H523. Value given by the project owner; not checked
 * against AN2606 here. */
#ifndef SYSTEM_BOOTLOADER_ADDRESS
#define SYSTEM_BOOTLOADER_ADDRESS   0x0BF97000UL
#endif

#if SPLC_BOOT_BTN_ENABLE
void splc_boot_button_poll(uint32_t now_ms);
#else
static inline void splc_boot_button_poll(uint32_t now_ms) { (void)now_ms; }
#endif

#ifdef __cplusplus
}
#endif
#endif /* SPLC_BOOT_BUTTON_PORT_H */