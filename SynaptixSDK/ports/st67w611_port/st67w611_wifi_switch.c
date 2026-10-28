#include "st67w611_wifi_switch.h"

#include <stddef.h>

static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0U;

    while (n < cap && s[n] != '\0') {
        ++n;
    }
    return n;
}

st67w611_status_t meshwf_st67_wifi_switch(const char *ssid, const char *password)
{
    st67w611_state_t state;
    st67w611_status_t rc;

    /* Validate first: a bad argument must not cost the working link. */
    if (ssid == NULL || ssid[0] == '\0' ||
        bounded_len(ssid, ST67_SWITCH_SSID_MAX_LEN + 1U) > ST67_SWITCH_SSID_MAX_LEN) {
        return ST67W611_ERR_INVALID_ARG;
    }
    if (password != NULL &&
        bounded_len(password, ST67_SWITCH_PASSWORD_MAX_LEN + 1U) > ST67_SWITCH_PASSWORD_MAX_LEN) {
        return ST67W611_ERR_INVALID_ARG;
    }

    rc = st67w611_get_state(&state);
    if (rc != ST67W611_OK) {
        return rc;
    }

    switch (state) {
    case ST67W611_STATE_READY:
        break;
    case ST67W611_STATE_CONNECTING:
    case ST67W611_STATE_CONNECTED:
    case ST67W611_STATE_ERROR:
        /* After a failed connect the module is alive but may still be
         * mid-join; a refused disconnect is reported and nothing else runs. */
        rc = st67w611_disconnect();
        if (rc != ST67W611_OK) {
            return rc;
        }
        break;
    default:
        return ST67W611_ERR_INVALID_STATE;
    }

    return st67w611_connect(ssid, password);
}