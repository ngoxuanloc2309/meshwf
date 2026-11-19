#ifndef SPLC_RTC_TZMEM_H
#define SPLC_RTC_TZMEM_H

/*
 * splc_rtc_tzmem.h - how the time-zone offset is laid out in the 4 bytes of user RAM of the
 * RX8130CE (registers 0x20..0x23, kept alive by the chip's own battery).
 *
 *   byte 0  tz low byte   (tz_offset_min, int16, little endian)
 *   byte 1  tz high byte
 *   byte 2  0x5A          marker
 *   byte 3  b0 ^ b1 ^ b2 ^ 0xA5   check
 *
 * The RAM holds random bits after a total power loss, so a record is accepted only when the
 * marker and the check match AND the offset is a real one (-720..+840 min). Pure functions,
 * no dependency: the host test includes this very file.
 */

#include <stdbool.h>
#include <stdint.h>

#define SPLC_TZMEM_LEN     4U
#define SPLC_TZMEM_MARK    0x5AU
#define SPLC_TZMEM_XOR     0xA5U
#define SPLC_TZMEM_TZ_MIN  (-720)
#define SPLC_TZMEM_TZ_MAX  (840)

static inline void splc_tzmem_encode(int16_t tz, uint8_t out[SPLC_TZMEM_LEN])
{
    const uint16_t u = (uint16_t)tz;
    out[0] = (uint8_t)(u & 0xFFU);
    out[1] = (uint8_t)(u >> 8);
    out[2] = (uint8_t)SPLC_TZMEM_MARK;
    out[3] = (uint8_t)(out[0] ^ out[1] ^ out[2] ^ SPLC_TZMEM_XOR);
}

static inline bool splc_tzmem_decode(const uint8_t in[SPLC_TZMEM_LEN], int16_t *tz)
{
    if (in[2] != (uint8_t)SPLC_TZMEM_MARK) {
        return false;
    }
    if (in[3] != (uint8_t)(in[0] ^ in[1] ^ in[2] ^ SPLC_TZMEM_XOR)) {
        return false;
    }
    const int16_t v = (int16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
    if (v < SPLC_TZMEM_TZ_MIN || v > SPLC_TZMEM_TZ_MAX) {
        return false;
    }
    *tz = v;
    return true;
}

#endif /* SPLC_RTC_TZMEM_H */