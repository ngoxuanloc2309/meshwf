#ifndef SPLC_RTC_PORT_H
#define SPLC_RTC_PORT_H

/*
 * splc_rtc_port.h - external RTC (RX8130CE on I2C1) as SimplePLC's clock (decision 57).
 *
 * Registers the get_epoch / set_epoch / is_synced hooks with plc_rtc_register(). The hooks run
 * in the engine task (the library's contract), so the I2C bus has a single user.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Engine task, after plc_engine_init() (the log is up) and before the first plc_engine_poll().
 * Initialises the chip (blocks ~40 ms, ~165 ms when the chip reports a power loss) and registers
 * the hooks. A chip that does not answer is not fatal: the hooks keep failing (the library then
 * reports no time and answers a Host write with a device error) and the chip is probed again
 * every RTC_INIT_RETRY_MS. A stuck I2C1 bus (SDA held low) is recovered after
 * RTC_BUS_ERR_THRESHOLD consecutive bus errors. */
void splc_rtc_port_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SPLC_RTC_PORT_H */