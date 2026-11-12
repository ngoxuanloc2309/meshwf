/**
 * @file splc_wifi_port.c
 * @brief See splc_wifi_port.h.
 */
#include "splc_wifi_port.h"

#if SPLC_WIFI_ENABLE

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "st67w611.h"
#include "st67w611_wifi_switch.h"
#include "plc_wifi.h"
#include "splc_log.h"
#include "app_config.h"

static const char *TAG = "SPLC_WIFI";

/* Default network (app/app_config.h). Used ONLY while neither the App nor the Flash store has
 * given any Wi-Fi configuration; it is never written to Flash (the library saves only a
 * CONNECT that the App sent, and the default connect does not go through the library). */
#ifndef WIFI_SSID_DEFAULT
#define WIFI_SSID_DEFAULT       ""
#endif
#ifndef WIFI_PASSWORD_DEFAULT
#define WIFI_PASSWORD_DEFAULT   ""
#endif
/* While idle (no connect / scan running) the worker re-reads the link state this often, so a
 * dropped link is noticed and logged even when the module sends no event. */
#ifndef SPLC_HOST_WIFI_LINK_CHECK_MS
#define SPLC_HOST_WIFI_LINK_CHECK_MS    10000U
#endif
/* Pause between two default-connect attempts while the link is down. */
#ifndef SPLC_HOST_WIFI_DEFAULT_RETRY_MS
#define SPLC_HOST_WIFI_DEFAULT_RETRY_MS 30000U
#endif

/* Work for the ST67 task. Set by the engine-side callbacks / events, cleared by the worker. */
#define CMD_CONNECT     0x01U
#define CMD_DISCONNECT  0x02U
#define CMD_SCAN        0x04U
#define CMD_REFRESH     0x08U     /* re-read the link state and IP from the facade */

/* Live state seen by status(). Written by events and the worker, read by the engine task;
 * every access is inside a critical section (a few words, never a facade call). */
typedef struct {
    uint16_t conn;                /* PLC_WIFI_CONN_* of the LAST known link state */
    uint16_t err;                 /* PLC_WIFI_ERR_* of the last failure, 0 = none */
    uint32_t ip;                  /* host byte order, 0 = none */
    uint8_t  mac[6];
    bool     mac_valid;
} live_t;

static live_t                s_live;
static volatile uint32_t     s_cmd;
static volatile bool         s_ready;
static TaskHandle_t          s_task;

/* CONNECT request in flight. s_connecting makes status() report CONNECTING from the moment
 * the App's command is accepted until the worker has the final answer, so a GOT_IP that
 * belongs to the OLD link can never be taken for the success of the NEW one (the library
 * saves the credentials on the first GOT_IP it sees). s_gen tells the worker whether a
 * newer CONNECT / DISCONNECT arrived while it was busy. */
static splc_wifi_config_t    s_cfg;
static volatile bool         s_connecting;
static volatile uint32_t     s_gen;

/* Scan table: the 8 strongest networks, strongest first. */
static splc_wifi_ap_t        s_ap[PLC_WIFI_MAX_APS];
static uint8_t               s_ap_count;
static uint16_t              s_scan_state = PLC_WIFI_SCAN_IDLE;
static uint32_t              s_scan_deadline_ms;

/* Default-network policy. s_load_done / s_stored come from the Flash load (engine task, once
 * at boot); s_user_touched is set by ANY connect / disconnect / erase request from the App or
 * the library (a stored auto-reconnect included): from then on the default is never used until
 * the next reset. Plain flags written by the engine task, read by the worker. */
static volatile bool         s_load_done;
static volatile bool         s_stored;
static volatile bool         s_user_touched;
static volatile bool         s_rejoin_pending;    /* the module restarted: rejoin the App's network */

/* Worker-private (no lock needed). */
static uint32_t              s_link_next_ms;      /* next idle link check, 0 = at once */
static uint16_t              s_logged_conn = 0xFFFFU;   /* last link state printed */
static uint16_t              s_logged_err;
static uint32_t              s_logged_ip;         /* last IP printed (0 = none), worker only */
static uint32_t              s_default_next_ms;   /* 0 = first attempt allowed at once */
static bool                  s_default_logged;    /* the "why not" line is printed once */
static uint32_t              s_ip_poll_until_ms;   /* 0 = not waiting for an IP */
static uint32_t              s_ip_poll_next_ms;
static uint32_t              s_mac_next_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(((uint64_t)xTaskGetTickCount() * 1000U) / configTICK_RATE_HZ);
}

static bool time_reached(uint32_t now, uint32_t target)
{
    return (int32_t)(now - target) >= 0;       /* uint32 wrap-safe */
}

static void post(uint32_t bits)
{
    taskENTER_CRITICAL();
    s_cmd |= bits;
    taskEXIT_CRITICAL();
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

static uint32_t take_cmds(void)
{
    taskENTER_CRITICAL();
    const uint32_t c = s_cmd;
    s_cmd = 0U;
    taskEXIT_CRITICAL();
    return c;
}

static void live_set(uint16_t conn, uint16_t err, uint32_t ip)
{
    taskENTER_CRITICAL();
    s_live.conn = conn;
    s_live.err  = err;
    s_live.ip   = ip;
    taskEXIT_CRITICAL();
}

bool splc_wifi_port_has_ip(void)
{
    taskENTER_CRITICAL();
    const bool up = (s_live.conn == PLC_WIFI_CONN_GOT_IP);
    taskEXIT_CRITICAL();
    return up;
}

int splc_wifi_port_link_state(void)
{
    taskENTER_CRITICAL();
    const uint16_t conn = s_live.conn;
    const uint16_t err  = s_live.err;
    const bool connecting = s_connecting;
    const bool ready      = s_ready;
    const bool touched    = s_user_touched || s_stored;
    taskEXIT_CRITICAL();

    switch (conn) {
    case PLC_WIFI_CONN_GOT_IP:
        return 2;                                  /* SPLC_LINK_STATE_CONNECTED  */
    case PLC_WIFI_CONN_CONNECTING:
    case PLC_WIFI_CONN_CONNECTED:
        return 1;                                  /* SPLC_LINK_STATE_CONNECTING */
    default:
        break;
    }
    if (err != 0U) {
        return 3;                                  /* SPLC_LINK_STATE_FAILED     */
    }
    if (!ready || connecting || !touched) {
        return 1;                                  /* bring-up, a connect running, or the default join to come */
    }
    return 3;
}

/* ---- facade events (vendor callback context) ---------------------------------------- */

void splc_wifi_port_on_event(st67w611_event_t event, void *event_data)
{
    switch (event) {
    case ST67W611_EVENT_CONNECTING:
        live_set(PLC_WIFI_CONN_CONNECTING, 0U, 0U);
        break;
    case ST67W611_EVENT_CONNECTED:
        live_set(PLC_WIFI_CONN_CONNECTED, 0U, 0U);
        post(CMD_REFRESH);                     /* the IP may already be there */
        break;
    case ST67W611_EVENT_GOT_IP:
        live_set(PLC_WIFI_CONN_CONNECTED, 0U, 0U);
        post(CMD_REFRESH);                     /* the worker reads the address and sets GOT_IP */
        break;
    case ST67W611_EVENT_DISCONNECTED:
        live_set(PLC_WIFI_CONN_DISCONNECTED, 0U, 0U);
        break;
    case ST67W611_EVENT_ERROR:
        /* Diagnostic: event_data is the vendor function that failed (__func__, a string literal) when
         * the error comes from the vendor error callback, NULL when the facade itself failed an
         * operation. MQTT functions never get here (st67w611.c ignores them). */
        log_warn(TAG, "driver error event from %s: Wi-Fi link marked down",
                 (event_data != NULL) ? (const char *)event_data : "the facade (no vendor function)");
        live_set(PLC_WIFI_CONN_DISCONNECTED, PLC_WIFI_ERR_DRIVER, 0U);
        break;
    case ST67W611_EVENT_READY:
        /* After the bring-up, READY means the module restarted (soft reset): the AP link is gone.
         * The default network comes back through default_service(); a network from the App or the
         * Flash is rejoined by the worker (rejoin_after_module_restart). Flag only. */
        if (s_ready) {
            s_rejoin_pending = true;
            post(CMD_REFRESH);
        }
        break;
    default:
        break;
    }
}

/* ---- scan --------------------------------------------------------------------------- */

static void scan_insert(const st67w611_scan_result_t *r)
{
    splc_wifi_ap_t ap;
    memset(&ap, 0, sizeof(ap));
    strncpy(ap.ssid, r->ssid, sizeof(ap.ssid) - 1U);
    ap.rssi_dbm  = r->rssi;
    ap.auth_mode = 0U;                         /* the facade has no security mode */
    ap.channel   = r->channel;
    memcpy(ap.bssid, r->mac, sizeof(ap.bssid));

    taskENTER_CRITICAL();
    uint8_t pos = s_ap_count;
    if (s_ap_count < PLC_WIFI_MAX_APS) {
        s_ap_count++;
    } else if (ap.rssi_dbm <= s_ap[PLC_WIFI_MAX_APS - 1U].rssi_dbm) {
        taskEXIT_CRITICAL();
        return;                                /* full and weaker than the weakest kept */
    } else {
        pos = PLC_WIFI_MAX_APS - 1U;           /* replace the weakest */
    }
    while (pos > 0U && s_ap[pos - 1U].rssi_dbm < ap.rssi_dbm) {
        s_ap[pos] = s_ap[pos - 1U];
        pos--;
    }
    s_ap[pos] = ap;
    taskEXIT_CRITICAL();
}

/* May run inside st67w611_scan() (worker) or later in the vendor task. */
static void scan_cb(st67w611_status_t status, const st67w611_scan_result_t *result, void *user)
{
    (void)user;
    if (status != ST67W611_OK) {
        taskENTER_CRITICAL();
        s_scan_state = PLC_WIFI_SCAN_FAILED;
        taskEXIT_CRITICAL();
    } else if (result != NULL) {
        scan_insert(result);
    } else {
        taskENTER_CRITICAL();
        s_scan_state = PLC_WIFI_SCAN_COMPLETED;
        taskEXIT_CRITICAL();
    }
}

static void do_scan(void)
{
    const st67w611_status_t st = st67w611_scan(scan_cb, NULL);
    if (st != ST67W611_OK) {
        log_warn(TAG, "scan start refused: %s", st67w611_status_string(st));
        taskENTER_CRITICAL();
        s_scan_state = PLC_WIFI_SCAN_FAILED;
        taskEXIT_CRITICAL();
    }
}

/* ---- link state --------------------------------------------------------------------- */

/* Re-derive the link state from the facade. Worker only (may block on the module). */
static void refresh_link(void)
{
    st67w611_state_t fs;
    if (st67w611_get_state(&fs) != ST67W611_OK) {
        return;                                /* a facade operation is running: try later */
    }
    switch (fs) {
    case ST67W611_STATE_CONNECTED: {
        uint8_t ip[4], gw[4], nm[4];
        if (st67w611_get_ip(ip, gw, nm) == ST67W611_OK &&
            (ip[0] | ip[1] | ip[2] | ip[3]) != 0U) {
            const uint32_t v = ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                               ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
            live_set(PLC_WIFI_CONN_GOT_IP, 0U, v);
            s_ip_poll_until_ms = 0U;
            if (v != s_logged_ip) {            /* print once per new address */
                s_logged_ip = v;
                log_info(TAG, "Wi-Fi connected, IP %u.%u.%u.%u  gateway %u.%u.%u.%u  netmask %u.%u.%u.%u",
                         (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3],
                         (unsigned)gw[0], (unsigned)gw[1], (unsigned)gw[2], (unsigned)gw[3],
                         (unsigned)nm[0], (unsigned)nm[1], (unsigned)nm[2], (unsigned)nm[3]);
            }
        } else {
            live_set(PLC_WIFI_CONN_CONNECTED, 0U, 0U);
            if (s_ip_poll_until_ms == 0U) {    /* DHCP still running: poll for a while */
                s_ip_poll_until_ms = now_ms() + SPLC_HOST_WIFI_IP_WAIT_MS;
                if (s_ip_poll_until_ms == 0U) { s_ip_poll_until_ms = 1U; }
                s_ip_poll_next_ms = now_ms() + 500U;
            }
        }
        break;
    }
    case ST67W611_STATE_CONNECTING:
        live_set(PLC_WIFI_CONN_CONNECTING, 0U, 0U);
        break;
    case ST67W611_STATE_READY:
        live_set(PLC_WIFI_CONN_DISCONNECTED, 0U, 0U);
        s_ip_poll_until_ms = 0U;
        s_logged_ip = 0U;
        break;
    case ST67W611_STATE_ERROR:
        live_set(PLC_WIFI_CONN_DISCONNECTED, PLC_WIFI_ERR_DRIVER, 0U);
        s_ip_poll_until_ms = 0U;
        break;
    default:
        break;                                 /* OFF / INITIALIZING / RESETTING: leave as is */
    }
}

static void do_connect(void)
{
    splc_wifi_config_t cfg;
    uint32_t gen;

    taskENTER_CRITICAL();
    cfg = s_cfg;
    gen = s_gen;
    taskEXIT_CRITICAL();

    s_ip_poll_until_ms = 0U;
    const st67w611_status_t st = meshwf_st67_wifi_switch(cfg.ssid, cfg.password);

    if (st == ST67W611_OK) {
        refresh_link();
    } else {
        log_warn(TAG, "connect to \"%s\" failed: %s", cfg.ssid, st67w611_status_string(st));
        live_set(PLC_WIFI_CONN_DISCONNECTED, PLC_WIFI_ERR_DRIVER, 0U);
    }

    taskENTER_CRITICAL();
    if (s_gen == gen) {
        s_connecting = false;                  /* no newer CONNECT / DISCONNECT overtook us */
    }
    taskEXIT_CRITICAL();
    cfg.password[0] = '\0';                    /* do not leave the password on the stack */
}

static void do_disconnect(void)
{
    s_ip_poll_until_ms = 0U;
    const st67w611_status_t st = st67w611_disconnect();
    if (st != ST67W611_OK) {
        /* Nothing to disconnect (the vendor refuses after a failed join) is not an error. */
        log_debug(TAG, "disconnect: %s", st67w611_status_string(st));
    }
    live_set(PLC_WIFI_CONN_DISCONNECTED, 0U, 0U);
}

/* ---- callbacks of plc_wifi (engine task; must return at once) ----------------------- */

static int op_connect(void *ctx, const splc_wifi_config_t *cfg)
{
    (void)ctx;
    s_user_touched = true;                     /* a real configuration exists: no default */
    if (!s_ready) {
        return PLC_WIFI_ERR_HW_NOT_READY;
    }
    if ((cfg->control_flags & PLC_WIFI_CTRL_ENABLE) == 0U) {
        return PLC_WIFI_ERR_HW_NOT_READY;      /* Enable = 0: the App asks for Wi-Fi off */
    }
    if (((cfg->control_flags & PLC_WIFI_CTRL_MODE_MASK) >> PLC_WIFI_CTRL_MODE_SHIFT) != 0U) {
        log_warn(TAG, "connect refused: only station mode is supported");
        return PLC_WIFI_ERR_DRIVER;
    }
    if ((cfg->control_flags & PLC_WIFI_CTRL_DHCP) == 0U) {
        log_warn(TAG, "connect refused: static IP is not supported by the ST67 facade");
        return PLC_WIFI_ERR_DRIVER;
    }
    if (strlen(cfg->ssid) > ST67_SWITCH_SSID_MAX_LEN ||
        strlen(cfg->password) > ST67_SWITCH_PASSWORD_MAX_LEN) {
        log_warn(TAG, "connect refused: SSID > %u or password > %u characters",
                 (unsigned)ST67_SWITCH_SSID_MAX_LEN, (unsigned)ST67_SWITCH_PASSWORD_MAX_LEN);
        return PLC_WIFI_ERR_DRIVER;
    }
    if (s_scan_state == PLC_WIFI_SCAN_SCANNING) {
        return PLC_WIFI_ERR_BUSY;              /* the facade refuses connect while scanning */
    }

    taskENTER_CRITICAL();
    s_cfg = *cfg;
    s_connecting = true;
    s_gen++;
    s_cmd &= ~CMD_DISCONNECT;                  /* the newest request wins */
    taskEXIT_CRITICAL();
    post(CMD_CONNECT);
    return PLC_WIFI_ERR_NONE;
}

static int op_disconnect(void *ctx)
{
    (void)ctx;
    s_user_touched = true;                     /* the App wants it off / forgotten */
    if (!s_ready) {
        return PLC_WIFI_ERR_HW_NOT_READY;
    }
    taskENTER_CRITICAL();
    s_connecting = false;
    s_gen++;
    s_cmd &= ~CMD_CONNECT;
    taskEXIT_CRITICAL();
    post(CMD_DISCONNECT);
    return PLC_WIFI_ERR_NONE;
}

static bool op_status(void *ctx, splc_wifi_status_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    if (!s_ready) {
        return false;                          /* no state yet */
    }
    taskENTER_CRITICAL();
    const live_t live = s_live;
    const bool connecting = s_connecting;
    out->scan_state = s_scan_state;
    out->ap_count   = s_ap_count;
    taskEXIT_CRITICAL();

    out->conn_status = connecting ? PLC_WIFI_CONN_CONNECTING : live.conn;
    out->error_code  = connecting ? 0U : live.err;
    out->ip          = live.ip;
    if (live.mac_valid) {
        memcpy(out->mac, live.mac, sizeof(out->mac));
    }
    out->rssi_dbm = 0;                         /* the facade has no RSSI (see the header) */
    return true;
}

static int op_scan_start(void *ctx)
{
    (void)ctx;
    if (!s_ready) {
        return PLC_WIFI_ERR_HW_NOT_READY;
    }
    if (s_scan_state == PLC_WIFI_SCAN_SCANNING || s_connecting) {
        return PLC_WIFI_ERR_BUSY;              /* connect and scan exclude each other in the facade */
    }
    taskENTER_CRITICAL();
    s_ap_count = 0U;
    s_scan_state = PLC_WIFI_SCAN_SCANNING;
    s_scan_deadline_ms = now_ms() + SPLC_HOST_WIFI_SCAN_TIMEOUT_MS;
    taskEXIT_CRITICAL();
    post(CMD_SCAN);
    return PLC_WIFI_ERR_NONE;
}

static bool op_scan_ap(void *ctx, uint8_t index, splc_wifi_ap_t *out)
{
    (void)ctx;
    bool ok = false;
    taskENTER_CRITICAL();
    if (index < s_ap_count) {
        *out = s_ap[index];
        ok = true;
    }
    taskEXIT_CRITICAL();
    return ok;
}

#if SPLC_HOST_WIFI_PERSIST
static bool op_load(void *ctx, splc_wifi_config_t *out)
{
    (void)ctx;
    const bool found = meshwf_wifi_store_load(out);
    s_stored = found;
    s_load_done = true;
    return found;
}

static bool op_save(void *ctx, const splc_wifi_config_t *cfg)
{
    (void)ctx;
    return meshwf_wifi_store_save(cfg);
}

static void op_erase(void *ctx)
{
    (void)ctx;
    s_user_touched = true;                     /* FORGET / FACTORY_RESET: no default either */
    meshwf_wifi_store_erase();
}
#endif

void splc_wifi_port_init(void)
{
    static const splc_wifi_ops_t ops = {
        .ctx        = NULL,
        .connect    = op_connect,
        .disconnect = op_disconnect,
        .status     = op_status,
        .scan_start = op_scan_start,
        .scan_ap    = op_scan_ap,
#if SPLC_HOST_WIFI_PERSIST
        .load       = op_load,
        .save       = op_save,
        .erase      = op_erase,
#else
        .load       = NULL,
        .save       = NULL,
        .erase      = NULL,
#endif
    };

    if (!splc_wifi_register(&ops)) {
        log_error(TAG, "plc_wifi registration failed");
        return;
    }
#if !SPLC_HOST_WIFI_PERSIST
    s_load_done = true;                        /* nothing to load: the default may be used */
#endif
#if !SPLC_HOST_WIFI_PERSIST
    log_warn(TAG, "no Wi-Fi persistence: credentials are lost at power-off");
#endif
}

/* ---- ST67 task ---------------------------------------------------------------------- */

void splc_wifi_port_set_ready(bool ready)
{
    s_ready = ready;
    if (ready) {
        s_mac_next_ms = 0U;
        post(CMD_REFRESH);
    } else {
        live_set(PLC_WIFI_CONN_DISCONNECTED, PLC_WIFI_ERR_HW_NOT_READY, 0U);
    }
}

/* ---- link state log ----------------------------------------------------------------- */

static const char *conn_name(uint16_t c)
{
    switch (c) {
    case PLC_WIFI_CONN_DISCONNECTED:     return "DISCONNECTED";
    case PLC_WIFI_CONN_CONNECTING:       return "CONNECTING";
    case PLC_WIFI_CONN_CONNECTED:        return "CONNECTED (no IP yet)";
    case PLC_WIFI_CONN_GOT_IP:           return "GOT_IP";
    case PLC_WIFI_CONN_AUTH_FAILED:      return "AUTH_FAILED";
    case PLC_WIFI_CONN_SSID_NOT_FOUND:   return "SSID_NOT_FOUND";
    case PLC_WIFI_CONN_AP_MODE_ACTIVE:   return "AP_MODE_ACTIVE";
    default:                             return "UNKNOWN";
    }
}

/* Worker only. Prints one log_info line each time the real link state (or its error) changes.
 * Never called from the vendor callback context, so the events stay cheap. */
static void link_log(void)
{
    taskENTER_CRITICAL();
    const uint16_t conn = s_live.conn;
    const uint16_t err  = s_live.err;
    taskEXIT_CRITICAL();

    if (conn == s_logged_conn && err == s_logged_err) {
        return;
    }
    if (conn == PLC_WIFI_CONN_DISCONNECTED) {
        s_logged_ip = 0U;                      /* the next IP is printed again */
    }
    if (err != 0U) {
        log_info(TAG, "Wi-Fi link: %s (error %u)", conn_name(conn), (unsigned)err);
    } else {
        log_info(TAG, "Wi-Fi link: %s", conn_name(conn));
    }
    s_logged_conn = conn;
    s_logged_err = err;
}

/* ---- default network ---------------------------------------------------------------- */

static void default_connect(void)
{
    uint32_t gen;

    taskENTER_CRITICAL();
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.control_flags = PLC_WIFI_CTRL_ENABLE | PLC_WIFI_CTRL_DHCP;
    strncpy(s_cfg.ssid, WIFI_SSID_DEFAULT, sizeof(s_cfg.ssid) - 1U);
    strncpy(s_cfg.password, WIFI_PASSWORD_DEFAULT, sizeof(s_cfg.password) - 1U);
    s_connecting = true;
    s_gen++;
    gen = s_gen;
    taskEXIT_CRITICAL();

    log_info(TAG, "no Wi-Fi configuration from the App or the Flash: connecting to the default \"%s\"",
             WIFI_SSID_DEFAULT);
    do_connect();

    taskENTER_CRITICAL();
    if (s_gen == gen) {
        memset(s_cfg.password, 0, sizeof(s_cfg.password));   /* connect finished, nobody newer */
    }
    taskEXIT_CRITICAL();
}

/* Worker only. The module restarted and dropped the AP link. A network set by the App or restored
 * from the Flash is NOT rejoined by default_service(), so do it here from s_cfg (the password stays
 * there after op_connect). Nothing to do when the link is up or joining, a connect is already
 * running, Wi-Fi is disabled or no SSID is known. */
static void rejoin_after_module_restart(void)
{
    if (!s_stored && !s_user_touched) {
        return;                                /* default network: default_service() does it */
    }
    bool go = false;
    taskENTER_CRITICAL();
    const uint16_t conn = s_live.conn;
    if (!s_connecting && s_cfg.ssid[0] != '\0' && (s_cfg.control_flags & PLC_WIFI_CTRL_ENABLE) != 0U &&
        conn != PLC_WIFI_CONN_CONNECTED && conn != PLC_WIFI_CONN_GOT_IP &&
        conn != PLC_WIFI_CONN_CONNECTING) {
        s_connecting = true;
        s_gen++;
        go = true;
    }
    taskEXIT_CRITICAL();
    if (!go) {
        return;
    }
    log_info(TAG, "module restarted: rejoining the configured Wi-Fi network");
    do_connect();                              /* clears s_connecting when it finishes */
}

/* Worker only. Decides, every pass, whether the default network may be joined. */
static void default_service(uint32_t now)
{
    if (!s_ready || !s_load_done) {
        return;                                /* wait for the module and for the Flash load */
    }
    if (s_stored || s_user_touched) {
        if (!s_default_logged) {
            s_default_logged = true;
            log_debug(TAG, "default Wi-Fi not used (stored=%d, app_or_library_request=%d)",
                      (int)s_stored, (int)s_user_touched);
        }
        return;
    }
    if (WIFI_SSID_DEFAULT[0] == '\0') {
        return;                                /* no default compiled in */
    }
    if (strlen(WIFI_SSID_DEFAULT) > ST67_SWITCH_SSID_MAX_LEN ||
        strlen(WIFI_PASSWORD_DEFAULT) > ST67_SWITCH_PASSWORD_MAX_LEN) {
        if (!s_default_logged) {
            s_default_logged = true;
            log_warn(TAG, "default Wi-Fi refused: SSID > %u or password > %u characters",
                     (unsigned)ST67_SWITCH_SSID_MAX_LEN, (unsigned)ST67_SWITCH_PASSWORD_MAX_LEN);
        }
        return;
    }
    if (s_cmd != 0U || s_connecting || s_scan_state == PLC_WIFI_SCAN_SCANNING) {
        return;                                /* the App is busy with it */
    }
    if (s_default_next_ms != 0U && !time_reached(now, s_default_next_ms)) {
        return;
    }

    taskENTER_CRITICAL();
    const uint16_t conn = s_live.conn;
    taskEXIT_CRITICAL();
    if (conn == PLC_WIFI_CONN_CONNECTING || conn == PLC_WIFI_CONN_CONNECTED ||
        conn == PLC_WIFI_CONN_GOT_IP) {
        return;                                /* already joined (or joining): leave it */
    }

    default_connect();
    s_default_next_ms = now_ms() + SPLC_HOST_WIFI_DEFAULT_RETRY_MS;
    if (s_default_next_ms == 0U) { s_default_next_ms = 1U; }
}

void splc_wifi_port_service(uint32_t wait_ms)
{
    if (s_task == NULL) {
        s_task = xTaskGetCurrentTaskHandle();
    }
    if (s_cmd == 0U) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }

    const uint32_t cmds = take_cmds();
    if (!s_ready) {
        return;
    }

    if ((cmds & CMD_DISCONNECT) != 0U) { do_disconnect(); }
    if ((cmds & CMD_CONNECT) != 0U)    { do_connect(); }
    if ((cmds & CMD_SCAN) != 0U)       { do_scan(); }
    if ((cmds & CMD_REFRESH) != 0U && !s_connecting) { refresh_link(); }

    const uint32_t now = now_ms();

    /* The MAC does not change: read it once, retry every 2 s until the module answers. */
    if (!s_live.mac_valid && time_reached(now, s_mac_next_ms)) {
        uint8_t mac[6];
        if (st67w611_get_mac(mac) == ST67W611_OK) {
            taskENTER_CRITICAL();
            memcpy(s_live.mac, mac, sizeof(mac));
            s_live.mac_valid = true;
            taskEXIT_CRITICAL();
        } else {
            s_mac_next_ms = now + 2000U;
        }
    }

    /* Connected but no address yet (DHCP): ask again every 500 ms, for a limited time. */
    if (s_ip_poll_until_ms != 0U && !s_connecting && time_reached(now, s_ip_poll_next_ms)) {
        if (time_reached(now, s_ip_poll_until_ms)) {
            s_ip_poll_until_ms = 0U;           /* give up: stays CONNECTED, never GOT_IP */
            log_warn(TAG, "no IP address from the access point");
        } else {
            s_ip_poll_next_ms = now + 500U;
            refresh_link();
        }
    }

    if (s_rejoin_pending) {
        s_rejoin_pending = false;
        rejoin_after_module_restart();
    }
    default_service(now);

    /* Idle: re-read the link now and then so a dropped link shows up in the log. */
    if (!s_connecting && s_scan_state != PLC_WIFI_SCAN_SCANNING &&
        time_reached(now_ms(), s_link_next_ms)) {
        s_link_next_ms = now_ms() + SPLC_HOST_WIFI_LINK_CHECK_MS;
        if (s_link_next_ms == 0U) { s_link_next_ms = 1U; }
        refresh_link();
    }
    link_log();

    /* A scan whose end never arrives must not leave the App waiting forever. */
    if (s_scan_state == PLC_WIFI_SCAN_SCANNING && time_reached(now, s_scan_deadline_ms)) {
        log_warn(TAG, "scan timed out");
        taskENTER_CRITICAL();
        s_scan_state = PLC_WIFI_SCAN_FAILED;
        taskEXIT_CRITICAL();
    }
}

#endif /* SPLC_WIFI_ENABLE */