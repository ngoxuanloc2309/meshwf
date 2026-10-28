#include "sx_mqtt.h"

#if SPLC_MQTT_ENABLE

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app_config.h"
#include "plc_mqtt.h"
#include "splc_log.h"
#include "logger.h"
#include "st67_mqtt.h"

static const char *TAG = "SX_MQTT";

/* ------------------------------------------------------------------------------------ */
/* Publish queue: engine task produces, ST67 task consumes (single producer/consumer).   */
/* ------------------------------------------------------------------------------------ */

typedef struct {
    char     topic[SX_MQTT_TOPIC_MAX + 1U];
    char     payload[SPLC_MQTT_PAYLOAD_MAX + 1U];
    uint16_t len;
    uint8_t  qos;
    bool     retain;
} msg_t;

static msg_t            s_q[SX_MQTT_QUEUE_DEPTH];
static volatile uint8_t s_q_head;      /* next slot to fill   (engine task only writes) */
static volatile uint8_t s_q_tail;      /* next slot to send   (ST67 task only writes)   */
static volatile uint8_t s_q_count;     /* guarded by a critical section                 */
static uint32_t         s_oversize;    /* messages the module path cannot carry (dropped) */

static int publish_cb(const char *topic, const char *payload, size_t len,
                      uint8_t qos, bool retain, void *ctx)
{
    (void)ctx;
    if (strlen(topic) > SX_MQTT_TOPIC_MAX || len > SPLC_MQTT_PAYLOAD_MAX) {
        /* Retrying cannot help: drop and count (return 0 so the library does not loop). */
        if (s_oversize++ == 0U) {
            log_warn(TAG, "message dropped: topic/payload too long for the queue");
        }
        return 0;
    }

    taskENTER_CRITICAL();
    const bool full = (s_q_count >= SX_MQTT_QUEUE_DEPTH);
    taskEXIT_CRITICAL();
    if (full) {
        return -1;                      /* busy: the library retries after SPLC_MQTT_RETRY_MS */
    }

    msg_t *m = &s_q[s_q_head];          /* the slot is not visible to the consumer yet */
    strcpy(m->topic, topic);
    memcpy(m->payload, payload, len);
    m->payload[len] = '\0';
    m->len    = (uint16_t)len;
    m->qos    = qos;
    m->retain = retain;

    taskENTER_CRITICAL();
    s_q_head = (uint8_t)((s_q_head + 1U) % SX_MQTT_QUEUE_DEPTH);
    s_q_count++;
    taskEXIT_CRITICAL();
    return 0;
}

static void queue_clear(void)           /* ST67 task: data from a dead session is dropped */
{
    taskENTER_CRITICAL();
    s_q_tail  = s_q_head;
    s_q_count = 0U;
    taskEXIT_CRITICAL();
}

/* ------------------------------------------------------------------------------------ */
/* Config mailbox: engine task writes (sx_mqtt_pump), ST67 task reads (sx_mqtt_service).  */
/* ------------------------------------------------------------------------------------ */

typedef struct {
    bool               valid;
    bool               from_app;        /* false = the defaults of app_config.h */
    uint32_t           generation;      /* of the library config this was built for */
    uint32_t           seq;             /* changes whenever the target connection changes */
    st67_mqtt_config_t cfg;
} box_t;

static box_t s_box;

/* Connection state published by the ST67 task, read by the engine task. */
static volatile bool     s_conn_valid;
static volatile uint32_t s_conn_generation;
static volatile uint16_t s_last_error;

static void fill_default(st67_mqtt_config_t *c)
{
    memset(c, 0, sizeof(*c));
    strncpy(c->host, BROKER_DEFAULT_HOST, ST67_MQTT_HOST_MAX);
    strncpy(c->client_id, BROKER_DEFAULT_DEVICEID, ST67_MQTT_CRED_MAX);
    c->port          = (uint16_t)BROKER_DEFAULT_PORT;
    c->keepalive_sec = 60U;
    c->clean_session = true;
}

/* App broker -> module config. false = cannot be applied (reason logged); the caller then
 * keeps the default broker, like the gateway does. */
static bool convert_app(const splc_mqtt_broker_t *b, st67_mqtt_config_t *c)
{
    if (b->use_tls) {
        log_warn(TAG, "App broker asks for TLS: not supported yet, using the default broker");
        return false;
    }
    if (strlen(b->host) > ST67_MQTT_HOST_MAX || strlen(b->client_id) > ST67_MQTT_CRED_MAX ||
        strlen(b->username) > ST67_MQTT_CRED_MAX || strlen(b->password) > ST67_MQTT_CRED_MAX) {
        log_warn(TAG, "App broker strings exceed the module limits (host %u, others %u): "
                      "using the default broker",
                 (unsigned)ST67_MQTT_HOST_MAX, (unsigned)ST67_MQTT_CRED_MAX);
        return false;
    }
    if (b->version != 0U) {
        log_warn(TAG, "App broker asks for MQTT 5.0: the module speaks 3.1.1 only, continuing with 3.1.1");
    }
    memset(c, 0, sizeof(*c));
    strcpy(c->host, b->host);
    strcpy(c->username, b->username);
    strcpy(c->password, b->password);
    /* An empty client id would let the module pick one; keep it stable instead. */
    strcpy(c->client_id, (b->client_id[0] != '\0') ? b->client_id : BROKER_DEFAULT_DEVICEID);
    c->port          = (b->port != 0U) ? b->port : (uint16_t)BROKER_DEFAULT_PORT;
    c->keepalive_sec = b->keepalive_sec;
    c->clean_session = b->clean_session;
    return true;
}

/* ------------------------------------------------------------------------------------ */
/* Engine side                                                                            */
/* ------------------------------------------------------------------------------------ */

#define PUMP_PERIOD_MS  100U

static uint32_t s_pump_next_ms;
static bool     s_pump_first = true;
static bool     s_have_last;
static bool     s_last_app;
static uint32_t s_last_gen;
static uint32_t s_seq;

void sx_mqtt_init(void)
{
    if (!splc_mqtt_register(publish_cb, NULL)) {
        log_error(TAG, "splc_mqtt_register failed");
    }
    splc_mqtt_set_default_device_name(BROKER_DEFAULT_DEVICEID);
}

void sx_mqtt_pump(uint32_t now_ms)
{
    if (!s_pump_first && (int32_t)(now_ms - s_pump_next_ms) < 0) {
        return;
    }
    s_pump_first   = false;
    s_pump_next_ms = now_ms + PUMP_PERIOD_MS;

    /* 1. broker config -> mailbox, only when something changed */
    splc_mqtt_broker_t b;
    bool     app = false;
    uint32_t gen = 0U;
    splc_mqtt_get_broker(&b, &app, &gen);

    if (!s_have_last || app != s_last_app || gen != s_last_gen) {
        st67_mqtt_config_t cfg;
        const bool use_app = app && convert_app(&b, &cfg);
        if (!use_app) {
            fill_default(&cfg);
        }
        s_have_last = true;
        s_last_app  = app;
        s_last_gen  = gen;
        s_seq++;

        taskENTER_CRITICAL();
        s_box.cfg        = cfg;
        s_box.from_app   = use_app;
        s_box.generation = gen;
        s_box.seq        = s_seq;
        s_box.valid      = true;
        taskEXIT_CRITICAL();

        log_info(TAG, "target broker %s:%u (%s), generation %u", cfg.host, (unsigned)cfg.port,
                 use_app ? "from App" : "default", (unsigned)gen);
    }

    /* 2. connection state -> library. generation = the config the connection was built
     * with; the library publishes only while it equals its own current generation. */
    splc_mqtt_status_t st;
    memset(&st, 0, sizeof(st));
    const bool up = s_conn_valid && st67_mqtt_is_connected();
    st.flags      = up ? SPLC_MQTT_STATUS_CONNECTED : 0U;
    st.generation = s_conn_generation;
    st.last_error = up ? 0U : s_last_error;
    splc_mqtt_set_status(&st);
}

/* ------------------------------------------------------------------------------------ */
/* ST67 task side                                                                         */
/* ------------------------------------------------------------------------------------ */

typedef enum { S_IDLE, S_CONNECTED, S_BACKOFF } state_t;

static state_t  s_state = S_IDLE;
static uint32_t s_conn_seq;
static uint32_t s_backoff_ms = SX_MQTT_BACKOFF_MIN_MS;
static uint32_t s_next_try_ms;
static bool     s_hello_pending;
static volatile bool s_module_reset;

void sx_mqtt_notify_module_reset(void)
{
    s_module_reset = true;
    st67_mqtt_on_module_reset();
}

static void go_down(uint32_t now_ms, uint16_t err, bool backoff)
{
    s_conn_valid = false;
    s_last_error = err;
    queue_clear();
    if (backoff) {
        s_state       = S_BACKOFF;
        s_next_try_ms = now_ms + s_backoff_ms;
        log_warn(TAG, "MQTT down (error %u), retry in %u ms", (unsigned)err, (unsigned)s_backoff_ms);
        s_backoff_ms = (s_backoff_ms * 2U > SX_MQTT_BACKOFF_MAX_MS) ? SX_MQTT_BACKOFF_MAX_MS
                                                                    : (s_backoff_ms * 2U);
    } else {
        s_state = S_IDLE;
    }
}

static bool box_snapshot(box_t *out)
{
    taskENTER_CRITICAL();
    *out = s_box;
    taskEXIT_CRITICAL();
    return out->valid;
}

static void try_connect(uint32_t now_ms)
{
    static box_t box;                    /* ~0.2 KB, keep it off the 4 KB ST67 stack */
    if (!box_snapshot(&box)) {
        return;                          /* the engine task has not delivered a target yet */
    }
    log_info(TAG, "connecting to %s:%u (%s)", box.cfg.host, (unsigned)box.cfg.port,
             box.from_app ? "App" : "default");

    const st67_mqtt_status_t r = st67_mqtt_connect(&box.cfg, SX_MQTT_CONNECT_TIMEOUT_MS);
    if (r != ST67_MQTT_OK) {
        go_down(now_ms, (r == ST67_MQTT_ERR_TIMEOUT) ? 4U /* TIMEOUT */
                                                     : 2U /* TCP_RESET: vendor gives no detail */, true);
        return;
    }
    s_conn_seq        = box.seq;
    s_conn_generation = box.generation;
    s_conn_valid      = true;
    s_last_error      = 0U;
    s_state           = S_CONNECTED;
    s_backoff_ms      = SX_MQTT_BACKOFF_MIN_MS;
    s_hello_pending   = (SX_MQTT_HELLO_ON_DEFAULT != 0) && !box.from_app;
    queue_clear();
    log_info(TAG, "MQTT connected (generation %u)", (unsigned)box.generation);
}

static void send_one(void)
{
    if (s_hello_pending) {
        s_hello_pending = false;
        (void)st67_mqtt_publish(DEFAULT_TOPIC, DEFAULT_MESSAGE, strlen(DEFAULT_MESSAGE), 0U, false);
        return;
    }

    taskENTER_CRITICAL();
    const uint8_t n = s_q_count;
    taskEXIT_CRITICAL();
    if (n == 0U) {
        return;
    }

    const msg_t *m = &s_q[s_q_tail];     /* not overwritten: the producer sees the slot as used */
    const st67_mqtt_status_t r = st67_mqtt_publish(m->topic, m->payload, m->len, m->qos, m->retain);
    if (r != ST67_MQTT_OK) {
        log_warn(TAG, "publish failed (%d): message dropped", (int)r);
    }

    taskENTER_CRITICAL();
    s_q_tail = (uint8_t)((s_q_tail + 1U) % SX_MQTT_QUEUE_DEPTH);
    s_q_count--;
    taskEXIT_CRITICAL();
}

void sx_mqtt_service(uint32_t now_ms, bool link_up)
{
    if (s_module_reset) {                /* the module restarted: the session is gone */
        s_module_reset = false;
        go_down(now_ms, 0U, false);
        s_backoff_ms = SX_MQTT_BACKOFF_MIN_MS;
    }

    if (!link_up) {
        if (s_state != S_IDLE) {
            go_down(now_ms, 0U, false);  /* no IP: wait, no timer running */
        }
        return;
    }

    if (st67_mqtt_needs_init()) {
        if (st67_mqtt_init() != ST67_MQTT_OK) {
            go_down(now_ms, 2U, true);
            return;
        }
    }

    switch (s_state) {
    case S_IDLE:
        try_connect(now_ms);
        break;

    case S_BACKOFF:
        if ((int32_t)(now_ms - s_next_try_ms) >= 0) {
            try_connect(now_ms);
        }
        break;

    case S_CONNECTED: {
        if (!st67_mqtt_is_connected()) {
            go_down(now_ms, 2U, true);
            break;
        }
        uint32_t seq;
        taskENTER_CRITICAL();
        seq = s_box.seq;
        taskEXIT_CRITICAL();
        if (seq != s_conn_seq) {         /* the App changed the broker: reconnect at once */
            log_info(TAG, "broker config changed, reconnecting");
            (void)st67_mqtt_disconnect();
            go_down(now_ms, 0U, false);
            s_backoff_ms = SX_MQTT_BACKOFF_MIN_MS;
            break;
        }
        send_one();
        break;
    }
    }
}

#endif /* SPLC_MQTT_ENABLE */