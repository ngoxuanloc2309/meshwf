#ifndef SPLCOPTS_H
#define SPLCOPTS_H

/*
 * splcopts.h - SimplePLC configuration of the Wi-Fi IO board (meshwf,
 * STM32H523 + ST67W611). Library defaults and the full option list:
 * SynaptixSDK/libs/simple_plc/config/splc_opt.h. Only options that differ from
 * the default (or are worth stating) are listed.
 */

/* Target */
#define SPLC_PLATFORM                 SPLC_PLATFORM_STM32H5
#define SPLC_BOARD                    SPLC_BOARD_WIFI_IO

/* Chip Flash size in KB. ASSUMED STM32H523CC = 256 (same as the Zigbee-IO
 * board). NOTE: STM32H523xx_FLASH.ld declares 512 KB; the Rule A/B + Retain
 * sectors are the last 5 sectors of SPLC_FLASH_SIZE_KB, so this MUST be the
 * real chip size. */
#define SPLC_FLASH_SIZE_KB            256

/* FreeRTOS: SynaptixSDK/ports/splc_port creates the one engine task. */
#define SX_OS_USE_FREERTOS            1

/* App link (rules, tags, config): 1 = USB-CDC through SimplePLC's own TinyUSB
 * (port/usb, CubeMX USB_DRD_FS), 0 = RS485 Modbus RTU slave on USART1 (DE = PA15).
 * Both are implemented in board_wifi_io.c; this is the only switch. */
#define SPLC_PORT_USB_CDC             1

/* Engine */
#define PLC_SCAN_INTERVAL_MS          10U
#define PLC_PVD_EMERGENCY_SAVE_ENABLE 1

/* Logging (LPUART1). SPLC_LOG_LEVEL: 0 OFF, 1 ERROR, 2 WARN, 3 INFO, 4 DEBUG. */
#define SPLC_LOG_LEVEL                3
/* 1 = compile-time filter (config/splc_log.h): log calls above SPLC_LOG_LEVEL are removed with their
 * strings to save Flash, and the level is fixed at build time. 0 = run-time filter only (as before).
 * Does not affect libs/st67w611, which has its own logger. */
#define SPLC_LOG_COMPILE_FILTER       1

/* Clock: external RTC IC on I2C1 (decision 57). The hooks are registered by
 * the product glue (ports/splc_port) before plc_engine_init(). */
#define SPLC_RTC_EXTERNAL             1

/* No ADC in this CubeMX project (no hadc1), and the board has 0 AI. */
#define SPLC_ADC_BUILTIN              0

/* Features. MQTT publish + subscribe run through SynaptixSDK/services/mqtt + components/st67
 * (the MQTT client inside the ST67 module). MQTT TLS certificates live in components/st67/st67_cert_store; HTTP(S) is not written yet;
 * Modbus master (MB_READ/MB_WRITE) runs on USART1, the only RS485 port, so the App link is USB (SPLC_PORT_USB_CDC 1). */
#define SPLC_MQTT_ENABLE              1
#define SPLC_MQTT_SUB_ENABLE          1
#define SPLC_MQTT_TLS_CERT_ENABLE     1
#define SPLC_MBM_ENABLE               1
#define SPLC_MBW_ENABLE               1
#define SPLC_REAL_ENABLE              1
#define SPLC_WIFI_ENABLE              1
/* Connection indicator: LEDs POWER PC15 / STATUS PC14 / LINK PC13 (splc_indicators_port.c). */
#define SPLC_INDICATOR_ENABLE         1
/* LED STATUS rhythms (LED LINK always shows the pattern of the protocol being reported):
 *   everything OK            : 300 ms on / 300 ms off    (heartbeat, drawn by the PLC scan: a frozen
 *                              engine stops it; LINK is steady on)
 *   Wi-Fi/MQTT not OK        : 1 s on / 1 s off          (CONNECTING and FAILED share it)
 *   ST67 task stalled (45 s) : 2 s on / 2 s off          (STALLED) */
#define SPLC_IND_HEARTBEAT_PERIOD_MS  600
#define SPLC_IND_HEARTBEAT_ON_MS      300
#define SPLC_IND_STATUS_CONNECTING_MS 2000
#define SPLC_IND_STATUS_FAILED_MS     2000
#define SPLC_LED_FLASH_ON_MS          1000
#define SPLC_IND_STATUS_STALLED_MS    4000
#define SPLC_IND_STATUS_STALLED_ON_MS 2000
/* The ST67 task (Wi-Fi/MQTT) has not finished a loop pass for this long: Wi-Fi and MQTT are reported
 * STALLED instead of the last cached "connected" (splc_indicators_port.c). 0 = off. */
#define SPLC_IND_NET_STALE_MS         45000U
/* Wi-Fi credentials survive a power cycle: stored by splc_wifi_store.c (one Flash sector). */
#define SPLC_HOST_WIFI_PERSIST        1

/* App link transports served at once: slot 0 = USB-CDC (board), slot 1 = Modbus TCP slave over the
 * ST67 module (components/st67/st67_modbus_tcp.c, registered in splc_task). */
#define SPLC_MODBUS_MAX_TRANSPORTS    2

#define SPLC_SYSTAG_ENABLE            0

/* Watchdog (docs/SPLC_Watchdog_Design.md). 0 = off: the firmware behaves as before. Set 1 ONLY
 * after the IWDG is configured in CubeMX (software start, window = maximum, handle hiwdg; the
 * timeout below must match its prescaler/reload) and the project was regenerated. Not built or
 * tried on the board yet: start with the generous values below and read the boot log. */
#define SPLC_WDG_ENABLE               1
#define SPLC_WDG_HW_TIMEOUT_MS        5000U   /* what CubeMX is set to; checked against hiwdg at boot */
#define SPLC_WDG_SERVICE_MS           500U    /* supervisor period */
#define SPLC_WDG_PLC_TIMEOUT_MS       2000U   /* engine task: gap between two plc_engine_poll() */
#define SPLC_WDG_NET_TIMEOUT_MS       45000U  /* ST67 task loop: first guess, measure then tighten */
#define SPLC_WDG_BOOT_GRACE_MS        30000U  /* time until the first sign of life of a channel */
#define SPLC_WDG_FLASH_GRACE_MS       3000U   /* extra time for the engine task around a Flash erase+write */
#define SPLC_WDG_ST67_START_GRACE_MS  30000U  /* extra time for st67w611_init()+start() */

#endif /* SPLCOPTS_H */