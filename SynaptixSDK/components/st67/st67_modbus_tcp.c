#include "st67_modbus_tcp.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "splc_log.h"
#include "w6x_api.h"

static const char *TAG = "MBTCP";

/* MBAP header: transaction(2) protocol(2) length(2), then `length` bytes (unit id + PDU). */
#define MBAP_HDR        6U
#define MBAP_MIN_LEN    2U
#define MBAP_MAX_LEN    254U
#define MBAP_MAX_ADU    (MBAP_HDR + MBAP_MAX_LEN)      /* 260 */
#define RX_BUF          (2U * MBAP_MAX_ADU)            /* one frame + the start of the next */

#if (ST67_MBTCP_MAX_CLIENTS < 1U) || (ST67_MBTCP_MAX_CLIENTS > 4U)
#error "ST67_MBTCP_MAX_CLIENTS must be 1..4"
#endif

typedef struct {
    int32_t  sock;                 /* -1 = free */
    uint16_t len;                  /* bytes in buf */
    uint32_t last_rx_ms;
    uint32_t partial_since_ms;     /* when the bytes now in buf started waiting */
    uint8_t  buf[RX_BUF];
} client_t;

/* Hand-over between the I/O task and the engine task (one request at a time):
 * IDLE -> REQ_READY (I/O)  -> REQ_TAKEN (engine read() got every byte)
 *      -> RSP_READY (engine write()) -> IDLE (I/O, after it sent the answer). */
enum { X_IDLE = 0, X_REQ_READY, X_REQ_TAKEN, X_RSP_READY };

static client_t              s_cl[ST67_MBTCP_MAX_CLIENTS];
static bool                (*s_link_up)(void);
static volatile int32_t      s_listener = -1;
static QueueHandle_t         s_q_new;           /* socket ids accepted, waiting for the I/O task */
static TaskHandle_t          s_io_task;

static volatile uint8_t      s_x_state = X_IDLE;
static uint8_t               s_req[MBAP_MAX_ADU];
static volatile uint16_t     s_req_len;
static volatile uint16_t     s_req_pos;
static uint8_t               s_rsp[MBAP_MAX_ADU];
static volatile uint16_t     s_rsp_len;

static struct {
    uint32_t accepted, rejected, dropped, req, rsp, no_rsp;
} s_stat;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void x_set(uint8_t st)
{
    taskENTER_CRITICAL();
    s_x_state = st;
    taskEXIT_CRITICAL();
}

/* ---------------------------------------------------------------------------------------- */
/* Engine side: called from the SPLC task by plc_modbus_cfg, never blocks                     */
/* ---------------------------------------------------------------------------------------- */

static int32_t tr_read(uint8_t *buf, uint16_t count, int32_t timeout_ms, void *ctx)
{
    (void)timeout_ms;
    (void)ctx;
    if (s_x_state != X_REQ_READY) {
        return 0;                              /* no request (or no client): not an error */
    }
    const uint16_t left = (uint16_t)(s_req_len - s_req_pos);
    const uint16_t n = (count < left) ? count : left;
    memcpy(buf, &s_req[s_req_pos], n);
    s_req_pos = (uint16_t)(s_req_pos + n);
    if (s_req_pos >= s_req_len) {
        x_set(X_REQ_TAKEN);                    /* the whole frame is with the library now */
    }
    return (int32_t)n;
}

static int32_t tr_write(const uint8_t *buf, uint16_t count, int32_t timeout_ms, void *ctx)
{
    (void)timeout_ms;
    (void)ctx;
    if (s_x_state != X_REQ_TAKEN || count == 0U || count > MBAP_MAX_ADU) {
        return -1;                             /* nobody waits for it (answer came too late) */
    }
    memcpy(s_rsp, buf, count);
    s_rsp_len = count;
    x_set(X_RSP_READY);
    if (s_io_task != NULL) {
        (void)xTaskNotifyGive(s_io_task);
    }
    return (int32_t)count;
}

/* ---------------------------------------------------------------------------------------- */
/* I/O task                                                                                   */
/* ---------------------------------------------------------------------------------------- */

/* 1 = complete frame (*adu = its length), 0 = need more bytes, -1 = not Modbus TCP. */
static int frame_check(const uint8_t *b, uint16_t len, uint16_t *adu)
{
    if (len >= 4U && (b[2] != 0U || b[3] != 0U)) {
        return -1;                             /* protocol id must be 0 */
    }
    if (len < MBAP_HDR) {
        return 0;
    }
    const uint16_t l = (uint16_t)(((uint16_t)b[4] << 8) | b[5]);
    if (l < MBAP_MIN_LEN || l > MBAP_MAX_LEN) {
        return -1;
    }
    if (len < (uint16_t)(MBAP_HDR + l)) {
        return 0;
    }
    *adu = (uint16_t)(MBAP_HDR + l);
    return 1;
}

static void client_drop(client_t *c, const char *why)
{
    log_info(TAG, "client socket %d closed: %s", (int)c->sock, why);
    (void)W6X_Net_Close(c->sock);
    c->sock = -1;
    c->len  = 0U;
    s_stat.dropped++;
}

/* Give one frame to the engine, wait for the answer, send it to the same client.
 * false = the client must be closed. */
static bool serve_frame(client_t *c, uint16_t adu)
{
    memcpy(s_req, c->buf, adu);
    s_req_len = adu;
    s_req_pos = 0U;
    (void)ulTaskNotifyTake(pdTRUE, 0);         /* forget a late notification of an old request */
    x_set(X_REQ_READY);
    s_stat.req++;

    const uint32_t t0 = now_ms();
    const uint32_t got = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ST67_MBTCP_RESP_MS));
    if (got == 0U || s_x_state != X_RSP_READY) {
        x_set(X_IDLE);
        s_stat.no_rsp++;
        log_warn(TAG, "no answer from the engine in %u ms (fc=0x%02X): request dropped",
                 (unsigned)ST67_MBTCP_RESP_MS, (unsigned)((adu > 7U) ? c->buf[7] : 0U));
        return true;                           /* keep the client, the master will time out */
    }

    const uint16_t rlen = s_rsp_len;
    const uint32_t sd_t0 = now_ms();
    const ssize_t sent = W6X_Net_Send(c->sock, s_rsp, rlen, 0);
    if ((now_ms() - sd_t0) > 400U) {
        log_warn(TAG, "W6X_Net_Send slow: %u ms", (unsigned)(now_ms() - sd_t0));
    }
    x_set(X_IDLE);
    if (sent != (ssize_t)rlen) {
        log_warn(TAG, "send failed (%d of %u bytes)", (int)sent, (unsigned)rlen);
        return false;
    }
    s_stat.rsp++;
    log_debug(TAG, "req %uB -> rsp %uB in %u ms (tid=0x%02X%02X fc=0x%02X)", (unsigned)adu,
              (unsigned)rlen, (unsigned)(now_ms() - t0), (unsigned)c->buf[0], (unsigned)c->buf[1],
              (unsigned)((adu > 7U) ? c->buf[7] : 0U));
    return true;
}

static uint8_t s_recv_trace;                   /* DIAG: calls left to trace after a client is adopted */

static void service_client(client_t *c)
{
    const uint16_t space = (uint16_t)(RX_BUF - c->len);
    if (space == 0U) {
        client_drop(c, "receive buffer full");
        return;
    }

    /* DIAG (temporary): trace the first calls after a client is adopted, and any slow call. */
    const bool trace = (s_recv_trace > 0U);
    if (trace) {
        s_recv_trace--;
        log_debug(TAG, "recv begin sock=%d t=%u", (int)c->sock, (unsigned)now_ms());
    }
    const uint32_t rv_t0 = now_ms();
    /* Waits up to ~100 ms for data (vendor: even with SO_RCVTIMEO = 0). */
    const ssize_t n = W6X_Net_Recv(c->sock, &c->buf[c->len], space, 0);
    const uint32_t rv_dt = now_ms() - rv_t0;
    if (rv_dt > 400U) {
        log_warn(TAG, "W6X_Net_Recv slow: %u ms (n=%d)", (unsigned)rv_dt, (int)n);
    } else if (trace) {
        log_debug(TAG, "recv end %u ms n=%d", (unsigned)rv_dt, (int)n);
    }
    if (n > 0) {
        if (c->len == 0U) {
            c->partial_since_ms = now_ms();
        }
        c->len = (uint16_t)(c->len + (uint16_t)n);
        c->last_rx_ms = now_ms();
    } else if (n < 0 && n != -2) {             /* -2 = BUSY: try again later */
        client_drop(c, "disconnected or receive error");
        return;
    }

    for (;;) {
        uint16_t adu = 0U;
        const int st = frame_check(c->buf, c->len, &adu);
        if (st < 0) {
            client_drop(c, "not a Modbus TCP frame");
            return;
        }
        if (st == 0) {
            break;
        }
        if (!serve_frame(c, adu)) {
            client_drop(c, "send error");
            return;
        }
        memmove(c->buf, &c->buf[adu], (size_t)(c->len - adu));
        c->len = (uint16_t)(c->len - adu);
        c->partial_since_ms = now_ms();
    }

    const uint32_t now = now_ms();
    if (c->len > 0U && (now - c->partial_since_ms) > ST67_MBTCP_PARTIAL_MS) {
        client_drop(c, "incomplete frame timeout");
    } else if (ST67_MBTCP_IDLE_MS != 0U && (now - c->last_rx_ms) > ST67_MBTCP_IDLE_MS) {
        client_drop(c, "idle timeout");
    }
}

static void adopt(int32_t sock)
{
    for (uint32_t i = 0U; i < ST67_MBTCP_MAX_CLIENTS; i++) {
        if (s_cl[i].sock < 0) {
            s_cl[i].sock = sock;
            s_cl[i].len = 0U;
            s_cl[i].last_rx_ms = now_ms();
            s_cl[i].partial_since_ms = s_cl[i].last_rx_ms;
            s_recv_trace = 6U;                 /* DIAG */
            log_debug(TAG, "client adopted: slot %u socket %d", (unsigned)i, (int)sock);
            return;
        }
    }
    s_stat.rejected++;
    log_warn(TAG, "too many clients (max %u): socket %d rejected", (unsigned)ST67_MBTCP_MAX_CLIENTS,
             (int)sock);
    (void)W6X_Net_Close(sock);
}

/* Wi-Fi lost: close every client, then the listener (this also frees the accept task, which
 * is blocked inside W6X_Net_Accept). Best effort: the AT commands may fail while the link is
 * down. NOT verified on hardware. */
static void teardown(void)
{
    int32_t ns;
    for (uint32_t i = 0U; i < ST67_MBTCP_MAX_CLIENTS; i++) {
        if (s_cl[i].sock >= 0) {
            (void)W6X_Net_Close(s_cl[i].sock);
            s_cl[i].sock = -1;
            s_cl[i].len = 0U;
        }
    }
    while (xQueueReceive(s_q_new, &ns, 0) == pdTRUE) {
        (void)W6X_Net_Close(ns);
    }
    x_set(X_IDLE);

    const int32_t ls = s_listener;
    if (ls >= 0) {
        const int32_t r = W6X_Net_Shutdown(ls, 1);
        log_info(TAG, "listener %d shutdown -> %d", (int)ls, (int)r);
        s_listener = -1;
    }
}

static void io_task(void *arg)
{
    (void)arg;
    bool was_up = false;
    uint32_t next_stat = now_ms() + 10000U;

    for (;;) {
        const bool up = (s_link_up != NULL) && s_link_up();
        if (!up) {
            if (was_up) {
                log_warn(TAG, "Wi-Fi link down: closing clients and listener");
                teardown();
            }
            was_up = false;
            vTaskDelay(pdMS_TO_TICKS(250U));
            continue;
        }
        was_up = true;

        bool any = false;
        for (uint32_t i = 0U; i < ST67_MBTCP_MAX_CLIENTS; i++) {
            if (s_cl[i].sock >= 0) {
                any = true;
                service_client(&s_cl[i]);
            }
        }

        int32_t ns;
        if (!any) {
            if (xQueueReceive(s_q_new, &ns, pdMS_TO_TICKS(100U)) == pdTRUE) {
                adopt(ns);
            }
        } else {
            while (xQueueReceive(s_q_new, &ns, 0) == pdTRUE) {
                adopt(ns);
            }
        }

        if ((int32_t)(now_ms() - next_stat) >= 0) {
            uint32_t n = 0U;
            for (uint32_t i = 0U; i < ST67_MBTCP_MAX_CLIENTS; i++) {
                n += (s_cl[i].sock >= 0) ? 1U : 0U;
            }
            log_debug(TAG, "stats clients=%u accepted=%u rejected=%u dropped=%u req=%u rsp=%u no_rsp=%u "
                           "io_stack_min_free=%u words",
                      (unsigned)n, (unsigned)s_stat.accepted, (unsigned)s_stat.rejected,
                      (unsigned)s_stat.dropped, (unsigned)s_stat.req, (unsigned)s_stat.rsp,
                      (unsigned)s_stat.no_rsp, (unsigned)uxTaskGetStackHighWaterMark(NULL));
            next_stat = now_ms() + 10000U;
        }
    }
}

/* ---------------------------------------------------------------------------------------- */
/* Accept task                                                                                */
/* ---------------------------------------------------------------------------------------- */

static int32_t open_listener(void)
{
    const int32_t s = W6X_Net_Socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        log_warn(TAG, "W6X_Net_Socket failed (%d)", (int)s);
        return -1;
    }

    /* Accepted sockets inherit this: 0 = one ~100 ms wait per W6X_Net_Recv. */
    const int32_t zero = 0;
    (void)W6X_Net_Setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &zero, (socklen_t)sizeof(zero));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len         = (uint8_t)sizeof(addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = PP_HTONS(ST67_MBTCP_PORT);
    addr.sin_addr.s_addr = 0U;

    if (W6X_Net_Bind(s, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) {
        log_warn(TAG, "W6X_Net_Bind failed");
        (void)W6X_Net_Close(s);
        return -1;
    }
    /* sends AT+CIPSERVERMAXCONN=<n> and AT+CIPSERVER=1,<port>,"TCP" */
    if (W6X_Net_Listen(s, (int32_t)ST67_MBTCP_MAX_CLIENTS) != 0) {
        log_warn(TAG, "W6X_Net_Listen failed (firmware or no free connection)");
        (void)W6X_Net_Close(s);
        return -1;
    }
    return s;
}

static void accept_task(void *arg)
{
    (void)arg;
    bool stack_logged = false;

    for (;;) {
        if (s_link_up == NULL || !s_link_up()) {
            vTaskDelay(pdMS_TO_TICKS(500U));
            continue;
        }

        const int32_t ls = open_listener();
        if (ls < 0) {
            vTaskDelay(pdMS_TO_TICKS(5000U));
            continue;
        }
        s_listener = ls;
        log_info(TAG, "Modbus TCP slave listening on port %u (max %u clients)",
                 (unsigned)ST67_MBTCP_PORT, (unsigned)ST67_MBTCP_MAX_CLIENTS);

        while (s_listener == ls) {
            struct sockaddr_in peer;
            memset(&peer, 0, sizeof(peer));
            socklen_t plen = (socklen_t)sizeof(peer);

            /* Blocks (vendor polls every 100 ms) until a client connects or the listener is
             * shut down by teardown(). */
            const int32_t c = W6X_Net_Accept(ls, (struct sockaddr *)&peer, &plen);
            if (c < 0) {
                vTaskDelay(pdMS_TO_TICKS(500U));     /* no free socket / listener closed */
                continue;
            }

            const uint8_t *ip = (const uint8_t *)&peer.sin_addr.s_addr;
            s_stat.accepted++;
            log_info(TAG, "client connected: socket %d from %u.%u.%u.%u:%u", (int)c, (unsigned)ip[0],
                     (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3],
                     (unsigned)PP_HTONS(peer.sin_port));
            if (xQueueSend(s_q_new, &c, 0) != pdTRUE) {
                log_warn(TAG, "hand-over queue full: socket %d closed", (int)c);
                (void)W6X_Net_Close(c);
            }
            if (!stack_logged) {
                stack_logged = true;
                log_debug(TAG, "accept task stack min free=%u words",
                          (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
    }
}

/* ---------------------------------------------------------------------------------------- */

bool st67_modbus_tcp_create(modbus_transport_t *out, bool (*link_up)(void))
{
    static bool created;
    if (out == NULL || link_up == NULL || created) {
        return false;
    }

    s_link_up = link_up;
    for (uint32_t i = 0U; i < ST67_MBTCP_MAX_CLIENTS; i++) {
        s_cl[i].sock = -1;
    }
    s_listener = -1;
    s_x_state = X_IDLE;

    s_q_new = xQueueCreate(ST67_MBTCP_MAX_CLIENTS, sizeof(int32_t));
    if (s_q_new == NULL) {
        log_error(TAG, "queue create failed");
        return false;
    }
    if (xTaskCreate(io_task, "MBTCP_IO", (configSTACK_DEPTH_TYPE)ST67_MBTCP_IO_STACK_WORDS, NULL,
                    ST67_MBTCP_IO_PRIO, &s_io_task) != pdPASS) {
        log_error(TAG, "I/O task create failed (heap?)");
        return false;
    }
    if (xTaskCreate(accept_task, "MBTCP_ACC", (configSTACK_DEPTH_TYPE)ST67_MBTCP_ACC_STACK_WORDS,
                    NULL, ST67_MBTCP_ACC_PRIO, NULL) != pdPASS) {
        log_error(TAG, "accept task create failed (heap?)");
        return false;           /* the I/O task stays idle: no client can ever arrive */
    }

    memset(out, 0, sizeof(*out));
    out->ctx     = NULL;
    out->read    = tr_read;
    out->write   = tr_write;
    out->process = NULL;
    out->kind    = MODBUS_TRANSPORT_KIND_TCP;
    out->unit_id = 1U;          /* ignored for TCP */

    created = true;
    log_info(TAG, "created: port %u, %u clients max, waiting for a Wi-Fi IP",
             (unsigned)ST67_MBTCP_PORT, (unsigned)ST67_MBTCP_MAX_CLIENTS);
    return true;
}