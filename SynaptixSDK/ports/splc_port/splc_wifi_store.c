/**
 * @file splc_wifi_store.c
 * @brief Wi-Fi credential storage for the meshwf board (SPLC_HOST_WIFI_PERSIST = 1).
 *
 * Implements meshwf_wifi_store_load/save/erase() declared in splc_wifi_port.h. The three
 * functions are called synchronously from the ENGINE task (the only task allowed to touch
 * SimplePLC Flash), so there is no lock here.
 *
 * Flash: ONE 8 KB sector, two sectors below SimplePLC's data region
 *   (SPLC_FLASH_DATA_BASE_ADDR - 2 sectors). The sector right below the data region belongs
 *   to st67_cert_store (MQTT TLS certificates), so it is left alone. Override the address with
 *   -DMESHWF_WIFI_STORE_ADDR=0x... if needed (must be 8 KB aligned, in bank 2).
 *
 * Format: the sector is a log of 160-byte slots (51 per sector). A save programs the next
 * blank slot; the sector is erased only when it is full, so a normal save is power-fail safe
 * (an interrupted slot fails its CRC and the previous record stays valid). The record with
 * the highest seq and a good CRC wins. erase() writes a "forgotten" record (state 2).
 * Nothing is written when the credentials did not change (no wear).
 *
 * Slot layout (little endian, bytes):
 *    0 magic u32   4 seq u32   8 state u16   10 control_flags u16
 *   12 ssid[33]   45 password[65]   112 static_ip u32  116 subnet u32  120 gateway u32
 *  124 dns u32   128 crc32 u32 (over bytes 0..127)   132..159 = 0xFF
 *
 * The password is never logged.
 */
#include "splc_wifi_port.h"

#if SPLC_WIFI_ENABLE && SPLC_HOST_WIFI_PERSIST

#include <string.h>

#include "plc_retain.h"
#include "splc_flash_define.h"
#include "splc_log.h"
#include "logger.h"
#include "sx_flash.h"

static const char *TAG = "WIFI_STORE";

#define SECTOR_SIZE   ((uint32_t)SPLC_FLASH_SECTOR_SIZE)

#ifndef MESHWF_WIFI_STORE_ADDR
#define MESHWF_WIFI_STORE_ADDR   ((uint32_t)(SPLC_FLASH_DATA_BASE_ADDR - 2U * SPLC_FLASH_SECTOR_SIZE))
#endif

_Static_assert(MESHWF_WIFI_STORE_ADDR % SPLC_FLASH_SECTOR_SIZE == 0U,
               "MESHWF_WIFI_STORE_ADDR must be 8 KB (sector) aligned");
_Static_assert(MESHWF_WIFI_STORE_ADDR >= FLASH_BASE + SPLC_FLASH_BANK_SIZE,
               "the Wi-Fi store must lie in bank 2 (code runs from bank 1 while it is erased)");

#define SLOT_SIZE     160U                       /* multiple of 16 (quad-word programming) */
#define SLOT_COUNT    (SECTOR_SIZE / SLOT_SIZE)  /* 51 */
#define CRC_SPAN      128U
#define REC_MAGIC     0x57494649UL               /* "WIFI" */

#define OFF_MAGIC     0U
#define OFF_SEQ       4U
#define OFF_STATE     8U
#define OFF_FLAGS     10U
#define OFF_SSID      12U
#define OFF_PASS      45U
#define OFF_IP        112U
#define OFF_SUBNET    116U
#define OFF_GATEWAY   120U
#define OFF_DNS       124U
#define OFF_CRC       128U

#define STATE_VALID   1U
#define STATE_ERASED  2U

typedef struct {
    bool     found;          /* a CRC-valid record exists */
    uint32_t seq;
    uint32_t idx;
    uint16_t state;
    bool     has_blank;
    uint32_t first_blank;
} scan_t;

extern uint32_t _sidata, _sdata, _edata;     /* STM32H523xx_FLASH.ld */

static uint32_t crc32_calc(const uint8_t *p, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return ~crc;
}

static uint32_t get32(const uint8_t *b, uint32_t off) { uint32_t v; memcpy(&v, b + off, 4); return v; }
static uint16_t get16(const uint8_t *b, uint32_t off) { uint16_t v; memcpy(&v, b + off, 2); return v; }
static void put32(uint8_t *b, uint32_t off, uint32_t v) { memcpy(b + off, &v, 4); }
static void put16(uint8_t *b, uint32_t off, uint16_t v) { memcpy(b + off, &v, 2); }

static uint32_t slot_addr(uint32_t i) { return MESHWF_WIFI_STORE_ADDR + i * SLOT_SIZE; }

/* The firmware image (end of .data in Flash) must stay below the store. */
static bool area_is_free(void)
{
    const uint32_t image_end = (uint32_t)&_sidata + ((uint32_t)&_edata - (uint32_t)&_sdata);
    if (image_end > MESHWF_WIFI_STORE_ADDR) {
        log_error(TAG, "firmware image ends at 0x%08lX, over the Wi-Fi store at 0x%08lX: not used",
                  (unsigned long)image_end, (unsigned long)MESHWF_WIFI_STORE_ADDR);
        return false;
    }
    return true;
}

static bool slot_blank(const uint8_t *b)
{
    for (uint32_t i = 0; i < SLOT_SIZE; i++) {
        if (b[i] != 0xFFU) {
            return false;
        }
    }
    return true;
}

static bool slot_valid(const uint8_t *b)
{
    if (get32(b, OFF_MAGIC) != (uint32_t)REC_MAGIC) {
        return false;
    }
    if (crc32_calc(b, CRC_SPAN) != get32(b, OFF_CRC)) {
        return false;
    }
    const uint16_t st = get16(b, OFF_STATE);
    return st == STATE_VALID || st == STATE_ERASED;
}

static void scan(scan_t *s, uint8_t *best_copy)
{
    uint8_t buf[SLOT_SIZE] __attribute__((aligned(4)));
    memset(s, 0, sizeof(*s));
    for (uint32_t i = 0; i < SLOT_COUNT; i++) {
        sx_flash_read(slot_addr(i), buf, SLOT_SIZE);
        if (slot_blank(buf)) {
            if (!s->has_blank) {
                s->has_blank = true;
                s->first_blank = i;
            }
            continue;
        }
        if (!slot_valid(buf)) {
            continue;                 /* interrupted write: skipped, never reused until erase */
        }
        const uint32_t seq = get32(buf, OFF_SEQ);
        if (!s->found || (int32_t)(seq - s->seq) > 0) {
            s->found = true;
            s->seq = seq;
            s->idx = i;
            s->state = get16(buf, OFF_STATE);
            if (best_copy != NULL) {
                memcpy(best_copy, buf, SLOT_SIZE);
            }
        }
    }
}

static void erase_sector(void)
{
    retain_flash_op_begin();
    sx_flash_unlock();
    sx_flash_erase(MESHWF_WIFI_STORE_ADDR, SECTOR_SIZE);
    sx_flash_lock();
    retain_flash_op_end();
}

static bool program_slot(uint32_t idx, const uint8_t *rec)
{
    uint8_t back[SLOT_SIZE] __attribute__((aligned(4)));

    retain_flash_op_begin();
    sx_flash_unlock();
    sx_flash_write(slot_addr(idx), rec, SLOT_SIZE);
    sx_flash_lock();
    retain_flash_op_end();

    sx_flash_read(slot_addr(idx), back, SLOT_SIZE);
    return memcmp(back, rec, SLOT_SIZE) == 0;
}

/* Build a record in `rec` (SLOT_SIZE bytes) and append it. Handles a full sector. */
static bool append(const scan_t *s, uint16_t state, const splc_wifi_config_t *cfg)
{
    uint8_t rec[SLOT_SIZE] __attribute__((aligned(16)));
    uint32_t idx;

    memset(rec, 0xFF, sizeof(rec));
    memset(rec, 0x00, CRC_SPAN);
    put32(rec, OFF_MAGIC, (uint32_t)REC_MAGIC);
    put32(rec, OFF_SEQ, s->found ? s->seq + 1U : 1U);
    put16(rec, OFF_STATE, state);
    if (cfg != NULL) {
        put16(rec, OFF_FLAGS, cfg->control_flags);
        memcpy(rec + OFF_SSID, cfg->ssid, sizeof(cfg->ssid));
        memcpy(rec + OFF_PASS, cfg->password, sizeof(cfg->password));
        put32(rec, OFF_IP, cfg->static_ip);
        put32(rec, OFF_SUBNET, cfg->static_subnet);
        put32(rec, OFF_GATEWAY, cfg->static_gateway);
        put32(rec, OFF_DNS, cfg->static_dns);
    }
    put32(rec, OFF_CRC, crc32_calc(rec, CRC_SPAN));

    if (s->has_blank) {
        idx = s->first_blank;
    } else {
        log_debug(TAG, "sector full, erasing it (the new record is written right after)");
        erase_sector();
        idx = 0U;
    }
    const bool ok = program_slot(idx, rec);
    if (!ok) {
        log_error(TAG, "slot %lu did not verify after programming", (unsigned long)idx);
    }
    return ok;
}

bool meshwf_wifi_store_load(splc_wifi_config_t *out)
{
    uint8_t rec[SLOT_SIZE] __attribute__((aligned(4)));
    scan_t s;

    if (out == NULL || !area_is_free()) {
        return false;
    }
    scan(&s, rec);
    if (!s.found || s.state != STATE_VALID) {
        log_debug(TAG, "load: nothing stored (found=%d)", (int)s.found);
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->control_flags = get16(rec, OFF_FLAGS);
    memcpy(out->ssid, rec + OFF_SSID, sizeof(out->ssid));
    memcpy(out->password, rec + OFF_PASS, sizeof(out->password));
    out->ssid[sizeof(out->ssid) - 1U] = '\0';
    out->password[sizeof(out->password) - 1U] = '\0';
    out->static_ip      = get32(rec, OFF_IP);
    out->static_subnet  = get32(rec, OFF_SUBNET);
    out->static_gateway = get32(rec, OFF_GATEWAY);
    out->static_dns     = get32(rec, OFF_DNS);

    if (out->ssid[0] == '\0') {
        return false;
    }
    log_debug(TAG, "load: ssid '%s' (slot %lu, seq %lu)", out->ssid,
              (unsigned long)s.idx, (unsigned long)s.seq);
    return true;
}

bool meshwf_wifi_store_save(const splc_wifi_config_t *cfg)
{
    uint8_t best[SLOT_SIZE] __attribute__((aligned(4)));
    scan_t s;

    if (cfg == NULL || cfg->ssid[0] == '\0' || !area_is_free()) {
        return false;
    }
    scan(&s, best);

    if (s.found && s.state == STATE_VALID) {
        splc_wifi_config_t cur;
        memset(&cur, 0, sizeof(cur));
        cur.control_flags = get16(best, OFF_FLAGS);
        memcpy(cur.ssid, best + OFF_SSID, sizeof(cur.ssid));
        memcpy(cur.password, best + OFF_PASS, sizeof(cur.password));
        cur.static_ip      = get32(best, OFF_IP);
        cur.static_subnet  = get32(best, OFF_SUBNET);
        cur.static_gateway = get32(best, OFF_GATEWAY);
        cur.static_dns     = get32(best, OFF_DNS);
        if (memcmp(&cur, cfg, sizeof(cur)) == 0) {
            log_debug(TAG, "save: unchanged, nothing written");
            return true;
        }
    }

    const bool ok = append(&s, STATE_VALID, cfg);
    log_debug(TAG, "save: ssid '%s' -> %s", cfg->ssid, ok ? "OK" : "FAILED");
    return ok;
}

void meshwf_wifi_store_erase(void)
{
    scan_t s;

    if (!area_is_free()) {
        return;
    }
    scan(&s, NULL);
    if (!s.found || s.state != STATE_VALID) {
        log_debug(TAG, "erase: nothing stored");
        return;
    }
    if (s.has_blank) {
        (void)append(&s, STATE_ERASED, NULL);
    } else {
        erase_sector();               /* full sector: a blank sector means "nothing stored" */
    }
    log_debug(TAG, "erase: credentials forgotten");
}

#endif /* SPLC_WIFI_ENABLE && SPLC_HOST_WIFI_PERSIST */