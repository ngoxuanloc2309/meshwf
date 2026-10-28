#ifndef ST67_CERT_STORE_H
#define ST67_CERT_STORE_H

/*
 * st67_cert_store.h - persistent store for the MQTT TLS certificates (decision 64).
 *
 * ONE 8 KB Flash sector holds the three PEM certificates the module needs:
 *   1 Root CA, 2 client certificate, 3 client private key.
 * The sector sits directly below SimplePLC's data region (SPLC_FLASH_DATA_BASE_ADDR - 8 KB)
 * and is NOT touched by the Rule Table / Retain code.
 *
 * Layout: an append-only log of records, each a multiple of 16 B (STM32H5 programs 16 B
 * quad-words, each quad-word only ONCE between two erases):
 *     header  16 B   magic, seq, len, type, header CRC          (written first)
 *     payload        PEM bytes + one NUL, padded with 0xFF to 16 B
 *     trailer 16 B   magic, CRC-16/MODBUS of payload (+NUL)     (written LAST)
 * A record is valid only when its trailer is present and the CRC matches. For every type the
 * valid record with the highest seq is the live one, so a transfer that stops half way
 * (cable pulled, 30 s timeout, power loss) leaves the previous certificate untouched.
 * The log is never erased while there is room; only when a new certificate does not fit the
 * tail is the sector compacted (the live records of the OTHER types are copied to RAM, the
 * sector is erased, they are written back). That path is not power-fail safe for the types
 * that are rewritten, and it drops the old copy of the type being replaced.
 *
 * Capacity: the three live PEMs (+ 48 B of framing each, + NUL, rounded to 16 B) must fit in
 * 8 KB. A begin() that cannot fit returns ST67_CERT_ERR_OVERFLOW and changes nothing.
 *
 * Context rules
 *   - begin/chunk/commit/abort/clear/service: ONE task only (the SimplePLC engine task):
 *     they program/erase Flash and are wrapped in retain_flash_op_begin/end().
 *   - get(): any task, but ONLY between acquire() and release(). While a reader holds the
 *     store, anything that would erase the sector (compaction, clear) is refused (BUSY) or,
 *     for clear, postponed to service().
 *   - The pointer returned by get() is a memory-mapped Flash address, NUL terminated: it can
 *     go straight into W6X_MQTT_Connect_t::*Content with no RAM copy.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ST67_CERT_ROOT_CA        1U
#define ST67_CERT_CLIENT_CERT    2U
#define ST67_CERT_CLIENT_KEY     3U

/* Largest single certificate the wire protocol allows (32 chunks x 120 B). */
#define ST67_CERT_MAX_SIZE       3840U

/* Result codes. */
#define ST67_CERT_OK             0
#define ST67_CERT_ERR_OVERFLOW   2    /* does not fit the sector (same value as PLC_MQTT_TLS_ST_OVERFLOW) */
#define ST67_CERT_ERR_STORAGE    3    /* Flash/RAM failure */
#define ST67_CERT_ERR_BUSY       4    /* a reader holds the store: retry later */
#define ST67_CERT_ERR_STATE      5    /* call out of sequence / bad argument */

#ifdef __cplusplus
extern "C" {
#endif

/* Scan the sector. Called lazily by every function below; calling it again is harmless. */
void st67_cert_store_init(void);

/* A new certificate of `type` starts: `total_size` PEM bytes (without NUL). */
int  st67_cert_store_begin(uint8_t type, uint16_t total_size);
/* The next chunk, in order (index starts at 0). */
int  st67_cert_store_chunk(uint8_t type, uint8_t index, const uint8_t *data, uint8_t len);
/* All bytes received: make the certificate live. On failure the old one stays live. */
int  st67_cert_store_commit(uint8_t type);
/* Abandon the open transfer (no effect on the live certificates). */
void st67_cert_store_abort(void);
/* Erase every certificate (FACTORY_RESET). Postponed to service() while a reader holds it. */
void st67_cert_store_clear(void);
/* Engine task, every loop: runs a postponed clear. */
void st67_cert_store_service(void);

/* bit0 Root CA, bit1 client cert, bit2 client key: which certificates are live. */
uint16_t st67_cert_store_mask(void);
/* Incremented by every commit and clear: a changed value means a new TLS configuration. */
uint32_t st67_cert_store_generation(void);

/* Reader side (ST67 task). acquire() fails while the sector is being erased. */
bool st67_cert_store_acquire(void);
void st67_cert_store_release(void);
/* Valid only between acquire() and release(). NULL when `type` is not stored.
 * *len (optional) = PEM length without the NUL. */
const char *st67_cert_store_get(uint8_t type, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* ST67_CERT_STORE_H */