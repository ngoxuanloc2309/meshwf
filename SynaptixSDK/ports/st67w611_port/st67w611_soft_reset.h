#ifndef ST67W611_SOFT_RESET_H
#define ST67W611_SOFT_RESET_H

/* Soft reset (AT+RST) of the ST67W611 for boards WITHOUT an EN pin.
 *
 * libs/st67w611 and the vendor driver are not modified. The facade has no
 * soft reset (st67w611_reset() only calls the port's hardware_reset hook and
 * runs W6X_DeInit first, which is unsafe without EN), so this calls the vendor
 * W6X_Reset(0) directly.
 *
 * Sequence:
 *   1. st67w611_disconnect()  - facade state becomes READY (it matches the
 *                               module state after the reboot, which has no link).
 *   2. W6X_Reset(0)           - AT+RST, wait ready, vendor re-inits Wi-Fi (and
 *                               Net on T01). Retry-safe: on failure the vendor
 *                               keeps its pending-state backup, so calling again
 *                               just repeats AT+RST.
 *   3. W6X_WiFi_Station_Start() - the reboot drops the station mode.
 * Afterwards connect with meshwf_st67_wifi_switch().
 *
 * What it can NOT do: recover a module that is hung hard and does not answer
 * AT commands (W6X_Reset fails at its first AT command and changes nothing).
 * Only the EN pin can. Without a hardware reset path, report the error to the PLC.
 *
 * Caller contract: the facade's internal lock and scan flag are private, so the
 * caller must serialise this call with every other Wi-Fi call (use one Wi-Fi
 * task). Blocking (seconds); never from an ISR. All sockets/MQTT sessions and
 * the AP link are lost. Power save is not touched (the vendor reset saves and
 * restores whatever mode was set).
 *
 * Return: OK, INVALID_STATE (OFF/INITIALIZING/RESETTING), BUSY, TIMEOUT,
 * VENDOR. If the disconnect step fails while in ERROR state the reset is still
 * attempted (that is the recovery case); the facade then stays in ERROR until
 * meshwf_st67_wifi_switch() (which disconnects first) or st67w611_disconnect()
 * succeeds. */

#include "st67w611.h"

st67w611_status_t meshwf_st67_soft_reset(void);

#endif