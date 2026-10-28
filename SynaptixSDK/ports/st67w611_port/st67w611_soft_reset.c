#include "st67w611_soft_reset.h"

#include "w6x_api.h"

static st67w611_status_t map_vendor(W6X_Status_t s)
{
    if (s == W6X_STATUS_OK)      return ST67W611_OK;
    if (s == W6X_STATUS_TIMEOUT) return ST67W611_ERR_TIMEOUT;
    if (s == W6X_STATUS_BUSY)    return ST67W611_ERR_BUSY;
    return ST67W611_ERR_VENDOR;
}

st67w611_status_t meshwf_st67_soft_reset(void)
{
    st67w611_state_t state;
    st67w611_status_t rc;
    W6X_Status_t vs;

    rc = st67w611_get_state(&state);
    if (rc != ST67W611_OK) {
        return rc;
    }
    if (state == ST67W611_STATE_OFF || state == ST67W611_STATE_INITIALIZING ||
        state == ST67W611_STATE_RESETTING) {
        return ST67W611_ERR_INVALID_STATE;
    }

    if (state != ST67W611_STATE_READY) {
        rc = st67w611_disconnect();
        /* In ERROR the module may not answer; resetting it is the point. */
        if (rc != ST67W611_OK && state != ST67W611_STATE_ERROR) {
            return rc;
        }
    }

    vs = W6X_Reset(0U);
    if (vs != W6X_STATUS_OK) {
        return map_vendor(vs);
    }
    return map_vendor(W6X_WiFi_Station_Start());
}