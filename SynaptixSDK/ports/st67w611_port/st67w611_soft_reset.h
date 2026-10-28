#ifndef ST67W611_SOFT_RESET_H
#define ST67W611_SOFT_RESET_H

/* Soft reset (AT+RST) of the ST67W611 for boards WITHOUT an EN pin.
 *
 * Thin wrapper of st67w611_soft_reset() (libs/st67w611). The facade owns the
 * operation lock and the state machine, so the reset is serialised against
 * connect/disconnect/scan by the library itself and the state ends READY.
 *
 * Do NOT use st67w611_reset() on a board without EN: it de-initialises the
 * vendor driver before its hardware reset fails and leaves the driver stopped.
 *
 * What it can NOT do: recover a module that is hung hard and does not answer
 * AT commands (the vendor reset fails at its first AT command and changes
 * nothing; state ERROR, retry-safe). Only the EN pin can. Report the error.
 *
 * Blocking (seconds), task context only. All sockets/MQTT sessions and the AP
 * link are lost; connect again with meshwf_st67_wifi_switch().
 *
 * Return: st67w611_soft_reset() codes (OK, NOT_INITIALIZED, INVALID_STATE,
 * NOT_READY, BUSY, TIMEOUT, VENDOR, or what a lifecycle hook returned). */

#include "st67w611.h"

st67w611_status_t meshwf_st67_soft_reset(void);

#endif