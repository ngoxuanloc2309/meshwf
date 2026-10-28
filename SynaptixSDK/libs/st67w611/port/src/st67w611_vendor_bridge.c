#include "st67w611_port.h"
#include "st67w611_vendor_bridge.h"
#include "st67w611_port_events.h"
#include "spi_port.h"
#include "spi_iface.h"
#include <string.h>
#include "FreeRTOS.h"
#include "timers.h"

static const st67w611_port_ops_t *bridge_ops;
static void *bridge_context;
static spi_transaction_complete_t bridge_complete;
static volatile uint8_t bridge_peer_failed;
static volatile uint8_t bridge_dma_active;
static TimerHandle_t bridge_dma_timer;

static void bridge_dma_timeout(TimerHandle_t timer)
{
    (void)timer;
    if (bridge_dma_active != 0U && bridge_ops != NULL &&
        bridge_ops->cancel_transfer != NULL) {
        (void)bridge_ops->cancel_transfer(bridge_context);
        bridge_dma_active = 0U;
        bridge_peer_failed = 1U;
    }
}

static void bridge_dma_complete(void *context, int status)
{
    (void)context;
    if (bridge_dma_active == 0U) {
        return;
    }
    bridge_dma_active = 0U;
    if (status != 0) {
        bridge_peer_failed = 1U;
    }
    if (bridge_complete != NULL) {
        bridge_complete();
    }
}

void st67w611_vendor_bridge_configure(const st67w611_port_ops_t *ops,
                                      void *context)
{
    if (bridge_dma_timer != NULL) {
        (void)xTimerDelete(bridge_dma_timer, 0U);
        bridge_dma_timer = NULL;
    }
    bridge_ops = ops;
    bridge_context = context;
    bridge_complete = NULL;
    bridge_peer_failed = 0U;
    bridge_dma_active = 0U;
    bridge_dma_timer = xTimerCreate(
        "st67_dma",
        pdMS_TO_TICKS(SPI_WAIT_MSG_XFER_TIMEOUT_MS),
        pdFALSE,
        NULL,
        bridge_dma_timeout);
}

int32_t spi_port_init(spi_transaction_complete_t transaction_complete_cb)
{
    bridge_complete = transaction_complete_cb;
    if (bridge_ops == NULL || bridge_ops->init == NULL) {
        return -1;
    }
    return bridge_ops->init(bridge_context, bridge_dma_complete, bridge_context);
}

int32_t spi_port_deinit(void)
{
    int result;
    if (bridge_ops == NULL || bridge_ops->deinit == NULL) {
        return -1;
    }
    if (bridge_dma_active != 0U && bridge_ops->cancel_transfer != NULL) {
        (void)bridge_ops->cancel_transfer(bridge_context);
    }
    bridge_dma_active = 0U;
    if (bridge_dma_timer != NULL) {
        (void)xTimerStop(bridge_dma_timer, 0U);
    }
    result = bridge_ops->deinit(bridge_context);
    bridge_complete = NULL;
    return result;
}

int32_t spi_port_transfer(void *tx_buf, void *rx_buf,
                          uint16_t len, uint32_t timeout)
{
    int result;
    if (bridge_ops == NULL || bridge_ops->transfer == NULL) {
        return -1;
    }
    result = bridge_ops->transfer(bridge_context, tx_buf, rx_buf, len, timeout);
    if (result != 0) {
        bridge_peer_failed = 1U;
    }
    return result;
}

int32_t spi_port_transfer_dma(void *tx_buf, void *rx_buf, uint16_t len)
{
    int result;
    if (bridge_ops == NULL || bridge_ops->transfer_dma == NULL ||
        bridge_dma_active != 0U) {
        return -1;
    }
    bridge_dma_active = 1U;
    result = bridge_ops->transfer_dma(bridge_context, tx_buf, rx_buf, len);
    if (result != 0) {
        bridge_dma_active = 0U;
        bridge_peer_failed = 1U;
    } else if (bridge_dma_timer != NULL) {
        (void)xTimerStart(bridge_dma_timer, 0U);
    }
    return result;
}

int32_t spi_port_is_ready(void)
{
    if (bridge_ops == NULL || bridge_ops->read_ready == NULL) {
        return 0;
    }
    return bridge_ops->read_ready(bridge_context) ? 1 : 0;
}

int32_t spi_port_set_cs(int32_t state)
{
    if (bridge_ops == NULL || bridge_ops->set_cs == NULL) {
        return -1;
    }
    return bridge_ops->set_cs(bridge_context, state != 0);
}

uint8_t spi_port_has_peer_failed(void)
{
    return bridge_peer_failed;
}

void *spi_port_memcpy(void *dest, const void *src, unsigned int len)
{
    return memcpy(dest, src, len);
}

void st67w611_port_notify_ready_rising(void)
{
    (void)spi_on_txn_data_ready();
}

void st67w611_port_notify_ready_falling(void)
{
    (void)spi_on_header_ack();
}
