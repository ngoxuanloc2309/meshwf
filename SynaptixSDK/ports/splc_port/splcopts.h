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

/* Logging (LPUART1) */
#define SPLC_LOG_LEVEL                4

/* Clock: external RTC IC on I2C1 (decision 57). The hooks are registered by
 * the product glue (ports/splc_port) before plc_engine_init(). */
#define SPLC_RTC_EXTERNAL             1

/* No ADC in this CubeMX project (no hadc1), and the board has 0 AI. */
#define SPLC_ADC_BUILTIN              0

/* Features. MQTT publish runs through SynaptixSDK/services/mqtt + components/st67 (the MQTT
 * client inside the ST67 module). MQTT Subscribe, MQTT TLS and HTTP(S) are not written yet;
 * the only RS485 port is the App link, so Modbus master (MB_READ/MB_WRITE) is off. */
#define SPLC_MQTT_ENABLE              1
#define SPLC_MQTT_SUB_ENABLE          0
#define SPLC_MBM_ENABLE               0
#define SPLC_MBW_ENABLE               0
#define SPLC_REAL_ENABLE              1
#define SPLC_WIFI_ENABLE              1

#endif /* SPLCOPTS_H */