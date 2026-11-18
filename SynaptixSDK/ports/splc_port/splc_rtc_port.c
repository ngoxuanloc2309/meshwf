#include "splc_rtc_port.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "stm32h5xx_hal.h"
#include "i2c.h"              /* hi2c1, MX_I2C1_Init() (CubeMX) */

#include "plc_rtc.h"
#include "splc_log.h"
#include "logger.h"
#include "sx_i2c.h"
#include "sx_time.h"
#include "sx_ex_rtc.h"

static const char *TAG = "SPLC_RTC";

/* A chip that did not answer is probed again after this long. A re-init blocks the engine task
 * for ~40 ms, so it must not run every second. */
#ifndef RTC_INIT_RETRY_MS
#define RTC_INIT_RETRY_MS       30000U
#endif

/* Consecutive failed hook calls with a BUS / TIMEOUT error (not a NACK) before the bus recovery
 * runs. The library calls the hooks about once a second. */
#ifndef RTC_BUS_ERR_THRESHOLD
#define RTC_BUS_ERR_THRESHOLD   3U
#endif

static sx_i2c_config_t s_i2c;
static rx8130ce_t      s_rtc;
static bool            s_tried;
static uint32_t        s_last_try_ms;
static bool            s_read_ok = true;      /* last get_epoch result, to log changes only */
static uint32_t        s_bus_errs;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ------------------------------------------------------------------------ */
/* I2C1 bus recovery (PB6 = SCL, PB7 = SDA, this board's only I2C bus)       */
/*                                                                           */
/* A slave can be left mid-byte (noise, brown-out, a transfer timing out     */
/* between clocks) and hold SDA low for good; HAL does not free the bus on   */
/* a timeout. Standard recovery: stop the peripheral, clock SCL up to 9      */
/* times as plain GPIO until SDA is released, issue a STOP, then bring the   */
/* peripheral back. MX_I2C1_Init() -> HAL_I2C_MspInit() puts the pins back   */
/* on the I2C1 alternate function, so no pin setup is repeated here.         */
/* Runs in the engine task, i.e. never overlaps a transfer.                  */
/* ------------------------------------------------------------------------ */
static void i2c1_bus_recover(void)
{
    GPIO_InitTypeDef gpio = {0};

    log_warn(TAG, "I2C1 bus stuck, running recovery");

    (void)HAL_I2C_DeInit(&hi2c1);
    __HAL_RCC_GPIOB_CLK_ENABLE();

    gpio.Pin   = GPIO_PIN_6;                    /* SCL: open drain, driven by hand */
    gpio.Mode  = GPIO_MODE_OUTPUT_OD;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);

    gpio.Pin  = GPIO_PIN_7;                     /* SDA: input, to see whether it is released */
    gpio.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(GPIOB, &gpio);

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
    sx_delay_ms(1U);

    for (int i = 0; i < 9; i++) {
        if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7) == GPIO_PIN_SET) {
            break;                              /* SDA released */
        }
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);
        sx_delay_ms(1U);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
        sx_delay_ms(1U);
    }

    /* Manual STOP: SDA low -> high while SCL is high */
    gpio.Pin  = GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_RESET);
    sx_delay_ms(1U);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
    sx_delay_ms(1U);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);
    sx_delay_ms(1U);

    MX_I2C1_Init();                             /* HAL init + MSP: pins back to I2C1 AF */
    (void)sx_i2c_init(&s_i2c);

    s_bus_errs = 0U;
}

/* Called at the end of every hook that may have touched the bus. A NACK means the bus works
 * (the chip is simply absent); only BUS / TIMEOUT count towards a recovery. */
static void bus_note(bool ok)
{
    const int err = s_rtc.last_i2c_err;

    if (ok || err == SX_I2C_ERR_NACK) {
        s_bus_errs = 0U;
        return;
    }
    if (err == SX_I2C_ERR_BUS || err == SX_I2C_ERR_TIMEOUT) {
        if (++s_bus_errs >= RTC_BUS_ERR_THRESHOLD) {
            i2c1_bus_recover();
        }
    }
}


/* ------------------------------------------------------------------------ */
/* TEMPORARY probe: does the RX8130CE have user RAM that survives power loss? */
/*                                                                           */
/* MESHWF_RTC_USERRAM_PROBE:                                                 */
/*   0 = off (default, no behaviour change)                                  */
/*   1 = READ ONLY: log registers 0x20..0x2F and the VLF flag, write nothing */
/*   2 = as 1, then if 0x20..0x23 do not hold the test pattern, write it and */
/*       read it back. After a FULL power cycle (all supplies off, coin cell */
/*       kept) the next boot logs whether the pattern survived.              */
/* Remove this block once the question is answered.                          */
/* ------------------------------------------------------------------------ */
#ifndef MESHWF_RTC_USERRAM_PROBE
#define MESHWF_RTC_USERRAM_PROBE 1
#endif

#if MESHWF_RTC_USERRAM_PROBE
#define URAM_REG      0x20U
#define URAM_LEN      4U
static const uint8_t URAM_PATTERN[URAM_LEN] = { 0xA5U, 0x5AU, 0xC3U, 0x3CU };

static void userram_probe(void)
{
    uint8_t d[16];
    uint8_t flag = 0xFFU;

    if (sx_i2c_mem_read(&s_i2c, RX8130CE_ADDR7, 0x1DU, 1U, &flag, 1U, RX8130CE_I2C_TIMEOUT_MS) != SX_I2C_OK) {
        log_info(TAG, "URAM probe: cannot read flag register");
        return;
    }
    if (sx_i2c_mem_read(&s_i2c, RX8130CE_ADDR7, URAM_REG, 1U, d, 16U, RX8130CE_I2C_TIMEOUT_MS) != SX_I2C_OK) {
        log_info(TAG, "URAM probe: cannot read 0x20..0x2F");
        return;
    }
    log_info(TAG, "URAM probe: VLF=%u 0x20..0x23 = %02X %02X %02X %02X | 0x24..0x27 = %02X %02X %02X %02X",
             (unsigned)((flag >> 1) & 1U), d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    log_info(TAG, "URAM probe: 0x28..0x2F = %02X %02X %02X %02X %02X %02X %02X %02X",
             d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);

#if MESHWF_RTC_USERRAM_PROBE >= 2
    if (memcmp(d, URAM_PATTERN, URAM_LEN) == 0) {
        log_info(TAG, "URAM probe: PATTERN PRESENT. It survived since the last write "
                      "(only counts as power-loss proof if every supply was off in between)");
        return;
    }
    uint8_t back[URAM_LEN] = {0};
    if (sx_i2c_mem_write(&s_i2c, RX8130CE_ADDR7, URAM_REG, 1U, (uint8_t *)URAM_PATTERN, URAM_LEN,
                         RX8130CE_I2C_TIMEOUT_MS) != SX_I2C_OK ||
        sx_i2c_mem_read(&s_i2c, RX8130CE_ADDR7, URAM_REG, 1U, back, URAM_LEN,
                        RX8130CE_I2C_TIMEOUT_MS) != SX_I2C_OK) {
        log_info(TAG, "URAM probe: write/readback I2C error");
        return;
    }
    if (memcmp(back, URAM_PATTERN, URAM_LEN) == 0) {
        log_info(TAG, "URAM probe: pattern written and read back OK. Now cut ALL power, wait 1 min, power up");
    } else {
        log_info(TAG, "URAM probe: readback %02X %02X %02X %02X != pattern: NOT writable RAM",
                 back[0], back[1], back[2], back[3]);
    }
#endif
}
#endif /* MESHWF_RTC_USERRAM_PROBE */

/* ------------------------------------------------------------------------ */
/* Chip bring-up                                                             */
/* ------------------------------------------------------------------------ */
static bool try_init(void)
{
    s_tried       = true;
    s_last_try_ms = now_ms();

    const int rc = rx8130ce_init(&s_rtc, &s_i2c);
    if (rc == RX8130CE_OK) {
        log_info(TAG, "RX8130CE ready");
#if MESHWF_RTC_USERRAM_PROBE
        static bool s_probed;
        if (!s_probed) {
            s_probed = true;
            userram_probe();
        }
#endif
        return true;
    }
    log_warn(TAG, "RX8130CE init failed (%d, i2c %d), retry in %u s", rc, s_rtc.last_i2c_err,
             (unsigned)(RTC_INIT_RETRY_MS / 1000U));
    bus_note(false);
    return false;
}

static bool ensure_ready(void)
{
    if (s_rtc.initialized) {
        return true;
    }
    if (s_tried && (uint32_t)(now_ms() - s_last_try_ms) < (uint32_t)RTC_INIT_RETRY_MS) {
        return false;
    }
    return try_init();
}

/* ------------------------------------------------------------------------ */
/* plc_rtc hooks (engine task)                                               */
/* ------------------------------------------------------------------------ */
static bool hook_get_epoch(void *ctx, uint32_t *epoch_utc_s)
{
    (void)ctx;
    bool ok = false;

    s_rtc.last_i2c_err = SX_I2C_OK;
    if (ensure_ready()) {
        rx8130ce_time_t t;
        const int rc = rx8130ce_get_time(&s_rtc, &t);
        ok = (rc == RX8130CE_OK) && (rx8130ce_time_to_epoch(&t, epoch_utc_s) == RX8130CE_OK);
        bus_note(ok);
    }

    if (ok != s_read_ok) {               /* log the change, not every second */
        s_read_ok = ok;
        if (ok) {
            log_info(TAG, "clock readable again");
        } else {
            log_warn(TAG, "clock not readable (no chip, I2C error, or no valid time stored yet)");
        }
    }
    return ok;
}

static bool hook_set_epoch(void *ctx, uint32_t epoch_utc_s)
{
    (void)ctx;

    s_rtc.last_i2c_err = SX_I2C_OK;
    if (!ensure_ready()) {
        return false;
    }

    rx8130ce_time_t t;
    if (rx8130ce_epoch_to_time(epoch_utc_s, &t) != RX8130CE_OK) {
        return false;
    }
    const int rc = rx8130ce_set_time(&s_rtc, &t);
    bus_note(rc == RX8130CE_OK);
    if (rc != RX8130CE_OK) {
        log_warn(TAG, "set time failed (%d)", rc);
        return false;
    }
    log_info(TAG, "time set: 20%02u-%02u-%02u %02u:%02u:%02u UTC",
             (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
             (unsigned)t.hour, (unsigned)t.min, (unsigned)t.sec);
    return true;
}

/* Trustworthy = VLF clear. The driver clears VLF only after a good write of the time. */
static bool hook_is_synced(void *ctx)
{
    (void)ctx;

    s_rtc.last_i2c_err = SX_I2C_OK;
    if (!ensure_ready()) {
        return false;
    }
    bool valid = false;
    const int rc = rx8130ce_is_time_valid(&s_rtc, &valid);
    bus_note(rc == RX8130CE_OK);
    return (rc == RX8130CE_OK) && valid;
}

void splc_rtc_port_init(void)
{
    s_i2c.pDriver = &hi2c1;
    (void)sx_i2c_init(&s_i2c);

    (void)try_init();

    static const plc_rtc_ops_t ops = {
        .ctx       = NULL,
        .get_epoch = hook_get_epoch,
        .set_epoch = hook_set_epoch,
        .is_synced = hook_is_synced,
    };
    if (!plc_rtc_register(&ops)) {
        log_error(TAG, "plc_rtc_register failed");
    }
}