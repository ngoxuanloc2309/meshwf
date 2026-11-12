/**
 * @file st67w611_ble_stub.c
 * @brief Weak stand-ins for the vendor BLE entry points that this build does not compile.
 *
 * The vendor W6X_Reset() (w6x_sys.c) calls W6X_Ble_DeInit() / W6X_Ble_Init() to save and restore a
 * BLE session across the reset. This project never compiles w6x_ble.c and never starts BLE
 * (ResetCfg.Ble_status is never W61_MODULE_STATE_INIT), so those two calls are never executed. They
 * only have to exist for the linker once something references W6X_Reset(): st67w611_soft_reset().
 *
 * Weak, so a build that does compile w6x_ble.c silently uses the real functions.
 */
#include "w6x_api.h"

__attribute__((weak)) W6X_Status_t W6X_Ble_Init(W6X_Ble_Mode_e mode, uint8_t *recv_data, size_t max_len)
{
    (void)mode;
    (void)recv_data;
    (void)max_len;
    return W6X_STATUS_NOT_SUPPORTED;
}

__attribute__((weak)) void W6X_Ble_DeInit(void)
{
}