#include "st67_cert_store.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "plc_retain.h"
#include "splc_flash_define.h"
#include "splc_log.h"
#include "logger.h"
#include "sx_flash.h"

static const char *TAG = "ST67_CERT";

/* ------------------------------------------------------------------------------------ */
/* Geometry                                                                              */
/* ------------------------------------------------------------------------------------ */

#define SECTOR_SIZE   ((uint32_t)SPLC_FLASH_SECTOR_SIZE)

/* The sector right below SimplePLC's data region (Retain + Rule Table A/B). */
#ifndef ST67_CERT_BASE_ADDR
#define ST67_CERT_BASE_ADDR   ((uint32_t)(SPLC_FLASH_DATA_BASE_ADDR - SPLC_FLASH_SECTOR_SIZE))
#endif

#ifndef __cplusplus
_Static_assert(ST67_CERT_BASE_ADDR % SPLC_FLASH_SECTOR_SIZE == 0U,
               "certificate sector must be 8 KB aligned");
_Static_assert(ST67_CERT_BASE_ADDR >= FLASH_BASE + SPLC_FLASH_BANK_SIZE,
               "certificate sector must lie in bank 2, like the SimplePLC data sectors");
#endif

/* Memory-mapped read pointer for a Flash address (overridable so the logic can run on a PC). */
#ifndef ST67_CERT_FLASH_PTR
#define ST67_CERT_FLASH_PTR(a)   ((const uint8_t *)(uintptr_t)(a))
#endif

#define HDR_SIZE      16U
#define TRL_SIZE      16U
#define MAGIC_HDR     0x54524543UL    /* "CERT" little-endian */
#define MAGIC_TRL     0x454E4F44UL    /* "DONE" little-endian */

/* ------------------------------------------------------------------------------------ */
/* State                                                                                 */
/* ------------------------------------------------------------------------------------ */

typedef struct {
    bool     valid;
    uint32_t off;      /* record offset inside the sector */
    uint32_t seq;
    uint16_t len;      /* PEM bytes without the NUL */
} live_t;

static live_t   s_live[4];            /* index = type 1..3 */
static uint32_t s_free_off;           /* first erased byte of the log; SECTOR_SIZE = no room */
static uint32_t s_seq_max;
static bool     s_inited;
static volatile uint32_t s_generation;

static struct {
    bool     open;
    uint8_t  type;
    uint16_t total;
    uint16_t written;
    uint8_t  next_index;
    uint32_t rec_off;
    uint32_t pw;          /* Flash address of the next payload quad-word */
    uint16_t crc;
    uint8_t  acc[16];
    uint8_t  acc_n;
} s_tx;

static volatile uint8_t s_readers;
static volatile bool    s_erasing;
static volatile bool    s_clear_pending;

/* ------------------------------------------------------------------------------------ */
/* Helpers                                                                               */
/* ------------------------------------------------------------------------------------ */

static uint16_t crc16(uint16_t crc, const uint8_t *p, uint32_t n)
{
    while (n--) {
        crc ^= *p++;
        for (uint8_t b = 0; b < 8U; b++) {
            crc = (uint16_t)((crc & 1U) ? ((crc >> 1) ^ 0xA001U) : (crc >> 1));
        }
    }
    return crc;
}

static uint32_t round16(uint32_t n)
{
    return (n + 15U) & ~15U;
}

/* Whole record: header + (PEM + NUL) padded to 16 B + trailer. */
static uint32_t rec_size(uint16_t len)
{
    return HDR_SIZE + round16((uint32_t)len + 1U) + TRL_SIZE;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool all_ff(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != 0xFFU) {
            return false;
        }
    }
    return true;
}

static void flash_begin(void)
{
    retain_flash_op_begin();
    sx_flash_unlock();
}

static void flash_end(void)
{
    sx_flash_lock();
    retain_flash_op_end();
}

static void build_header(uint8_t h[HDR_SIZE], uint8_t type, uint16_t len, uint32_t seq)
{
    memset(h, 0xFF, HDR_SIZE);
    put32(&h[0], MAGIC_HDR);
    put32(&h[4], seq);
    h[8]  = (uint8_t)len;
    h[9]  = (uint8_t)(len >> 8);
    h[10] = type;
    h[11] = 0U;
    const uint16_t c = crc16(0xFFFFU, h, 12U);
    h[12] = (uint8_t)c;
    h[13] = (uint8_t)(c >> 8);
    h[14] = 0U;
    h[15] = 0U;
}

/* Parse a header image. false = not a valid header. */
static bool parse_header(const uint8_t h[HDR_SIZE], uint8_t *type, uint16_t *len, uint32_t *seq)
{
    if (get32(&h[0]) != MAGIC_HDR) {
        return false;
    }
    const uint16_t c = (uint16_t)(h[12] | ((uint16_t)h[13] << 8));
    if (crc16(0xFFFFU, h, 12U) != c) {
        return false;
    }
    const uint16_t l = (uint16_t)(h[8] | ((uint16_t)h[9] << 8));
    const uint8_t  t = h[10];
    if (t < ST67_CERT_ROOT_CA || t > ST67_CERT_CLIENT_KEY || l == 0U || l > ST67_CERT_MAX_SIZE) {
        return false;
    }
    *type = t;
    *len  = l;
    *seq  = get32(&h[4]);
    return true;
}

/* CRC of the payload (PEM + NUL) of the record at `off`, read straight from Flash. */
static uint16_t payload_crc(uint32_t off, uint16_t len)
{
    const uint8_t *p = ST67_CERT_FLASH_PTR(ST67_CERT_BASE_ADDR + off + HDR_SIZE);
    return crc16(0xFFFFU, p, (uint32_t)len + 1U);
}

/* The trailer is present and matches the payload (PEM must also end with the NUL). */
static bool record_complete(uint32_t off, uint16_t len)
{
    uint8_t t[TRL_SIZE];
    sx_flash_read(ST67_CERT_BASE_ADDR + off + HDR_SIZE + round16((uint32_t)len + 1U), t, TRL_SIZE);
    if (get32(&t[0]) != MAGIC_TRL) {
        return false;
    }
    const uint16_t c = (uint16_t)(t[4] | ((uint16_t)t[5] << 8));
    if (payload_crc(off, len) != c) {
        return false;
    }
    return *ST67_CERT_FLASH_PTR(ST67_CERT_BASE_ADDR + off + HDR_SIZE + len) == 0U;
}

/* Walk the log: find the live record of each type and the first free offset. */
static void rescan(void)
{
    memset(s_live, 0, sizeof(s_live));
    s_seq_max  = 0U;
    s_free_off = SECTOR_SIZE;

    uint32_t off = 0U;
    while (off + HDR_SIZE <= SECTOR_SIZE) {
        uint8_t h[HDR_SIZE];
        sx_flash_read(ST67_CERT_BASE_ADDR + off, h, HDR_SIZE);
        if (all_ff(h, HDR_SIZE)) {
            s_free_off = off;                       /* erased: the log ends here */
            break;
        }
        uint8_t  type;
        uint16_t len;
        uint32_t seq;
        if (!parse_header(h, &type, &len, &seq) || off + rec_size(len) > SECTOR_SIZE) {
            /* Garbage that cannot be skipped: refuse to append after it. The next begin()
             * compacts (erases) the sector, which also cleans it. */
            log_warn(TAG, "corrupt record at +0x%04lX: sector needs compaction", (unsigned long)off);
            s_free_off = SECTOR_SIZE;
            break;
        }
        if (seq > s_seq_max) {
            s_seq_max = seq;
        }
        if (record_complete(off, len) && (!s_live[type].valid || seq > s_live[type].seq)) {
            s_live[type].valid = true;
            s_live[type].off   = off;
            s_live[type].seq   = seq;
            s_live[type].len   = len;
        }
        off += rec_size(len);                       /* complete or not, the length is known */
    }
    s_inited = true;
}

static void ensure_init(void)
{
    if (!s_inited) {
        rescan();
    }
}

/* Erase guard: refuse while a reader holds pointers into the sector. */
static bool erase_enter(void)
{
    bool ok;
    taskENTER_CRITICAL();
    ok = (s_readers == 0U);
    if (ok) {
        s_erasing = true;
    }
    taskEXIT_CRITICAL();
    return ok;
}

static void erase_leave(void)
{
    s_erasing = false;
}

static void tx_reset(void)
{
    memset(&s_tx, 0, sizeof(s_tx));
}

/* ------------------------------------------------------------------------------------ */
/* Compaction: keep the live records of every type except `replace`                      */
/* ------------------------------------------------------------------------------------ */

static uint32_t others_size(uint8_t replace)
{
    uint32_t sum = 0U;
    for (uint8_t t = ST67_CERT_ROOT_CA; t <= ST67_CERT_CLIENT_KEY; t++) {
        if (t != replace && s_live[t].valid) {
            sum += rec_size(s_live[t].len);
        }
    }
    return sum;
}

static int compact(uint8_t replace)
{
    const uint32_t total = others_size(replace);
    uint8_t *buf = NULL;
    uint8_t kept_mask = 0U;                          /* bit t set = type t is live and kept */
    for (uint8_t t = ST67_CERT_ROOT_CA; t <= ST67_CERT_CLIENT_KEY; t++) {
        if (t != replace && s_live[t].valid) {
            kept_mask |= (uint8_t)(1U << t);
        }
    }

    if (!erase_enter()) {
        return ST67_CERT_ERR_BUSY;
    }
    if (total != 0U) {
        buf = (uint8_t *)pvPortMalloc(total);
        if (buf == NULL) {
            erase_leave();
            log_error(TAG, "compaction needs %lu B of heap: not enough, nothing changed",
                      (unsigned long)total);
            return ST67_CERT_ERR_STORAGE;
        }
        uint32_t pos = 0U;
        for (uint8_t t = ST67_CERT_ROOT_CA; t <= ST67_CERT_CLIENT_KEY; t++) {
            if (t != replace && s_live[t].valid) {
                const uint32_t n = rec_size(s_live[t].len);
                sx_flash_read(ST67_CERT_BASE_ADDR + s_live[t].off, &buf[pos], n);
                pos += n;
            }
        }
    }

    flash_begin();
    sx_flash_erase(ST67_CERT_BASE_ADDR, SECTOR_SIZE);
    if (total != 0U) {
        sx_flash_write(ST67_CERT_BASE_ADDR, buf, total);
    }
    flash_end();
    if (buf != NULL) {
        vPortFree(buf);
    }

    rescan();
    erase_leave();

    /* Every kept type must have survived the rewrite; only `replace` may be gone. */
    bool ok = (s_free_off == total);
    for (uint8_t t = ST67_CERT_ROOT_CA; t <= ST67_CERT_CLIENT_KEY; t++) {
        if (t == replace) {
            continue;
        }
        if (((kept_mask >> t) & 1U) != 0U && !s_live[t].valid) {
            ok = false;
        }
    }
    if (!ok) {
        log_error(TAG, "compaction verify failed (free offset 0x%lX, expected 0x%lX)",
                  (unsigned long)s_free_off, (unsigned long)total);
        return ST67_CERT_ERR_STORAGE;
    }
    s_generation++;                                  /* the old copy of `replace` is gone */
    return ST67_CERT_OK;
}

/* ------------------------------------------------------------------------------------ */
/* Writer API (engine task)                                                              */
/* ------------------------------------------------------------------------------------ */

void st67_cert_store_init(void)
{
    rescan();
}

int st67_cert_store_begin(uint8_t type, uint16_t total_size)
{
    ensure_init();
    if (type < ST67_CERT_ROOT_CA || type > ST67_CERT_CLIENT_KEY || total_size == 0U) {
        return ST67_CERT_ERR_STATE;
    }
    if (total_size > ST67_CERT_MAX_SIZE) {
        return ST67_CERT_ERR_OVERFLOW;
    }
    if (s_tx.open) {
        st67_cert_store_abort();                     /* a restart: the half record stays dead */
    }

    const uint32_t need = rec_size(total_size);
    if (s_free_off + need > SECTOR_SIZE) {
        /* Does not fit at the tail: would it fit after dropping the dead records? */
        if (others_size(type) + need > SECTOR_SIZE) {
            log_warn(TAG, "type %u (%u B) does not fit: other certificates use %lu B of %lu",
                     (unsigned)type, (unsigned)total_size, (unsigned long)others_size(type),
                     (unsigned long)SECTOR_SIZE);
            return ST67_CERT_ERR_OVERFLOW;
        }
        const int rc = compact(type);
        if (rc != ST67_CERT_OK) {
            return rc;
        }
        if (s_free_off + need > SECTOR_SIZE) {       /* cannot happen after the size check */
            return ST67_CERT_ERR_STORAGE;
        }
    }

    uint8_t h[HDR_SIZE];
    s_seq_max++;
    build_header(h, type, total_size, s_seq_max);
    flash_begin();
    sx_flash_write(ST67_CERT_BASE_ADDR + s_free_off, h, HDR_SIZE);
    flash_end();

    uint8_t rb[HDR_SIZE];
    sx_flash_read(ST67_CERT_BASE_ADDR + s_free_off, rb, HDR_SIZE);
    if (memcmp(rb, h, HDR_SIZE) != 0) {
        log_error(TAG, "header write did not verify at +0x%04lX", (unsigned long)s_free_off);
        rescan();                                    /* works out where the log really ends */
        return ST67_CERT_ERR_STORAGE;
    }

    tx_reset();
    s_tx.open    = true;
    s_tx.type    = type;
    s_tx.total   = total_size;
    s_tx.rec_off = s_free_off;
    s_tx.pw      = ST67_CERT_BASE_ADDR + s_free_off + HDR_SIZE;
    s_tx.crc     = 0xFFFFU;
    /* The record now occupies its whole size even if the transfer dies. */
    s_free_off  += need;
    return ST67_CERT_OK;
}

int st67_cert_store_chunk(uint8_t type, uint8_t index, const uint8_t *data, uint8_t len)
{
    if (!s_tx.open || type != s_tx.type || index != s_tx.next_index || data == NULL || len == 0U) {
        return ST67_CERT_ERR_STATE;
    }
    if ((uint32_t)s_tx.written + len > s_tx.total) {
        return ST67_CERT_ERR_OVERFLOW;
    }

    flash_begin();
    for (uint8_t i = 0; i < len; i++) {
        s_tx.acc[s_tx.acc_n++] = data[i];
        if (s_tx.acc_n == 16U) {
            sx_flash_write(s_tx.pw, s_tx.acc, 16U);
            s_tx.pw   += 16U;
            s_tx.acc_n = 0U;
        }
    }
    flash_end();

    s_tx.crc = crc16(s_tx.crc, data, len);
    s_tx.written = (uint16_t)(s_tx.written + len);
    s_tx.next_index++;
    return ST67_CERT_OK;
}

int st67_cert_store_commit(uint8_t type)
{
    if (!s_tx.open || type != s_tx.type || s_tx.written != s_tx.total) {
        return ST67_CERT_ERR_STATE;
    }

    /* Last payload quad-word: the remaining bytes, the NUL, then 0xFF padding. */
    static const uint8_t nul = 0U;
    s_tx.acc[s_tx.acc_n++] = nul;
    s_tx.crc = crc16(s_tx.crc, &nul, 1U);
    if (s_tx.acc_n < 16U) {
        memset(&s_tx.acc[s_tx.acc_n], 0xFF, 16U - s_tx.acc_n);
    }

    uint8_t t[TRL_SIZE];
    memset(t, 0xFF, sizeof(t));
    put32(&t[0], MAGIC_TRL);
    t[4] = (uint8_t)s_tx.crc;
    t[5] = (uint8_t)(s_tx.crc >> 8);

    flash_begin();
    sx_flash_write(s_tx.pw, s_tx.acc, 16U);
    sx_flash_write(s_tx.pw + 16U, t, TRL_SIZE);      /* the trailer makes the record valid */
    flash_end();

    const uint32_t rec_off = s_tx.rec_off;
    const uint8_t  ty      = s_tx.type;
    tx_reset();

    rescan();                                        /* verifies header, payload CRC, trailer */
    if (!s_live[ty].valid || s_live[ty].off != rec_off) {
        log_error(TAG, "commit of type %u did not verify: the previous certificate stays live",
                  (unsigned)ty);
        return ST67_CERT_ERR_STORAGE;
    }
    s_generation++;
    log_info(TAG, "type %u stored: %u B at +0x%04lX seq %lu", (unsigned)ty,
             (unsigned)s_live[ty].len, (unsigned long)rec_off, (unsigned long)s_live[ty].seq);
    return ST67_CERT_OK;
}

void st67_cert_store_abort(void)
{
    /* The header already sits in Flash and s_free_off already skips the record: it stays
     * a dead record that rescan() jumps over. Nothing to undo. */
    tx_reset();
}

void st67_cert_store_clear(void)
{
    ensure_init();
    st67_cert_store_abort();
    if (s_free_off == 0U && !s_live[1].valid && !s_live[2].valid && !s_live[3].valid) {
        return;                                      /* already empty: spare the erase cycle */
    }
    if (!erase_enter()) {
        s_clear_pending = true;                      /* a reader holds pointers: do it later */
        return;
    }
    flash_begin();
    sx_flash_erase(ST67_CERT_BASE_ADDR, SECTOR_SIZE);
    flash_end();
    rescan();
    erase_leave();
    s_clear_pending = false;
    s_generation++;
    log_info(TAG, "all certificates erased");
}

void st67_cert_store_service(void)
{
    if (s_clear_pending) {
        st67_cert_store_clear();
    }
}

/* ------------------------------------------------------------------------------------ */
/* Reader API                                                                            */
/* ------------------------------------------------------------------------------------ */

uint16_t st67_cert_store_mask(void)
{
    ensure_init();
    uint16_t m = 0U;
    for (uint8_t t = ST67_CERT_ROOT_CA; t <= ST67_CERT_CLIENT_KEY; t++) {
        if (s_live[t].valid) {
            m |= (uint16_t)(1U << (t - 1U));
        }
    }
    return m;
}

uint32_t st67_cert_store_generation(void)
{
    return s_generation;
}

bool st67_cert_store_acquire(void)
{
    bool ok;
    ensure_init();
    taskENTER_CRITICAL();
    ok = !s_erasing;
    if (ok && s_readers < 255U) {
        s_readers++;
    }
    taskEXIT_CRITICAL();
    return ok;
}

void st67_cert_store_release(void)
{
    taskENTER_CRITICAL();
    if (s_readers != 0U) {
        s_readers--;
    }
    taskEXIT_CRITICAL();
}

const char *st67_cert_store_get(uint8_t type, size_t *len)
{
    if (type < ST67_CERT_ROOT_CA || type > ST67_CERT_CLIENT_KEY || !s_live[type].valid) {
        return NULL;
    }
    if (len != NULL) {
        *len = s_live[type].len;
    }
    return (const char *)ST67_CERT_FLASH_PTR(ST67_CERT_BASE_ADDR + s_live[type].off + HDR_SIZE);
}