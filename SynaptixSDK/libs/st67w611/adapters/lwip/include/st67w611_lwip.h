#ifndef ST67W611_LWIP_H
#define ST67W611_LWIP_H

#include "st67w611.h"
#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

typedef int32_t (*st67w611_lwip_link_cb_t)(void);
typedef void (*st67w611_lwip_notify_cb_t)(void *arg);

typedef struct {
    st67w611_lwip_link_cb_t link_sta_up;
    st67w611_lwip_link_cb_t link_sta_down;
    st67w611_lwip_link_cb_t link_ap_up;
    st67w611_lwip_link_cb_t link_ap_down;
    st67w611_lwip_notify_cb_t rx_sta_notify;
    st67w611_lwip_notify_cb_t rx_ap_notify;
} st67w611_lwip_callbacks_t;

st67w611_status_t st67w611_lwip_attach(struct netif *netif,
                                       const st67w611_lwip_callbacks_t *callbacks);
st67w611_status_t st67w611_lwip_detach(struct netif *netif);
void st67w611_lwip_transport_invalidated(void);
st67w611_status_t st67w611_lwip_rebind(void);
int st67w611_lwip_is_bound(void);

st67w611_status_t st67w611_lwip_get_ip(struct netif *netif,
                                       uint8_t ip[4], uint8_t gateway[4],
                                       uint8_t netmask[4]);
st67w611_status_t st67w611_lwip_ping(struct netif *netif,
                                     const char *host, uint32_t timeout_ms);

#endif
