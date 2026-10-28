#include "st67w611_lwip_internal.h"
#include "w6x_api.h"
#include "w6x_types.h"
#include "spi_iface.h"
#include "lwip/ip4_addr.h"
#include <string.h>

static struct netif *attached_netif;
static st67w611_lwip_callbacks_t attached_callbacks;
static uint8_t callbacks_valid;
static uint8_t transport_bound;

static st67w611_status_t bind_transport(void)
{
    W6X_Net_if_cb_t vendor_callbacks;
    if (attached_netif == NULL || callbacks_valid == 0U) {
        return ST67W611_ERR_INVALID_STATE;
    }
    memset(&vendor_callbacks, 0, sizeof(vendor_callbacks));
    vendor_callbacks.link_sta_up_fn = attached_callbacks.link_sta_up;
    vendor_callbacks.link_sta_down_fn = attached_callbacks.link_sta_down;
    vendor_callbacks.link_ap_up_fn = attached_callbacks.link_ap_up;
    vendor_callbacks.link_ap_down_fn = attached_callbacks.link_ap_down;
    vendor_callbacks.rxd_sta_notify_fn = attached_callbacks.rx_sta_notify;
    vendor_callbacks.rxd_ap_notify_fn = attached_callbacks.rx_ap_notify;
    if (W6X_Netif_Init(&vendor_callbacks) != W6X_STATUS_OK) {
        transport_bound = 0U;
        return ST67W611_ERR_VENDOR;
    }
    transport_bound = 1U;
    return ST67W611_OK;
}

st67w611_status_t st67w611_lwip_attach(struct netif *netif,
                                       const st67w611_lwip_callbacks_t *callbacks)
{
    if (netif == NULL || callbacks == NULL) return ST67W611_ERR_INVALID_ARG;
    attached_netif = netif;
    attached_callbacks = *callbacks;
    callbacks_valid = 1U;
    return bind_transport();
}

st67w611_status_t st67w611_lwip_detach(struct netif *netif)
{
    if (netif == NULL || netif != attached_netif) return ST67W611_ERR_INVALID_ARG;
    W6X_Netif_DeInit();
    attached_netif = NULL;
    memset(&attached_callbacks, 0, sizeof(attached_callbacks));
    callbacks_valid = 0U;
    transport_bound = 0U;
    return ST67W611_OK;
}

void st67w611_lwip_transport_invalidated(void)
{
    transport_bound = 0U;
}

st67w611_status_t st67w611_lwip_rebind(void)
{
    if (transport_bound != 0U) return ST67W611_OK;
    return bind_transport();
}

int st67w611_lwip_is_bound(void)
{
    return transport_bound != 0U;
}

static int attached(struct netif *netif)
{
    return netif != NULL && netif == attached_netif;
}

int32_t st67w611_lwip_input(void **buffer, uint8_t **payload)
{
    if (!attached_netif || transport_bound == 0U) return -1;
    return W6X_Netif_input(W6X_NET_IF_STA, buffer, payload);
}

int32_t st67w611_lwip_free(void *buffer)
{
    return W6X_Netif_free(buffer);
}

int32_t st67w611_lwip_flush_rx(void)
{
    void *buffer = NULL;
    uint8_t *payload = NULL;
    int32_t count = 0;
    int32_t len;
    if (transport_bound == 0U) return 0;
    while ((len = W6X_Netif_input(W6X_NET_IF_STA, &buffer, &payload)) > 0) {
        (void)len;
        if (buffer != NULL) (void)W6X_Netif_free(buffer);
        buffer = NULL;
        payload = NULL;
        count++;
    }
    return count;
}

int32_t st67w611_lwip_output(const uint8_t *data, uint32_t length)
{
    if (!attached_netif || transport_bound == 0U || data == NULL ||
        length == 0U || length > 1500U) return -1;
    return W6X_Netif_output(W6X_NET_IF_STA, (uint8_t *)data, length);
}

st67w611_status_t st67w611_lwip_get_ip(struct netif *netif,
                                       uint8_t ip[4], uint8_t gateway[4],
                                       uint8_t netmask[4])
{
    if (!attached(netif) || ip == NULL || gateway == NULL || netmask == NULL) {
        return ST67W611_ERR_INVALID_ARG;
    }
    memcpy(ip, &netif_ip4_addr(netif)->addr, 4U);
    memcpy(gateway, &netif_ip4_gw(netif)->addr, 4U);
    memcpy(netmask, &netif_ip4_netmask(netif)->addr, 4U);
    return ST67W611_OK;
}

st67w611_status_t st67w611_lwip_ping(struct netif *netif,
                                     const char *host, uint32_t timeout_ms)
{
    (void)netif;
    (void)host;
    (void)timeout_ms;
    return ST67W611_ERR_UNSUPPORTED;
}

st67w611_status_t st67w611_lwip_get_ip_capability(void *context,
                                                  uint8_t ip[4],
                                                  uint8_t gateway[4],
                                                  uint8_t netmask[4])
{
    return st67w611_lwip_get_ip((struct netif *)context, ip, gateway, netmask);
}

st67w611_status_t st67w611_lwip_ping_capability(void *context,
                                                const char *host,
                                                uint32_t timeout_ms)
{
    return st67w611_lwip_ping((struct netif *)context, host, timeout_ms);
}

int st67w611_lwip_get_transport_diagnostics(
    st67w611_lwip_transport_diagnostics_t *diagnostics)
{
    struct spi_stat stats;
    if (diagnostics == NULL) return -1;
    if (spi_get_stats(&stats) != 0) return -1;
    diagnostics->rx_packets = stats.rx_pkts;
    diagnostics->rx_drops = stats.rx_drop;
    diagnostics->io_errors = stats.io_err;
    diagnostics->header_errors = stats.hdr_err;
    diagnostics->rx_stalls = stats.rx_stall;
    return 0;
}

void st67w611_lwip_dump_transport_queues(void)
{
    spi_dump();
}
