#ifndef ST67W611_WIFI_SWITCH_H
#define ST67W611_WIFI_SWITCH_H

/* Switch the ST67W611 to another access point.
 *
 * Built only on the public API of libs/st67w611 (st67w611_get_state,
 * st67w611_disconnect, st67w611_connect); the library is not modified.
 *
 * Limits mirror the vendor driver (W6X_WIFI_MAX_SSID_SIZE 32,
 * W6X_WIFI_MAX_PASSWORD_SIZE 63) and are checked BEFORE the working link is
 * dropped, so a bad argument never disconnects the module.
 *
 * Behaviour:
 *  - READY                      : connect directly.
 *  - CONNECTING/CONNECTED/ERROR : disconnect first, then connect.
 *  - OFF/INITIALIZING/RESETTING : ST67W611_ERR_INVALID_STATE, nothing is done.
 * If the new connect fails the module stays disconnected (state ERROR); the
 * caller may call this function again, with the new or the old credentials.
 * Blocking (as st67w611_connect); call from a task, not from an ISR.
 * Not reentrant: the caller must serialise calls (the library itself returns
 * ST67W611_ERR_BUSY while one of its operations is running). */

#include "st67w611.h"

#define ST67_SWITCH_SSID_MAX_LEN      32U
#define ST67_SWITCH_PASSWORD_MAX_LEN  63U

st67w611_status_t meshwf_st67_wifi_switch(const char *ssid, const char *password);

#endif