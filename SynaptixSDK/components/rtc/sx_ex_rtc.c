#include "sx_ex_rtc.h"
#include "sx_time.h"    /* sx_delay_ms() */

#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static inline bool bcd_ok(uint8_t bcd)
{
    return ((bcd >> 4) <= 9U) && ((bcd & 0x0FU) <= 9U);
}

static inline uint8_t bcd_to_dec(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0FU));
}

static inline uint8_t dec_to_bcd(uint8_t dec)
{
    return (uint8_t)(((dec / 10U) << 4) | (dec % 10U));
}

/* 0 = ok. The raw result stays in dev->last_i2c_err. */
static int _reg_write(rx8130ce_t *dev, uint8_t reg, uint8_t val)
{
    dev->last_i2c_err = sx_i2c_mem_write(dev->i2c, RX8130CE_ADDR7, reg, 1U,
                                         &val, 1U, RX8130CE_I2C_TIMEOUT_MS);
    return (dev->last_i2c_err == SX_I2C_OK) ? 0 : -1;
}

static int _reg_read(rx8130ce_t *dev, uint8_t reg, uint8_t *buf, uint16_t len)
{
    dev->last_i2c_err = sx_i2c_mem_read(dev->i2c, RX8130CE_ADDR7, reg, 1U,
                                        buf, len, RX8130CE_I2C_TIMEOUT_MS);
    return (dev->last_i2c_err == SX_I2C_OK) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Calendar maths (proleptic Gregorian, days since 1970-01-01)       */
/* ------------------------------------------------------------------ */

static bool is_leap(unsigned y)
{
    return ((y % 4U) == 0U && (y % 100U) != 0U) || ((y % 400U) == 0U);
}

static unsigned days_in_month(unsigned year, unsigned month)
{
    static const uint8_t dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (month == 2U && is_leap(year)) ? 29U : dim[month - 1U];
}

static int32_t days_from_civil(int32_t y, int32_t m, int32_t d)
{
    y -= (m <= 2) ? 1 : 0;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const int32_t yoe = y - era * 400;
    const int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int32_t z, int32_t *y, int32_t *m, int32_t *d)
{
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int32_t doe = z - era * 146097;
    const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int32_t mp  = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = (mp < 10) ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + ((*m <= 2) ? 1 : 0);
}

static bool time_is_real(const rx8130ce_time_t *t)
{
    return t->sec <= 59U && t->min <= 59U && t->hour <= 23U &&
           t->month >= 1U && t->month <= 12U && t->year <= 99U &&
           t->day >= 1U && t->day <= days_in_month(2000U + t->year, t->month);
}

int rx8130ce_time_to_epoch(const rx8130ce_time_t *t, uint32_t *epoch_utc_s)
{
    if (t == NULL || epoch_utc_s == NULL || !time_is_real(t)) {
        return RX8130CE_ERR_PARAM;
    }
    const int32_t days = days_from_civil(2000 + (int32_t)t->year, (int32_t)t->month, (int32_t)t->day);
    *epoch_utc_s = (uint32_t)days * 86400UL + (uint32_t)t->hour * 3600UL +
                   (uint32_t)t->min * 60UL + (uint32_t)t->sec;
    return RX8130CE_OK;
}

int rx8130ce_epoch_to_time(uint32_t epoch_utc_s, rx8130ce_time_t *t)
{
    if (t == NULL || epoch_utc_s < RX8130CE_EPOCH_MIN || epoch_utc_s > RX8130CE_EPOCH_MAX) {
        return RX8130CE_ERR_PARAM;
    }
    const int32_t days = (int32_t)(epoch_utc_s / 86400UL);
    const uint32_t sod = epoch_utc_s % 86400UL;
    int32_t y, m, d;
    civil_from_days(days, &y, &m, &d);

    t->year  = (uint8_t)(y - 2000);
    t->month = (uint8_t)m;
    t->day   = (uint8_t)d;
    t->hour  = (uint8_t)(sod / 3600UL);
    t->min   = (uint8_t)((sod % 3600UL) / 60UL);
    t->sec   = (uint8_t)(sod % 60UL);
    t->week  = (uint8_t)(1U << (unsigned)((days + 4) % 7));   /* 1970-01-01 was a Thursday; Sun = bit 0 */
    return RX8130CE_OK;
}

/* ------------------------------------------------------------------ */
/*  Software Reset sequence (datasheet 18.2)                           */
/* ------------------------------------------------------------------ */

static int _software_reset(rx8130ce_t *dev)
{
    uint8_t dummy;

    /* Steps 4-10 from the datasheet software-reset flow */
    if (_reg_write(dev, 0x1E, 0x00) != 0) return RX8130CE_ERR_I2C;
    if (_reg_write(dev, 0x1E, 0x80) != 0) return RX8130CE_ERR_I2C;
    if (_reg_write(dev, 0x50, 0x6C) != 0) return RX8130CE_ERR_I2C;
    if (_reg_write(dev, 0x53, 0x01) != 0) return RX8130CE_ERR_I2C;
    if (_reg_write(dev, 0x66, 0x03) != 0) return RX8130CE_ERR_I2C;
    if (_reg_write(dev, 0x6B, 0x02) != 0) return RX8130CE_ERR_I2C;
    if (_reg_write(dev, 0x6B, 0x01) != 0) return RX8130CE_ERR_I2C;

    /* Step 12: wait 125 ms for the reset to complete */
    sx_delay_ms(125U);

    /* Dummy read after reset: result ignored */
    (void)_reg_read(dev, RX8130CE_REG_FLAG, &dummy, 1U);

    return RX8130CE_OK;
}

/* ------------------------------------------------------------------ */
/*  Initialisation                                                     */
/* ------------------------------------------------------------------ */

int rx8130ce_init(rx8130ce_t *dev, sx_i2c_config_t *i2c)
{
    if (dev == NULL || i2c == NULL) return RX8130CE_ERR_PARAM;

    dev->i2c          = i2c;
    dev->initialized  = false;
    dev->last_i2c_err = SX_I2C_OK;

    /* Oscillation start-up, per the datasheet (>= 30 ms after VDD is applied). Intrinsic to the
     * chip, not to how power was turned on. */
    sx_delay_ms(40U);

    /* Dummy read: ACK/NACK ignored */
    {
        uint8_t dummy;
        (void)_reg_read(dev, RX8130CE_REG_FLAG, &dummy, 1U);
    }

    /* Check VLF. When set, the chip lost power / oscillation: software reset. VLF itself is
     * NOT cleared here (see sx_ex_rtc.h): only rx8130ce_set_time() clears it. */
    uint8_t flag = 0;
    if (_reg_read(dev, RX8130CE_REG_FLAG, &flag, 1U) != 0) return RX8130CE_ERR_I2C;

    if ((flag & RX8130CE_FLAG_VLF) != 0U) {
        if (_software_reset(dev) != RX8130CE_OK) return RX8130CE_ERR_I2C;
    }

    /* Control Register 1 (0x1F): INIEN = 1 (power switchover), CHGEN per RX8130CE_CHARGE_ENABLE */
    uint8_t ctrl1 = 0;
    if (_reg_read(dev, RX8130CE_REG_CTRL1, &ctrl1, 1U) != 0) return RX8130CE_ERR_I2C;

    ctrl1 |= RX8130CE_CTRL1_INIEN;
#if RX8130CE_CHARGE_ENABLE
    ctrl1 |= RX8130CE_CTRL1_CHGEN;
#else
    ctrl1 &= (uint8_t)~RX8130CE_CTRL1_CHGEN;
#endif
    if (_reg_write(dev, RX8130CE_REG_CTRL1, ctrl1) != 0) return RX8130CE_ERR_I2C;

    /* Control Register 0 (0x1E): TEST = AIE = TIE = UIE = 0, STOP = 0 (timekeeping running) */
    if (_reg_write(dev, RX8130CE_REG_CTRL0, 0x00) != 0) return RX8130CE_ERR_I2C;

    /* Clear TE in the Extension Register (0x1C): timer not used */
    uint8_t ext = 0;
    if (_reg_read(dev, RX8130CE_REG_EXT, &ext, 1U) != 0) return RX8130CE_ERR_I2C;

    ext &= (uint8_t)~RX8130CE_EXT_TE;
    if (_reg_write(dev, RX8130CE_REG_EXT, ext) != 0) return RX8130CE_ERR_I2C;

    dev->initialized = true;
    return RX8130CE_OK;
}

/* ------------------------------------------------------------------ */
/*  Set time                                                           */
/* ------------------------------------------------------------------ */

int rx8130ce_set_time(rx8130ce_t *dev, const rx8130ce_time_t *t)
{
    if (dev == NULL || t == NULL) return RX8130CE_ERR_PARAM;
    if (!dev->initialized)        return RX8130CE_ERR_NOT_INIT;
    if (!time_is_real(t))         return RX8130CE_ERR_PARAM;

    /* The week bitmask is derived from the date, not taken from the caller. */
    uint32_t epoch;
    rx8130ce_time_t norm;
    if (rx8130ce_time_to_epoch(t, &epoch) != RX8130CE_OK) return RX8130CE_ERR_PARAM;
    if (rx8130ce_epoch_to_time(epoch, &norm) != RX8130CE_OK) return RX8130CE_ERR_PARAM;

    /* STOP = 1 freezes the counters during the write */
    uint8_t ctrl0 = 0;
    if (_reg_read(dev, RX8130CE_REG_CTRL0, &ctrl0, 1U) != 0) return RX8130CE_ERR_I2C;
    ctrl0 |= RX8130CE_CTRL0_STOP;
    if (_reg_write(dev, RX8130CE_REG_CTRL0, ctrl0) != 0) return RX8130CE_ERR_I2C;

    uint8_t buf[7];
    buf[0] = dec_to_bcd(norm.sec);
    buf[1] = dec_to_bcd(norm.min);
    buf[2] = dec_to_bcd(norm.hour);
    buf[3] = norm.week;                 /* bitmask, not BCD */
    buf[4] = dec_to_bcd(norm.day);
    buf[5] = dec_to_bcd(norm.month);
    buf[6] = dec_to_bcd(norm.year);

    dev->last_i2c_err = sx_i2c_mem_write(dev->i2c, RX8130CE_ADDR7, RX8130CE_REG_SEC, 1U,
                                         buf, 7U, RX8130CE_I2C_TIMEOUT_MS);
    const bool wrote = (dev->last_i2c_err == SX_I2C_OK);

    /* STOP = 0 (restart the clock) whatever the write result was */
    ctrl0 &= (uint8_t)~RX8130CE_CTRL0_STOP;
    const int rc_run = _reg_write(dev, RX8130CE_REG_CTRL0, ctrl0);

    if (!wrote || rc_run != 0) return RX8130CE_ERR_I2C;

    /* The time is now real: clear VLF so is_time_valid() reports it. Only after a good write. */
    uint8_t flag = 0;
    if (_reg_read(dev, RX8130CE_REG_FLAG, &flag, 1U) != 0) return RX8130CE_ERR_I2C;
    flag &= (uint8_t)~RX8130CE_FLAG_VLF;
    if (_reg_write(dev, RX8130CE_REG_FLAG, flag) != 0) return RX8130CE_ERR_I2C;

    return RX8130CE_OK;
}

/* ------------------------------------------------------------------ */
/*  Get time                                                           */
/* ------------------------------------------------------------------ */

int rx8130ce_get_time(rx8130ce_t *dev, rx8130ce_time_t *t)
{
    if (dev == NULL || t == NULL) return RX8130CE_ERR_PARAM;
    if (!dev->initialized)        return RX8130CE_ERR_NOT_INIT;

    uint8_t buf[7];
    if (_reg_read(dev, RX8130CE_REG_SEC, buf, 7U) != 0) return RX8130CE_ERR_I2C;

    /* Mask unused bits before BCD conversion (datasheet table 12); reject non-BCD digits. */
    const uint8_t sec = buf[0] & 0x7FU, min = buf[1] & 0x7FU, hour = buf[2] & 0x3FU;
    const uint8_t day = buf[4] & 0x3FU, mon = buf[5] & 0x1FU, year = buf[6];
    if (!bcd_ok(sec) || !bcd_ok(min) || !bcd_ok(hour) || !bcd_ok(day) || !bcd_ok(mon) || !bcd_ok(year)) {
        return RX8130CE_ERR_DATA;
    }

    t->sec   = bcd_to_dec(sec);
    t->min   = bcd_to_dec(min);
    t->hour  = bcd_to_dec(hour);
    t->week  = buf[3] & 0x7FU;          /* bitmask, not BCD */
    t->day   = bcd_to_dec(day);
    t->month = bcd_to_dec(mon);
    t->year  = bcd_to_dec(year);

    return time_is_real(t) ? RX8130CE_OK : RX8130CE_ERR_DATA;
}

/* ------------------------------------------------------------------ */
/*  VLF check                                                          */
/* ------------------------------------------------------------------ */

int rx8130ce_is_time_valid(rx8130ce_t *dev, bool *valid)
{
    if (dev == NULL || valid == NULL) return RX8130CE_ERR_PARAM;
    if (!dev->initialized)            return RX8130CE_ERR_NOT_INIT;

    uint8_t flag = 0;
    if (_reg_read(dev, RX8130CE_REG_FLAG, &flag, 1U) != 0) return RX8130CE_ERR_I2C;

    *valid = ((flag & RX8130CE_FLAG_VLF) == 0U);
    return RX8130CE_OK;
}