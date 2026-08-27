/*
 * fastconstmap — immutable map from strings to uint64
 *
 * Port of github.com/lemire/constmap (Go) to C. Lookups are one hash plus
 * three array reads and two XORs. The data structure is immutable after
 * construction.
 *
 * Apache License 2.0
 */
#ifndef FASTCONSTMAP_H
#define FASTCONSTMAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FCM_OK                    0
#define FCM_E_LENGTH_MISMATCH    -1
#define FCM_E_NOMEM              -2
#define FCM_E_DUPLICATE_KEY      -3
#define FCM_E_CONSTRUCT_FAIL     -4
#define FCM_E_INVALID_FORMAT     -5
#define FCM_E_CHECKSUM           -6
#define FCM_E_SHORT_BUFFER       -7
#define FCM_E_UNALIGNED          -8

#define FCM_NOT_FOUND ((uint64_t)0xFFFFFFFFFFFFFFFFULL)

typedef struct fcm_constmap {
    uint64_t  seed;
    uint32_t  segment_length;
    uint32_t  segment_length_mask;
    uint32_t  segment_count;
    uint32_t  segment_count_length;
    uint32_t  data_len;
    uint32_t  n;          /* original number of inserted keys */
    uint64_t *data;
} fcm_constmap_t;

typedef struct fcm_verified_constmap {
    uint64_t  seed;
    uint32_t  segment_length;
    uint32_t  segment_length_mask;
    uint32_t  segment_count;
    uint32_t  segment_count_length;
    uint32_t  data_len;
    uint32_t  n;          /* original number of inserted keys */
    uint64_t *data;
    uint64_t *checks;
} fcm_verified_constmap_t;

typedef struct {
    const char *bytes;
    size_t      len;
} fcm_key_t;

/* Construction. `out` is zero-initialised on success. On error, `out` is
 * untouched (no allocation leaks). */
int fcm_constmap_new(fcm_constmap_t *out,
                     const fcm_key_t *keys,
                     const uint64_t  *values,
                     size_t n);
int fcm_verified_constmap_new(fcm_verified_constmap_t *out,
                              const fcm_key_t *keys,
                              const uint64_t  *values,
                              size_t n);

void fcm_constmap_free(fcm_constmap_t *cm);
void fcm_verified_constmap_free(fcm_verified_constmap_t *vm);

uint64_t fcm_constmap_lookup(const fcm_constmap_t *cm,
                             const char *key, size_t key_len);
uint64_t fcm_verified_constmap_lookup(const fcm_verified_constmap_t *vm,
                                      const char *key, size_t key_len);

/* Batched lookup: `out[i]` receives the value for `keys[i]`, exactly as the
 * single-key lookup would return it (including FCM_NOT_FOUND for a key the
 * verified map does not hold).
 *
 * These are faster than a loop over the single-key form because they hash a
 * block of keys before gathering any values, so the array reads of a whole
 * block are in flight at once instead of each key's loads waiting behind the
 * hashing of the one before it. A lookup is memory-latency bound as soon as
 * the map outgrows the last-level cache, which is where the gain comes from.
 *
 * `out` must have room for `n` values. `keys` and `out` may not overlap. */
void fcm_constmap_lookup_many(const fcm_constmap_t *cm,
                              const fcm_key_t *keys, size_t n,
                              uint64_t *out);
void fcm_verified_constmap_lookup_many(const fcm_verified_constmap_t *vm,
                                       const fcm_key_t *keys, size_t n,
                                       uint64_t *out);

/* Serialisation to/from a memory buffer.
 *   *_serialized_size : exact byte count
 *   *_write           : write to `buf` (must have at least serialized_size bytes)
 *   *_read            : populate `out` from `buf`; on success caller must free */
size_t fcm_constmap_serialized_size(const fcm_constmap_t *cm);
int    fcm_constmap_write(const fcm_constmap_t *cm, void *buf);
int    fcm_constmap_read (fcm_constmap_t *out, const void *buf, size_t buf_len);

size_t fcm_verified_constmap_serialized_size(const fcm_verified_constmap_t *vm);
int    fcm_verified_constmap_write(const fcm_verified_constmap_t *vm, void *buf);
int    fcm_verified_constmap_read (fcm_verified_constmap_t *out, const void *buf, size_t buf_len);

/* Zero-copy views over a serialized buffer.
 *
 * Unlike `*_read`, these do NOT copy: `out->data` (and `out->checks`) point
 * directly into `buf`. This makes a map openable from shared memory with no
 * per-process copy.
 *
 * Requirements: a little-endian host and a buffer whose start is 8-byte
 * aligned (so the embedded uint64 arrays are aligned). The serialized
 * format pads its header to a multiple of 8 to guarantee this.
 *
 * Returns FCM_OK, or FCM_E_UNALIGNED / FCM_E_INVALID_FORMAT (e.g. on a
 * big-endian host) / FCM_E_CHECKSUM / FCM_E_SHORT_BUFFER.
 *
 * The caller MUST keep `buf` alive for the lifetime of `out`, and MUST NOT
 * call `fcm_*_free` on a view (the data is borrowed, not owned). */
int fcm_constmap_view(fcm_constmap_t *out, const void *buf, size_t buf_len);
int fcm_verified_constmap_view(fcm_verified_constmap_t *out, const void *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* FASTCONSTMAP_H */
