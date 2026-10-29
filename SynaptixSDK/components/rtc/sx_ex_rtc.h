#ifndef SX_EX_RTC_H
#define SX_EX_RTC_H

/*
 * sx_ex_rtc.h - driver of the external RTC RX8130CE on I2C1, plus the conversion between its
 * calendar registers and UTC epoch seconds (the unit SimplePLC's plc_rtc hooks speak).
 *
 * The chip holds UTC: the library keeps the time-zone offset itself, so nothing here knows
 * about it. Year register 0..99 means 2000..2099.
 *
 * "Trustworthy time" = the VLF flag (register 0x1D bit 1) is clear. The chip sets VLF after a
 * loss of power / oscillation stop. This driver NEVER clears it on its own: only
 * rx8130ce_set_time() clears it, after the time was written. So rx8130ce_is_time_valid() is
 * false from a fresh power-up until a Host has set the clock.
 *
 * Uses the library's I2C component (components/i2c/sx_i2c.h: 7-bit addresses, blocking calls)
 * and its scheduler-aware sx_delay_ms(). Threading: one task only (the engine task).
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "sx_i2c.h"

/* ------------------------------------------------------------------ */
/*  I2C                                                                */
/* ------------------------------------------------------------------ */
#define RX8130CE_ADDR7          0x32U   /* 7-bit address (lib sx_i2c takes it un-shifted) */

/* Timeout of one transfer. A 7-byte read is well under 1 ms; the timeout only bites on a stuck
 * bus, and the caller is the engine task, so keep it short. */
#ifndef RX8130CE_I2C_TIMEOUT_MS
#define RX8130CE_I2C_TIMEOUT_MS 25U
#endif

/* ------------------------------------------------------------------ */
/*  Register Map                                                       */
/* ------------------------------------------------------------------ */
#define RX8130CE_REG_SEC        0x10
#define RX8130CE_REG_MIN        0x11
#define RX8130CE_REG_HOUR       0x12
#define RX8130CE_REG_WEEK       0x13
#define RX8130CE_REG_DAY        0x14
#define RX8130CE_REG_MONTH      0x15
#define RX8130CE_REG_YEAR       0x16

#define RX8130CE_REG_MIN_ALARM  0x17
#define RX8130CE_REG_HOUR_ALARM 0x18
#define RX8130CE_REG_WEEK_ALARM 0x19

#define RX8130CE_REG_TIMER0     0x1A
#define RX8130CE_REG_TIMER1     0x1B
#define RX8130CE_REG_EXT        0x1C
#define RX8130CE_REG_FLAG       0x1D
#define RX8130CE_REG_CTRL0      0x1E
#define RX8130CE_REG_CTRL1      0x1F

#define RX8130CE_REG_OFFSET     0x30
#define RX8130CE_REG_EXT1       0x31

/* ------------------------------------------------------------------ */
/*  Register Bit Masks                                                 */
/* ------------------------------------------------------------------ */

/* Flag Register (0x1D) */
#define RX8130CE_FLAG_VLF       (1 << 1)    /* Voltage Low Flag      */
#define RX8130CE_FLAG_VLF_VBFF  (1 << 0)    /* VBAT Full Flag        */
#define RX8130CE_FLAG_AF        (1 << 3)    /* Alarm Flag            */
#define RX8130CE_FLAG_TF        (1 << 4)    /* Timer Flag            */
#define RX8130CE_FLAG_UF        (1 << 5)    /* Update Flag           */
#define RX8130CE_FLAG_VBLF      (1 << 7)    /* VBAT Low Flag         */

/* Control Register 0 (0x1E) */
#define RX8130CE_CTRL0_STOP     (1 << 6)    /* Stop timekeeping      */
#define RX8130CE_CTRL0_AIE      (1 << 3)    /* Alarm Interrupt En    */
#define RX8130CE_CTRL0_TIE      (1 << 4)    /* Timer Interrupt En    */
#define RX8130CE_CTRL0_UIE      (1 << 5)    /* Update Interrupt En   */
#define RX8130CE_CTRL0_TEST     (1 << 7)    /* TEST bit (keep 0)     */

/* Control Register 1 (0x1F) */
#define RX8130CE_CTRL1_INIEN    (1 << 4)    /* Power switch enable   */
#define RX8130CE_CTRL1_CHGEN    (1 << 5)    /* Charge enable         */

/* Extension Register (0x1C) */
#define RX8130CE_EXT_TE         (1 << 4)    /* Timer Enable          */

/* Charge the backup source on VBAT? 0 = no (default). Set to 1 ONLY if a rechargeable cell or
 * supercap is fitted on VBAT; with a primary cell or VBAT tied to the supply it must be 0. */
#ifndef RX8130CE_CHARGE_ENABLE
#define RX8130CE_CHARGE_ENABLE  0
#endif

/* ------------------------------------------------------------------ */
/*  Week bit mapping                                                   */
/* ------------------------------------------------------------------ */
#define RX8130CE_WEEK_SUN       (1 << 0)
#define RX8130CE_WEEK_MON       (1 << 1)
#define RX8130CE_WEEK_TUE       (1 << 2)
#define RX8130CE_WEEK_WED       (1 << 3)
#define RX8130CE_WEEK_THU       (1 << 4)
#define RX8130CE_WEEK_FRI       (1 << 5)
#define RX8130CE_WEEK_SAT       (1 << 6)

/* ------------------------------------------------------------------ */
/*  Return codes                                                       */
/* ------------------------------------------------------------------ */
#define RX8130CE_OK             0
#define RX8130CE_ERR_I2C        (-1)
#define RX8130CE_ERR_NOT_INIT   (-2)
#define RX8130CE_ERR_PARAM      (-3)
#define RX8130CE_ERR_DATA       (-4)    /* the chip returned a date/time that is not a real one */

/* Range the library accepts and the chip can hold: 2000-01-01 00:00:00 .. 2099-12-31 23:59:59 UTC. */
#define RX8130CE_EPOCH_MIN      946684800UL
#define RX8130CE_EPOCH_MAX      4102444799UL

/* ------------------------------------------------------------------ */
/*  Data structures                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t sec;    /* 0 - 59                                        */
    uint8_t min;    /* 0 - 59                                        */
    uint8_t hour;   /* 0 - 23  (24-hour format)                      */
    uint8_t week;   /* bitmask: RX8130CE_WEEK_xxx                    */
    uint8_t day;    /* 1 - 31  (and valid for the month)             */
    uint8_t month;  /* 1 - 12                                        */
    uint8_t year;   /* 0 - 99  (e.g. 25 = 2025)                      */
} rx8130ce_time_t;

typedef struct {
    sx_i2c_config_t *i2c;
    bool             initialized;
    int              last_i2c_err;  /* result of the last sx_i2c_* call (SX_I2C_OK / SX_I2C_ERR_xxx):
                                     * lets the caller tell a NACK (no chip) from a stuck bus
                                     * (TIMEOUT / BUS) */
} rx8130ce_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* No power-cutoff GPIO on this board: VIO/VDD/VBAT are wired directly to 3.3 V.
 * Blocks ~40 ms (oscillator start-up) and a further ~125 ms when the chip reports VLF and needs
 * the datasheet software reset. Call it before the engine's first scan. Leaves VLF as found. */
int rx8130ce_init(rx8130ce_t *dev, sx_i2c_config_t *i2c);

/* Writes the time (week is ignored: computed from the date), then clears VLF. */
int rx8130ce_set_time(rx8130ce_t *dev, const rx8130ce_time_t *t);

/* RX8130CE_ERR_DATA when the registers hold something that is not a real date/time
 * (e.g. month 0 right after a reset). */
int rx8130ce_get_time(rx8130ce_t *dev, rx8130ce_time_t *t);

/* *valid = VLF clear. */
int rx8130ce_is_time_valid(rx8130ce_t *dev, bool *valid);

/* Pure calendar maths (no I2C), UTC. */
int rx8130ce_time_to_epoch(const rx8130ce_time_t *t, uint32_t *epoch_utc_s);   /* PARAM if t is not a real date */
int rx8130ce_epoch_to_time(uint32_t epoch_utc_s, rx8130ce_time_t *t);          /* PARAM outside EPOCH_MIN..MAX; fills week */

#ifdef __cplusplus
}
#endif

#endif /* SX_EX_RTC_H */