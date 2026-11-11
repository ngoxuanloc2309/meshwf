/*
 * st67w611_spi_port.c - ST67W611 SPI / pin port for the meshwf board.
 *
 * Hardware: STM32H523, SPI1 master (software CS), RDY on EXTI, WAKEUP GPIO,
 * optional EN / BOOT GPIOs. SPI transfers and CS go through simple_plc's
 * sx_spi (Layer 1); RDY EXTI, WAKEUP, EN, BOOT and the optional GPDMA path stay
 * on the HAL because sx_spi/sx_gpio have no interrupt or DMA support. See st67w611_spi_port_conf.h for the wiring and the
 * reasons behind every default. No CubeMX-generated file is modified.
 */
#include "st67w611_spi_port.h"
#include "st67w611_spi_port_conf.h"
#include "st67w611_port_events.h"
#include "st67w611_vendor_bridge.h"
#include "sx_gpio.h"
#include "sx_spi.h"

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

_Static_assert(ST67_RDY_IRQ_PRIORITY >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY,
               "RDY EXTI priority is above configMAX_SYSCALL_INTERRUPT_PRIORITY: "
               "the vendor ISR path calls FreeRTOS FromISR functions");
_Static_assert(ST67_SPI_MAX_XFER >= 1536U && ST67_SPI_MAX_XFER <= 65535U,
               "ST67_SPI_MAX_XFER must cover a Wi-Fi frame and fit uint16_t");

/* ------------------------------------------------------------------ state */

static st67w611_transfer_complete_cb_t s_complete_cb;
static void                           *s_complete_ctx;

static volatile uint8_t s_inited;
static volatile uint8_t s_busy;          /* a polled transfer is running */
static volatile uint8_t s_dma_active;
static volatile uint8_t s_dma_cancelled;

static struct {
    volatile uint32_t transfer_count;
    volatile uint32_t timeout_count;
    volatile uint32_t error_count;
    volatile uint32_t dma_error_count;
    volatile uint32_t rdy_rising;
    volatile uint32_t rdy_falling;
    volatile uint32_t en_release_tick;   /* tick of the deferred EN release (0 = not yet) */
    volatile uint32_t first_rise_tick;   /* tick of the first RDY rising edge after it */
    volatile uint32_t gate_open_tick;    /* tick the first-transfer gate was opened (0 = closed) */
    volatile uint32_t gate_wait_ms;      /* how long the engine waited at the gate */
    volatile uint32_t throttled;         /* times the engine was forced to yield (hot loop) */
    volatile uint8_t  where;             /* breadcrumb: last place reached (see dbg_at) */
    volatile uint16_t last_len;          /* length of the last transfer entered */
} s_diag;

/* Breadcrumb for diagnosis without a debugger: 1 port_init entered, 2 SPI up, 3 RDY armed,
 * 4 gate armed (port_init done), 5 engine waits at the gate, 6 gate released,
 * 7/8 port_transfer in/out, 9/10 port_transfer_dma in/out, 11 cancel. */
#define DBG_AT(n)  (s_diag.where = (uint8_t)(n))

/* TX source for RX-only transfers (zeroed at init, never written afterwards) and
 * sink for TX-only transfers. */
static uint8_t s_zeros[ST67_SPI_MAX_XFER];
static uint8_t s_sink[ST67_SPI_MAX_XFER];

/* sx_spi: SPI1 transfers + software CS (polarity, setup time, minimum idle gap). */
static sx_spi_t        s_spi;
static sx_spi_config_t s_spi_cfg;
static sx_gpio_pin_t   s_cs_pin = { ST67SPI_CS_PORT, ST67SPI_CS_PIN, SX_GPIO_MODE_OUTPUT_PP };

/* ----------------------------------------------------------------- timing */

static void delay_ms_os(uint32_t ms)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        HAL_Delay(ms);
    }
}

/* ------------------------------------------------------------------- pins */

static void hw_pins_safe_state(void)
{
    sx_spi_deselect(&s_spi);
#if ST67_HAS_BOOT
    HAL_GPIO_WritePin(ST67SPI_BOOT_PORT, ST67SPI_BOOT_PIN, GPIO_PIN_RESET);   /* boot from flash */
#endif
    HAL_GPIO_WritePin(ST67SPI_WAKEUP_PORT, ST67SPI_WAKEUP_PIN, GPIO_PIN_SET); /* keep module awake */
}

#if ST67_HAS_EN
static void module_reset_pulse(void)
{
    hw_pins_safe_state();
    HAL_GPIO_WritePin(ST67SPI_EN_PORT, ST67SPI_EN_PIN, GPIO_PIN_RESET);
    delay_ms_os(ST67_EN_LOW_MS);
    HAL_GPIO_WritePin(ST67SPI_EN_PORT, ST67SPI_EN_PIN, GPIO_PIN_SET);
    delay_ms_os(ST67_BOOT_WAIT_MS);
}
#endif

#if ST67_HAS_EN && ST67_DEFER_EN_RELEASE
static TimerHandle_t s_en_timer;

/* Timer task context: only a GPIO write, no blocking. */
static void en_release_cb(TimerHandle_t t)
{
    (void)t;
    s_diag.en_release_tick = (uint32_t)xTaskGetTickCount();
    HAL_GPIO_WritePin(ST67SPI_EN_PORT, ST67SPI_EN_PIN, GPIO_PIN_SET);
}

/* Hold the module in reset now, release EN later (see ST67_DEFER_EN_RELEASE). */
static int module_reset_deferred(void)
{
    hw_pins_safe_state();
    HAL_GPIO_WritePin(ST67SPI_EN_PORT, ST67SPI_EN_PIN, GPIO_PIN_RESET);
    s_diag.en_release_tick = 0U;
    s_diag.first_rise_tick = 0U;
    /* A module that stayed powered across an MCU reset may still drive RDY high for a
     * moment: wait until it drops, otherwise spi_transaction_init() would see RDY high
     * and start a transfer against a module in reset. This wait (vTaskDelay) must come
     * BEFORE the timer is started: from the start on, the timer may run whenever this
     * task blocks. After it, nothing blocks until W61_WaitForReady(), i.e. after the
     * vendor spi_bind(). */
    for (uint32_t i = 0U; i < 20U &&
         HAL_GPIO_ReadPin(ST67SPI_RDY_PORT, ST67SPI_RDY_PIN) == GPIO_PIN_SET; i++) {
        delay_ms_os(1U);
    }
    if (s_en_timer == NULL) {
        s_en_timer = xTimerCreate("st67_en", pdMS_TO_TICKS(ST67_EN_RELEASE_DELAY_MS),
                                  pdFALSE, NULL, en_release_cb);
    }
    if (s_en_timer == NULL || xTimerStart(s_en_timer, pdMS_TO_TICKS(100U)) != pdPASS) {
        /* No timer: fall back to the old synchronous pulse rather than leave EN low. */
        ST67_LOG("st67: EN timer failed, synchronous reset");
        module_reset_pulse();
    }
    return 0;
}
#endif

#if ST67_GATE_FIRST_XFER
static volatile uint8_t s_gate_open = 1U;   /* open until port_init() closes it */
static TimerHandle_t    s_gate_timer;

/* Timer task context: runs only when every higher-priority task is blocked. */
static void gate_cb(TimerHandle_t t)
{
    (void)t;
    s_diag.gate_open_tick = (uint32_t)xTaskGetTickCount();
    s_gate_open = 1U;
}

/* Called at the very end of port_init(): nothing may block after this until the
 * bring-up task waits for "ready". */
static void gate_arm(void)
{
    s_diag.gate_open_tick = 0U;
    s_diag.gate_wait_ms   = 0U;
    if (s_gate_timer == NULL) {
        s_gate_timer = xTimerCreate("st67_gate", pdMS_TO_TICKS(ST67_GATE_DELAY_MS),
                                    pdFALSE, NULL, gate_cb);
    }
    if (s_gate_timer == NULL) {
        s_gate_open = 1U;                     /* no timer: old behaviour */
        return;
    }
    s_gate_open = 0U;
    if (xTimerStart(s_gate_timer, 0U) != pdPASS) {
        s_gate_open = 1U;
    }
}

/* Engine task context (port_set_cs). Waits with vTaskDelay so lower-priority
 * tasks (the bring-up task, then the timer task) can run. */
static void gate_wait(void)
{
    uint32_t waited = 0U;
    if (s_gate_open == 0U) {
        DBG_AT(5);
    }
    while (s_gate_open == 0U && waited < ST67_GATE_MAX_WAIT_MS) {
        delay_ms_os(1U);
        waited++;
    }
    if (waited != 0U) {
        DBG_AT(6);
        s_diag.gate_wait_ms = waited;
        if (s_gate_open == 0U) {
            ST67_LOG("st67 gate: not opened after %u ms, continuing", (unsigned)waited);
            s_gate_open = 1U;
        }
    }
}
#endif /* ST67_GATE_FIRST_XFER */

/* ---------------------------------------------------------------- RDY edge */

void meshwf_st67_port_on_rdy_edge(uint16_t gpio_pin, bool rising)
{
    if (gpio_pin != ST67SPI_RDY_PIN) {
        return;
    }
    if (rising) {
        if (s_diag.first_rise_tick == 0U) {
            s_diag.first_rise_tick = (uint32_t)xTaskGetTickCountFromISR();
        }
        s_diag.rdy_rising++;
        st67w611_port_notify_ready_rising();
    } else {
        s_diag.rdy_falling++;
        st67w611_port_notify_ready_falling();
    }
}

#if ST67_DEFINE_EXTI_CALLBACKS
void HAL_GPIO_EXTI_Rising_Callback(uint16_t gpio_pin)
{
    meshwf_st67_port_on_rdy_edge(gpio_pin, true);
}

void HAL_GPIO_EXTI_Falling_Callback(uint16_t gpio_pin)
{
    meshwf_st67_port_on_rdy_edge(gpio_pin, false);
}
#endif

#if ST67_DEFINE_RDY_IRQ_HANDLER
_Static_assert(ST67SPI_RDY_PIN == GPIO_PIN_0 || !ST67_RDY_RECONFIGURE ||
               ST67_RDY_EXTI_IRQn != EXTI0_IRQn,
               "RDY is not on pin 0: set ST67_RDY_EXTI_IRQn / ST67_RDY_EXTI_IRQHandler");
void ST67_RDY_EXTI_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(ST67SPI_RDY_PIN);
}
#endif

static void rdy_irq_arm(void)
{
#if ST67_RDY_RECONFIGURE
    GPIO_InitTypeDef g = {0};
    g.Pin  = ST67SPI_RDY_PIN;
    g.Mode = GPIO_MODE_IT_RISING_FALLING;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(ST67SPI_RDY_PORT, &g);
    HAL_NVIC_SetPriority(ST67_RDY_EXTI_IRQn, ST67_RDY_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(ST67_RDY_EXTI_IRQn);
#endif
}

static void rdy_irq_disarm(void)
{
#if ST67_RDY_RECONFIGURE
    HAL_NVIC_DisableIRQ(ST67_RDY_EXTI_IRQn);
#endif
}

/* --------------------------------------------------------- module presence */

bool meshwf_st67_module_present(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin   = ST67SPI_RDY_PIN;
    g.Mode  = GPIO_MODE_INPUT;      /* plain input: no EXTI while probing */
    g.Speed = GPIO_SPEED_FREQ_LOW;

    bool present = true;
    for (uint32_t round = 0U; round < 3U; round++) {
        g.Pull = GPIO_PULLDOWN;
        HAL_GPIO_Init(ST67SPI_RDY_PORT, &g);
        delay_ms_os(2U);
        GPIO_PinState with_pulldown = HAL_GPIO_ReadPin(ST67SPI_RDY_PORT, ST67SPI_RDY_PIN);

        g.Pull = GPIO_PULLUP;
        HAL_GPIO_Init(ST67SPI_RDY_PORT, &g);
        delay_ms_os(2U);
        GPIO_PinState with_pullup = HAL_GPIO_ReadPin(ST67SPI_RDY_PORT, ST67SPI_RDY_PIN);

        if (with_pulldown != with_pullup) {   /* the pin follows our pull: nobody drives it */
            present = false;
            break;
        }
    }

    g.Pull = present ? GPIO_NOPULL : GPIO_PULLDOWN;   /* floating RDY must not be left open */
    HAL_GPIO_Init(ST67SPI_RDY_PORT, &g);
    return present;
}

/* --------------------------------------------------------------- transfers */

/* Polled full-duplex transfer; a missing side uses the zero source / the sink. */
static int transfer_polled(const void *tx, void *rx, uint16_t len, uint32_t timeout_ms)
{
    const uint8_t *txb = (tx != NULL) ? (const uint8_t *)tx : s_zeros;
    uint8_t       *rxb = (rx != NULL) ? (uint8_t *)rx       : s_sink;
    int rc = sx_spi_transfer(&s_spi, txb, rxb, len, timeout_ms);
    s_diag.transfer_count++;
    if (rc == SX_SPI_ERR_TIMEOUT) {
        s_diag.timeout_count++;
    }
    if (rc != SX_SPI_OK) {
        s_diag.error_count++;
        ST67_LOG("st67 spi polled xfer failed (%d) len=%u", rc, (unsigned)len);
        return -1;
    }
    return 0;
}

static int port_init(void *context, st67w611_transfer_complete_cb_t cb, void *cb_ctx)
{
    (void)context;
    SPI_HandleTypeDef *h = ST67_SPI_HANDLE;
    DBG_AT(1);

    s_complete_cb  = cb;
    s_complete_ctx = cb_ctx;
    s_dma_active = 0U;
    s_dma_cancelled = 0U;
    s_busy = 0U;

    if (h->Instance == NULL || h->State == HAL_SPI_STATE_RESET) {
        ST67_LOG("st67 spi: SPI handle not initialised by CubeMX");
        return -1;
    }
#if ST67_SPI_USE_DMA
    if (h->hdmatx == NULL || h->hdmarx == NULL) {
        ST67_LOG("st67 spi: ST67_SPI_USE_DMA=1 but SPI1 has no GPDMA channels linked");
        return -1;
    }
#endif

    memset(&s_spi_cfg, 0, sizeof(s_spi_cfg));
    s_spi_cfg.pDriver        = h;
    s_spi_cfg.clock_hz       = ST67_SPI_CLOCK_HZ;
    s_spi_cfg.mode           = ST67_SPI_MODE;
    s_spi_cfg.cs             = &s_cs_pin;
    s_spi_cfg.cs_active_high = (ST67_CS_ACTIVE_HIGH != 0);
    s_spi_cfg.cs_setup_us    = (uint16_t)ST67_CS_SETUP_US;
    s_spi_cfg.cs_gap_us      = (uint16_t)ST67_CS_MIN_GAP_US;
    if (sx_spi_init(&s_spi, &s_spi_cfg) != SX_SPI_OK) {    /* also: keep-IO-state, CS idle level */
        ST67_LOG("st67 spi: sx_spi_init failed");
        return -1;
    }
    DBG_AT(2);
    rdy_irq_arm();          /* before the power sequence so no RDY edge is missed */
    DBG_AT(3);
    memset(s_zeros, 0, sizeof(s_zeros));

    s_inited = 1U;          /* before EN is released: a transfer may start at once */
#if ST67_HAS_EN && ST67_DEFER_EN_RELEASE
    (void)module_reset_deferred();
#elif ST67_HAS_EN
    module_reset_pulse();
#else
    hw_pins_safe_state();   /* no EN pin: the module was powered with the board */
#endif
#if ST67_GATE_FIRST_XFER
    gate_arm();             /* last thing: no blocking call may follow in this function */
#endif
    DBG_AT(4);
    return 0;
}

static int port_deinit(void *context)
{
    (void)context;
#if ST67_SPI_USE_DMA
    (void)HAL_SPI_Abort(ST67_SPI_HANDLE);
#endif
    s_dma_active = 0U;
    s_inited = 0U;
    s_complete_cb = NULL;
    s_complete_ctx = NULL;
    rdy_irq_disarm();
    sx_spi_deselect(&s_spi);
#if ST67_HAS_EN
    HAL_GPIO_WritePin(ST67SPI_EN_PORT, ST67SPI_EN_PIN, GPIO_PIN_RESET);
#endif
#if ST67_HAS_BOOT
    HAL_GPIO_WritePin(ST67SPI_BOOT_PORT, ST67SPI_BOOT_PIN, GPIO_PIN_RESET);
#endif
    HAL_GPIO_WritePin(ST67SPI_WAKEUP_PORT, ST67SPI_WAKEUP_PIN, GPIO_PIN_RESET);
    return 0;
}

static int port_transfer(void *context, const void *tx, void *rx,
                         uint16_t length, uint32_t timeout_ms)
{
    (void)context;
    if (!s_inited || length > ST67_SPI_MAX_XFER || s_busy || s_dma_active) {
        return -1;
    }
    if (length == 0U) {
        return 0;
    }
    s_diag.last_len = length;
    DBG_AT(7);
    s_busy = 1U;
    int r = transfer_polled(tx, rx, length, timeout_ms);
    s_busy = 0U;
    DBG_AT(8);
    return r;
}

static int port_transfer_dma(void *context, const void *tx, void *rx, uint16_t length)
{
    (void)context;
    if (!s_inited || length > ST67_SPI_MAX_XFER || s_busy || s_dma_active) {
        return -1;
    }
    if (length == 0U) {
        return 0;
    }
#if ST67_SPI_USE_DMA
    const uint8_t *txb = (tx != NULL) ? (const uint8_t *)tx : s_zeros;
    uint8_t       *rxb = (rx != NULL) ? (uint8_t *)rx       : s_sink;
    s_dma_cancelled = 0U;
    s_dma_active = 1U;
    s_diag.transfer_count++;
    if (HAL_SPI_TransmitReceive_DMA(ST67_SPI_HANDLE, (uint8_t *)txb, rxb, length) != HAL_OK) {
        s_dma_active = 0U;
        s_diag.dma_error_count++;
        return -1;
    }
    return 0;
#else
    /* No DMA: do it polled, then report completion. The vendor engine sets its
     * HW_XFER_DONE event from the callback and waits for it afterwards, so a
     * completion that arrives before the wait is not lost. On failure return -1
     * WITHOUT calling back (a callback would leave a stale event bit). */
    s_diag.last_len = length;
    DBG_AT(9);
    s_busy = 1U;
    int r = transfer_polled(tx, rx, length, 1000U);
    s_busy = 0U;
    DBG_AT(10);
    if (r == 0 && s_complete_cb != NULL) {
        s_complete_cb(s_complete_ctx, 0);
    }
    return r;
#endif
}

static int port_cancel_transfer(void *context)
{
    (void)context;
    s_dma_cancelled = 1U;
    s_dma_active = 0U;
#if ST67_SPI_USE_DMA
    return (HAL_SPI_Abort(ST67_SPI_HANDLE) == HAL_OK) ? 0 : -1;
#else
    return 0;                 /* polled transfers are synchronous: nothing in flight */
#endif
}

static int port_set_cs(void *context, bool asserted)
{
    (void)context;
    if (asserted) {
#if ST67_GATE_FIRST_XFER
        gate_wait();
#endif
        /* Hot-loop guard. The vendor engine (priority above SPLC and the diag task) loops
         * while RDY is high and skips its blocking ack wait when a transfer fails or the
         * header is invalid: that would starve every lower-priority task (no log at all).
         * More than ST67_MAX_CS_PER_TICK assertions in one tick -> sleep one tick. */
        static TickType_t s_last_tick;
        static uint32_t   s_same_tick;
        TickType_t now = xTaskGetTickCount();
        if (now == s_last_tick) {
            if (++s_same_tick >= ST67_MAX_CS_PER_TICK) {
                s_same_tick = 0U;
                s_diag.throttled++;
                delay_ms_os(1U);
            }
        } else {
            s_last_tick = now;
            s_same_tick = 0U;
        }
        sx_spi_select(&s_spi);      /* honours the minimum idle gap, then the setup time */
    } else {
        sx_spi_deselect(&s_spi);
    }
    return 0;
}

static bool port_read_ready(void *context)
{
    (void)context;
    return HAL_GPIO_ReadPin(ST67SPI_RDY_PORT, ST67SPI_RDY_PIN) == GPIO_PIN_SET;
}

static int port_hardware_reset(void *context, uint32_t timeout_ms)
{
    (void)context;
    (void)timeout_ms;
#if ST67_HAS_EN
    if (!s_inited) {
        return -1;
    }
    module_reset_pulse();
    return 0;
#else
    ST67_LOG("st67: no EN pin on this board, hardware reset unavailable");
    return -1;
#endif
}

static int port_set_wakeup(void *context, bool enabled)
{
    (void)context;
    HAL_GPIO_WritePin(ST67SPI_WAKEUP_PORT, ST67SPI_WAKEUP_PIN,
                      enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return 0;
}

#if ST67_HAS_EN
static int port_set_enable(void *context, bool enabled)
{
    (void)context;
    HAL_GPIO_WritePin(ST67SPI_EN_PORT, ST67SPI_EN_PIN, enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return 0;
}
#endif

#if ST67_HAS_BOOT
static int port_set_boot(void *context, bool enabled)
{
    (void)context;
    HAL_GPIO_WritePin(ST67SPI_BOOT_PORT, ST67SPI_BOOT_PIN, enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return 0;
}
#endif

static void port_delay_ms(void *context, uint32_t delay_ms)
{
    (void)context;
    delay_ms_os(delay_ms);
}

static int port_get_diagnostics(void *context, st67w611_port_diagnostics_t *d)
{
    (void)context;
    if (d == NULL) {
        return -1;
    }
    d->transfer_count      = s_diag.transfer_count;
    d->timeout_count       = s_diag.timeout_count;
    d->error_count         = s_diag.error_count;
    d->dma_error_count     = s_diag.dma_error_count;
    d->ready_rising_count  = s_diag.rdy_rising;
    d->ready_falling_count = s_diag.rdy_falling;
    return 0;
}

void meshwf_st67_port_get_diag(meshwf_st67_diag_t *d)
{
    if (d == NULL) {
        return;
    }
    d->rdy_rising      = s_diag.rdy_rising;
    d->rdy_falling     = s_diag.rdy_falling;
    d->transfers       = s_diag.transfer_count;
    d->timeouts        = s_diag.timeout_count;
    d->errors          = s_diag.error_count;
    d->en_release_tick = s_diag.en_release_tick;
    d->first_rise_tick = s_diag.first_rise_tick;
    d->gate_open_tick  = s_diag.gate_open_tick;
    d->gate_wait_ms    = s_diag.gate_wait_ms;
    d->throttled       = s_diag.throttled;
    d->where           = s_diag.where;
    d->last_len        = s_diag.last_len;

    st67w611_vendor_spi_stats_t v;
    d->vendor_ok = (st67w611_vendor_bridge_get_spi_stats(&v) == 0) ? 1U : 0U;
    if (d->vendor_ok != 0U) {
        d->v_tx_pkts = v.tx_pkts;   d->v_rx_pkts = v.rx_pkts;
        d->v_rx_drop = v.rx_drop;   d->v_io_err  = v.io_err;
        d->v_hdr_err = v.hdr_err;   d->v_mem_err = v.mem_err;
        d->v_wait_txn_to = v.wait_txn_timeouts;
        d->v_wait_ack_to = v.wait_hdr_ack_timeouts;
    }
}

const st67w611_port_ops_t meshwf_st67_port_ops = {
    .init            = port_init,
    .deinit          = port_deinit,
    .transfer        = port_transfer,
    .transfer_dma    = port_transfer_dma,
    .cancel_transfer = port_cancel_transfer,
    .set_cs          = port_set_cs,
#if ST67_HAS_EN
    .set_enable      = port_set_enable,
#else
    .set_enable      = NULL,
#endif
#if ST67_HAS_BOOT
    .set_boot        = port_set_boot,
#else
    .set_boot        = NULL,
#endif
    .set_wakeup      = port_set_wakeup,
    .read_ready      = port_read_ready,
    .hardware_reset  = port_hardware_reset,
    .delay_ms        = port_delay_ms,
    .get_diagnostics = port_get_diagnostics,
};

void meshwf_st67_port_get_pins(meshwf_st67_pins_t *p)
{
    if (p == NULL) {
        return;
    }
    p->cs     = (HAL_GPIO_ReadPin(ST67SPI_CS_PORT, ST67SPI_CS_PIN) == GPIO_PIN_SET) ? 1 : 0;
    p->rdy    = (HAL_GPIO_ReadPin(ST67SPI_RDY_PORT, ST67SPI_RDY_PIN) == GPIO_PIN_SET) ? 1 : 0;
    p->wakeup = (HAL_GPIO_ReadPin(ST67SPI_WAKEUP_PORT, ST67SPI_WAKEUP_PIN) == GPIO_PIN_SET) ? 1 : 0;
#if ST67_HAS_EN
    p->en     = (HAL_GPIO_ReadPin(ST67SPI_EN_PORT, ST67SPI_EN_PIN) == GPIO_PIN_SET) ? 1 : 0;
#else
    p->en     = -1;
#endif
#if ST67_HAS_BOOT
    p->boot   = (HAL_GPIO_ReadPin(ST67SPI_BOOT_PORT, ST67SPI_BOOT_PIN) == GPIO_PIN_SET) ? 1 : 0;
#else
    p->boot   = -1;
#endif
}

#if ST67_SPI_USE_DMA
/* Overrides the weak HAL callbacks (HAL stores no user context for them). */
static void dma_done(int status)
{
    if (!s_dma_active || s_dma_cancelled) {
        return;
    }
    s_dma_active = 0U;
    if (status != 0) {
        s_diag.dma_error_count++;
    }
    if (s_complete_cb != NULL) {
        s_complete_cb(s_complete_ctx, status);
    }
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == ST67_SPI_HANDLE) {
        dma_done(0);
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == ST67_SPI_HANDLE) {
        dma_done(-1);
    }
}
#endif