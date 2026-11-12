#include "st67_net_probe.h"

#if ST67_NET_PROBE

#include <string.h>

#include "splc_log.h"
#include "w6x_api.h"

static const char *TAG = "NET_PROBE";

#define PROBE_RETRY_MS     10000U
#define PROBE_MAX_TRIES    3U

typedef enum {
    PROBE_WAIT_IP = 0,
    PROBE_LISTENING,     /* done: leave the listener open, reset the board to test again */
    PROBE_GAVE_UP,
} probe_state_t;

static probe_state_t s_state = PROBE_WAIT_IP;
static uint32_t      s_next_ms;
static uint8_t       s_tries;

static void try_listen(void)
{
    s_tries++;
    log_info(TAG, "try %u/%u: TCP listener on port %u", (unsigned)s_tries,
             (unsigned)PROBE_MAX_TRIES, (unsigned)ST67_NET_PROBE_PORT);

    const int32_t sock = W6X_Net_Socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    log_info(TAG, "W6X_Net_Socket -> %d", (int)sock);
    if (sock < 0) {
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len         = (uint8_t)sizeof(addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = PP_HTONS(ST67_NET_PROBE_PORT);
    addr.sin_addr.s_addr = 0U;

    const int32_t b = W6X_Net_Bind(sock, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr));
    log_info(TAG, "W6X_Net_Bind -> %d", (int)b);
    if (b != 0) {
        (void)W6X_Net_Close(sock);
        return;
    }

    /* backlog 2: W6X_Net_Listen sends AT+CIPSERVERMAXCONN=2 then AT+CIPSERVER=1,<port>,"TCP" */
    const int32_t l = W6X_Net_Listen(sock, 2);
    log_info(TAG, "W6X_Net_Listen -> %d", (int)l);
    if (l != 0) {
        (void)W6X_Net_Close(sock);
        return;
    }

    s_state = PROBE_LISTENING;
    log_info(TAG, "LISTENING on port %u: test from the PC with Test-NetConnection <ip> -Port %u",
             (unsigned)ST67_NET_PROBE_PORT, (unsigned)ST67_NET_PROBE_PORT);
}

void st67_net_probe_service(uint32_t now_ms, bool has_ip)
{
    if (s_state != PROBE_WAIT_IP || !has_ip) {
        return;
    }
    if (s_tries != 0U && (int32_t)(now_ms - s_next_ms) < 0) {
        return;
    }

    try_listen();

    if (s_state == PROBE_WAIT_IP) {
        if (s_tries >= PROBE_MAX_TRIES) {
            s_state = PROBE_GAVE_UP;
            log_warn(TAG, "no TCP listener after %u tries: the NCP firmware or the driver refused it",
                     (unsigned)s_tries);
        } else {
            s_next_ms = now_ms + PROBE_RETRY_MS;
        }
    }
}

#endif /* ST67_NET_PROBE */