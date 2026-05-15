/*
 * fastconstmap — C implementation
 * Port of github.com/lemire/constmap (Go).
 *
 * Apache License 2.0
 */
#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include "third_party/xxhash/xxhash.h"

#include "constmap.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#  include <intrin.h>
#endif

/* ------------------------------------------------------------------------- */
/* Hashing                                                                   */
/* ------------------------------------------------------------------------- */

static inline uint64_t fcm_murmur64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static inline uint64_t fcm_mixsplit(uint64_t key, uint64_t seed) {
    return fcm_murmur64(key + seed);
}

static inline uint64_t fcm_splitmix64(uint64_t *state) {
    *state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = *state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint64_t fcm_hash_key(const char *key, size_t len) {
    return (uint64_t)XXH3_64bits(key, len);
}

/* (hash * N) >> 64, where N fits in uint32. */
static inline uint32_t fcm_mul_high32(uint64_t hash, uint32_t n) {
#if defined(__SIZEOF_INT128__)
    return (uint32_t)((__uint128_t)hash * (uint64_t)n >> 64);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    uint64_t hi;
    (void)_umul128(hash, (uint64_t)n, &hi);
    return (uint32_t)hi;
#else
    uint64_t hash_hi = hash >> 32;
    uint64_t hash_lo = hash & 0xFFFFFFFFULL;
    uint64_t prod_hi = hash_hi * (uint64_t)n;
    uint64_t prod_lo = hash_lo * (uint64_t)n;
    return (uint32_t)((prod_hi + (prod_lo >> 32)) >> 32);
#endif
}

/* ------------------------------------------------------------------------- */
/* Parameters                                                                */
/* ------------------------------------------------------------------------- */

static uint32_t fcm_calculate_segment_length(uint32_t size) {
    if (size == 0) return 4;
    return (uint32_t)1 << (int)floor(log((double)size) / log(3.33) + 2.25);
}

static double fcm_calculate_size_factor(uint32_t size) {
    double a = 0.875 + 0.25 * log(1000000.0) / log((double)size);
    return a > 1.125 ? a : 1.125;
}

/* Returns three positions h0, h1, h2 derived from a mixed hash. */
static inline void fcm_get_h012(uint64_t hash, uint32_t segment_length,
                                uint32_t segment_length_mask,
                                uint32_t segment_count_length,
                                uint32_t *h0, uint32_t *h1, uint32_t *h2) {
    *h0 = fcm_mul_high32(hash, segment_count_length);
    *h1 = *h0 + segment_length;
    *h2 = *h1 + segment_length;
    *h1 ^= (uint32_t)(hash >> 18) & segment_length_mask;
    *h2 ^= (uint32_t)hash & segment_length_mask;
}

/* ------------------------------------------------------------------------- */
/* (hash → value) lookup: sorted array + binary search                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint64_t hash;
    uint64_t value;
} fcm_pair_t;

static int fcm_pair_cmp(const void *a, const void *b) {
    uint64_t ha = ((const fcm_pair_t *)a)->hash;
    uint64_t hb = ((const fcm_pair_t *)b)->hash;
    if (ha < hb) return -1;
    if (ha > hb) return  1;
    return 0;
}

static inline uint64_t fcm_pair_lookup(const fcm_pair_t *pairs, size_t n, uint64_t h) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (pairs[mid].hash < h) lo = mid + 1;
        else                     hi = mid;
    }
    return pairs[lo].value;
}

/* ------------------------------------------------------------------------- */
/* Shared parameter initialisation                                           */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint64_t seed;
    uint32_t segment_length;
    uint32_t segment_length_mask;
    uint32_t segment_count;
    uint32_t segment_count_length;
    uint32_t array_len;
} fcm_params_t;

static void fcm_init_params(fcm_params_t *p, uint32_t size) {
    p->segment_length = fcm_calculate_segment_length(size);
    if (p->segment_length > 262144u) p->segment_length = 262144u;
    p->segment_length_mask = p->segment_length - 1;

    uint32_t capacity = 0;
    if (size > 1) {
        double sf = fcm_calculate_size_factor(size);
        capacity = (uint32_t)round((double)size * sf);
    }
    uint32_t total_segment_count =
        (capacity + p->segment_length - 1) / p->segment_length;
    if (total_segment_count < 3) total_segment_count = 3;
    p->segment_count        = total_segment_count - 2;
    p->segment_count_length = p->segment_count * p->segment_length;
    p->array_len            = total_segment_count * p->segment_length;
}

/* ------------------------------------------------------------------------- */
/* Construction (peeling) — shared between ConstMap and VerifiedConstMap     */
/* ------------------------------------------------------------------------- */

#define FCM_MAX_ITERATIONS 100

/*
 * Runs the binary-fuse-filter construction. On success returns 0, fills in
 * *out_params, and the caller-provided reverse_order (size+1 uint64s) and
 * reverse_h (size uint8s) contain the peeling stack. On error returns
 * FCM_E_DUPLICATE_KEY or FCM_E_CONSTRUCT_FAIL.
 *
 * The temporary buffers (alone, t2count, t2hash, start_pos) are owned by
 * the caller and must be sized for the worst case (array_len of the
 * initial parameters; start_pos sized 1 << ceil(log2(segment_count)) is
 * not predictable, so we size it from the current segment_count each
 * iteration -- but to avoid reallocation we use a generous upper bound).
 */
static int fcm_peel(const uint64_t *hashed,
                    uint32_t size,
                    fcm_params_t *params,
                    uint32_t *alone,
                    uint8_t  *t2count,
                    uint64_t *t2hash,
                    uint8_t  *reverse_h,
                    uint64_t *reverse_order /* size+1 */) {
    uint64_t rng_counter = 1;
    params->seed = fcm_splitmix64(&rng_counter);
    reverse_order[size] = 1;

    /* start_pos sizing: 1 << blockBits where blockBits = ceil(log2(segment_count)).
     * segment_count never exceeds the original total_segment_count which is at
     * most ceil(size * 1.23 / segment_length). We allocate once up to a safe
     * cap of 1024 entries; if needed we grow. */
    uint32_t *start_pos = NULL;
    uint32_t  start_pos_cap = 0;

    for (int iteration = 0; ; iteration++) {
        if (iteration > FCM_MAX_ITERATIONS) {
            free(start_pos);
            return FCM_E_CONSTRUCT_FAIL;
        }

        if (size > 4 && size < 1000000) {
            switch (iteration % 4) {
            case 2:
                params->segment_length      /= 2;
                params->segment_length_mask  = params->segment_length - 1;
                params->segment_count        = params->segment_count * 2 + 2;
                params->segment_count_length = params->segment_count * params->segment_length;
                break;
            case 3:
                params->segment_length      *= 2;
                params->segment_length_mask  = params->segment_length - 1;
                params->segment_count        = params->segment_count / 2 - 1;
                params->segment_count_length = params->segment_count * params->segment_length;
                break;
            default: break;
            }
        }

        int block_bits = 1;
        while ((1u << block_bits) < params->segment_count) block_bits++;
        uint32_t block_count = 1u << block_bits;

        if (block_count > start_pos_cap) {
            uint32_t *np = (uint32_t *)realloc(start_pos, block_count * sizeof(uint32_t));
            if (!np) { free(start_pos); return FCM_E_NOMEM; }
            start_pos     = np;
            start_pos_cap = block_count;
        }

        for (uint32_t i = 0; i < block_count; i++) {
            start_pos[i] = (uint32_t)(((uint64_t)i * (uint64_t)size) >> block_bits);
        }
        for (uint32_t i = 0; i < size; i++) {
            uint64_t hash = fcm_mixsplit(hashed[i], params->seed);
            uint64_t seg_index = hash >> (64 - block_bits);
            while (reverse_order[start_pos[seg_index]] != 0) {
                seg_index++;
                seg_index &= block_count - 1;
            }
            reverse_order[start_pos[seg_index]] = hash;
            start_pos[seg_index]++;
        }

        int has_error = 0;
        int duplicate = 0;
        for (uint32_t i = 0; i < size; i++) {
            uint64_t hash = reverse_order[i];
            uint32_t i1, i2, i3;
            fcm_get_h012(hash, params->segment_length, params->segment_length_mask,
                         params->segment_count_length, &i1, &i2, &i3);
            t2count[i1] += 4;
            t2hash[i1] ^= hash;
            t2count[i2] += 4;
            t2count[i2] ^= 1;
            t2hash[i2] ^= hash;
            t2count[i3] += 4;
            t2count[i3] ^= 2;
            t2hash[i3] ^= hash;

            if ((t2hash[i1] & t2hash[i2] & t2hash[i3]) == 0) {
                if ((t2hash[i1] == 0 && t2count[i1] == 8) ||
                    (t2hash[i2] == 0 && t2count[i2] == 8) ||
                    (t2hash[i3] == 0 && t2count[i3] == 8)) {
                    duplicate = 1;
                    break;
                }
            }
            if (t2count[i1] < 4 || t2count[i2] < 4 || t2count[i3] < 4) {
                has_error = 1;
            }
        }
        if (duplicate) {
            free(start_pos);
            return FCM_E_DUPLICATE_KEY;
        }
        if (has_error) {
            for (uint32_t i = 0; i < size; i++) reverse_order[i] = 0;
            for (uint32_t i = 0; i < params->array_len; i++) {
                t2count[i] = 0;
                t2hash[i]  = 0;
            }
            params->seed = fcm_splitmix64(&rng_counter);
            continue;
        }

        /* Peeling */
        uint32_t qsize = 0;
        for (uint32_t i = 0; i < params->array_len; i++) {
            alone[qsize] = i;
            if ((t2count[i] >> 2) == 1) qsize++;
        }

        uint32_t stacksize = 0;
        uint32_t seg_len   = params->segment_length;
        uint32_t seg_len_minus2 = seg_len ^ (uint32_t)(-(uint32_t)(2 * seg_len));

        while (qsize > 0) {
            qsize--;
            uint32_t idx = alone[qsize];
            if ((t2count[idx] >> 2) != 1) continue;

            uint64_t hash  = t2hash[idx];
            uint8_t  found = t2count[idx] & 3;
            reverse_h[stacksize]     = found;
            reverse_order[stacksize] = hash;
            stacksize++;

            uint32_t h01 = (uint32_t)(hash >> 18) & params->segment_length_mask;
            uint32_t h02 = (uint32_t)hash         & params->segment_length_mask;

            uint32_t is0 = (uint32_t)(-(int32_t)((uint8_t)(found - 1) >> 7));
            uint32_t is1 = (uint32_t)(-(int32_t)(found & 1));
            uint32_t is2 = (uint32_t)(-(int32_t)(found >> 1));

            uint32_t other1 = idx + (seg_len ^ (seg_len_minus2 & is2));
            uint32_t other2 = idx - (seg_len ^ (seg_len_minus2 & is0));

            other1 ^= (h01 & ~is2) ^ (h02 & ~is0);
            other2 ^= (h01 & ~is0) ^ (h02 & ~is1);

            uint8_t f1 = (uint8_t)((is0 & 1u) | (is1 & 2u));
            uint8_t f2 = (uint8_t)((is0 & 2u) | (is2 & 1u));

            alone[qsize] = other1;
            if ((t2count[other1] >> 2) == 2) qsize++;
            t2count[other1] -= 4;
            t2count[other1] ^= f1;
            t2hash[other1]  ^= hash;

            alone[qsize] = other2;
            if ((t2count[other2] >> 2) == 2) qsize++;
            t2count[other2] -= 4;
            t2count[other2] ^= f2;
            t2hash[other2]  ^= hash;
        }

        if (stacksize == size) {
            free(start_pos);
            return FCM_OK;
        }

        /* Reset and retry. */
        for (uint32_t i = 0; i < size; i++) reverse_order[i] = 0;
        for (uint32_t i = 0; i < params->array_len; i++) {
            t2count[i] = 0;
            t2hash[i]  = 0;
        }
        params->seed = fcm_splitmix64(&rng_counter);
    }
}

/* ------------------------------------------------------------------------- */
/* Construction front-ends                                                   */
/* ------------------------------------------------------------------------- */

static inline uint64_t fcm_fingerprint(uint64_t hash) {
    return hash ^ (hash >> 32);
}

int fcm_constmap_new(fcm_constmap_t *out,
                     const fcm_key_t *keys,
                     const uint64_t  *values,
                     size_t n) {
    if (!out) return FCM_E_LENGTH_MISMATCH;
    memset(out, 0, sizeof(*out));
    if (n == 0) return FCM_OK;
    if (n > 0xFFFFFFFFu) return FCM_E_LENGTH_MISMATCH;

    uint32_t size = (uint32_t)n;
    int rc = FCM_OK;

    fcm_params_t params;
    fcm_init_params(&params, size);

    /* Allocate everything up front. */
    uint64_t *hashed        = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *data          = (uint64_t *)calloc(params.array_len, sizeof(uint64_t));
    uint32_t *alone         = (uint32_t *)malloc((size_t)params.array_len * sizeof(uint32_t));
    uint8_t  *t2count       = (uint8_t  *)calloc(params.array_len, sizeof(uint8_t));
    uint64_t *t2hash        = (uint64_t *)calloc(params.array_len, sizeof(uint64_t));
    uint8_t  *reverse_h     = (uint8_t  *)malloc((size_t)size);
    uint64_t *reverse_order = (uint64_t *)calloc((size_t)size + 1, sizeof(uint64_t));
    fcm_pair_t *pairs       = (fcm_pair_t *)malloc((size_t)n * sizeof(fcm_pair_t));

    if (!hashed || !data || !alone || !t2count || !t2hash ||
        !reverse_h || !reverse_order || !pairs) {
        rc = FCM_E_NOMEM;
        goto cleanup;
    }

    for (size_t i = 0; i < n; i++) {
        hashed[i] = fcm_hash_key(keys[i].bytes, keys[i].len);
    }

    rc = fcm_peel(hashed, size, &params, alone, t2count, t2hash,
                  reverse_h, reverse_order);
    if (rc != FCM_OK) goto cleanup;

    for (size_t i = 0; i < n; i++) {
        pairs[i].hash  = fcm_mixsplit(hashed[i], params.seed);
        pairs[i].value = values[i];
    }
    qsort(pairs, n, sizeof(fcm_pair_t), fcm_pair_cmp);

    /* Assignment phase. */
    uint32_t h012[5];
    for (int32_t i = (int32_t)size - 1; i >= 0; i--) {
        uint64_t hash = reverse_order[i];
        uint64_t val  = fcm_pair_lookup(pairs, n, hash);
        uint32_t i1, i2, i3;
        fcm_get_h012(hash, params.segment_length, params.segment_length_mask,
                     params.segment_count_length, &i1, &i2, &i3);
        uint8_t found = reverse_h[i];
        h012[0] = i1; h012[1] = i2; h012[2] = i3;
        h012[3] = h012[0]; h012[4] = h012[1];
        data[h012[found]] = val ^ data[h012[found + 1]] ^ data[h012[found + 2]];
    }

    out->seed                 = params.seed;
    out->segment_length       = params.segment_length;
    out->segment_length_mask  = params.segment_length_mask;
    out->segment_count        = params.segment_count;
    out->segment_count_length = params.segment_count_length;
    out->data_len             = params.array_len;
    out->data                 = data;
    data = NULL;  /* ownership transferred */

cleanup:
    free(hashed);
    free(data);
    free(alone);
    free(t2count);
    free(t2hash);
    free(reverse_h);
    free(reverse_order);
    free(pairs);
    return rc;
}

int fcm_verified_constmap_new(fcm_verified_constmap_t *out,
                              const fcm_key_t *keys,
                              const uint64_t  *values,
                              size_t n) {
    if (!out) return FCM_E_LENGTH_MISMATCH;
    memset(out, 0, sizeof(*out));
    if (n == 0) return FCM_OK;
    if (n > 0xFFFFFFFFu) return FCM_E_LENGTH_MISMATCH;

    uint32_t size = (uint32_t)n;
    int rc = FCM_OK;

    fcm_params_t params;
    fcm_init_params(&params, size);

    uint64_t *hashed        = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    uint64_t *data          = (uint64_t *)calloc(params.array_len, sizeof(uint64_t));
    uint64_t *checks        = (uint64_t *)calloc(params.array_len, sizeof(uint64_t));
    uint32_t *alone         = (uint32_t *)malloc((size_t)params.array_len * sizeof(uint32_t));
    uint8_t  *t2count       = (uint8_t  *)calloc(params.array_len, sizeof(uint8_t));
    uint64_t *t2hash        = (uint64_t *)calloc(params.array_len, sizeof(uint64_t));
    uint8_t  *reverse_h     = (uint8_t  *)malloc((size_t)size);
    uint64_t *reverse_order = (uint64_t *)calloc((size_t)size + 1, sizeof(uint64_t));
    fcm_pair_t *pairs       = (fcm_pair_t *)malloc((size_t)n * sizeof(fcm_pair_t));

    if (!hashed || !data || !checks || !alone || !t2count || !t2hash ||
        !reverse_h || !reverse_order || !pairs) {
        rc = FCM_E_NOMEM;
        goto cleanup;
    }

    for (size_t i = 0; i < n; i++) {
        hashed[i] = fcm_hash_key(keys[i].bytes, keys[i].len);
    }

    rc = fcm_peel(hashed, size, &params, alone, t2count, t2hash,
                  reverse_h, reverse_order);
    if (rc != FCM_OK) goto cleanup;

    for (size_t i = 0; i < n; i++) {
        pairs[i].hash  = fcm_mixsplit(hashed[i], params.seed);
        pairs[i].value = values[i];
    }
    qsort(pairs, n, sizeof(fcm_pair_t), fcm_pair_cmp);

    uint32_t h012[5];
    for (int32_t i = (int32_t)size - 1; i >= 0; i--) {
        uint64_t hash = reverse_order[i];
        uint64_t val  = fcm_pair_lookup(pairs, n, hash);
        uint64_t fp   = fcm_fingerprint(hash);
        uint32_t i1, i2, i3;
        fcm_get_h012(hash, params.segment_length, params.segment_length_mask,
                     params.segment_count_length, &i1, &i2, &i3);
        uint8_t found = reverse_h[i];
        h012[0] = i1; h012[1] = i2; h012[2] = i3;
        h012[3] = h012[0]; h012[4] = h012[1];
        data[h012[found]]   = val ^ data[h012[found + 1]]   ^ data[h012[found + 2]];
        checks[h012[found]] = fp  ^ checks[h012[found + 1]] ^ checks[h012[found + 2]];
    }

    out->seed                 = params.seed;
    out->segment_length       = params.segment_length;
    out->segment_length_mask  = params.segment_length_mask;
    out->segment_count        = params.segment_count;
    out->segment_count_length = params.segment_count_length;
    out->data_len             = params.array_len;
    out->data                 = data;
    out->checks               = checks;
    data   = NULL;
    checks = NULL;

cleanup:
    free(hashed);
    free(data);
    free(checks);
    free(alone);
    free(t2count);
    free(t2hash);
    free(reverse_h);
    free(reverse_order);
    free(pairs);
    return rc;
}

void fcm_constmap_free(fcm_constmap_t *cm) {
    if (!cm) return;
    free(cm->data);
    memset(cm, 0, sizeof(*cm));
}

void fcm_verified_constmap_free(fcm_verified_constmap_t *vm) {
    if (!vm) return;
    free(vm->data);
    free(vm->checks);
    memset(vm, 0, sizeof(*vm));
}

/* ------------------------------------------------------------------------- */
/* Lookup                                                                    */
/* ------------------------------------------------------------------------- */

uint64_t fcm_constmap_lookup(const fcm_constmap_t *cm,
                             const char *key, size_t key_len) {
    if (cm->data_len == 0) return 0;
    uint64_t hash = fcm_mixsplit(fcm_hash_key(key, key_len), cm->seed);
    uint32_t h0, h1, h2;
    fcm_get_h012(hash, cm->segment_length, cm->segment_length_mask,
                 cm->segment_count_length, &h0, &h1, &h2);
    return cm->data[h0] ^ cm->data[h1] ^ cm->data[h2];
}

uint64_t fcm_verified_constmap_lookup(const fcm_verified_constmap_t *vm,
                                      const char *key, size_t key_len) {
    if (vm->data_len == 0) return FCM_NOT_FOUND;
    uint64_t hash = fcm_mixsplit(fcm_hash_key(key, key_len), vm->seed);
    uint32_t h0, h1, h2;
    fcm_get_h012(hash, vm->segment_length, vm->segment_length_mask,
                 vm->segment_count_length, &h0, &h1, &h2);
    uint64_t fp = vm->checks[h0] ^ vm->checks[h1] ^ vm->checks[h2];
    if (fp != fcm_fingerprint(hash)) return FCM_NOT_FOUND;
    return vm->data[h0] ^ vm->data[h1] ^ vm->data[h2];
}

/* ------------------------------------------------------------------------- */
/* Serialisation                                                             */
/*                                                                           */
/* Binary format (little-endian):                                            */
/*   [8] magic                                                               */
/*   [8] seed                                                                */
/*   [4] segment_length                                                      */
/*   [4] segment_count                                                       */
/*   [4] data_len                                                            */
/*   [4] reserved (zero) — pads the header to 32 bytes so the data array      */
/*       starts 8-byte aligned, enabling zero-copy uint64 views               */
/*   [8 * data_len] data                                                     */
/*   (verified only) [8 * data_len] checks                                   */
/*   [8] FNV-1a-64 checksum of all preceding bytes                           */
/* ------------------------------------------------------------------------- */

static const uint8_t fcm_magic_constmap[8]          = {'C','M','A','P','0','0','0','2'};
static const uint8_t fcm_magic_verified_constmap[8] = {'V','C','M','P','0','0','0','2'};

#define FCM_HEADER_SIZE 32u  /* 8 magic + 8 seed + 4 seglen + 4 segcount + 4 datalen + 4 pad */
#define FCM_TRAILER_SIZE 8u  /* checksum */

static inline void fcm_write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v      );
    p[1] = (uint8_t)(v >>  8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline void fcm_write_u64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v      );
    p[1] = (uint8_t)(v >>  8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
}
static inline uint32_t fcm_read_u32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}
static inline uint64_t fcm_read_u64(const uint8_t *p) {
    return (uint64_t)p[0]
         | ((uint64_t)p[1] <<  8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

/* FNV-1a 64-bit, matching Go's hash/fnv. */
static uint64_t fcm_fnv1a64(const uint8_t *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

size_t fcm_constmap_serialized_size(const fcm_constmap_t *cm) {
    return FCM_HEADER_SIZE + (size_t)cm->data_len * 8u + FCM_TRAILER_SIZE;
}

int fcm_constmap_write(const fcm_constmap_t *cm, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    uint8_t *start = p;
    memcpy(p, fcm_magic_constmap, 8); p += 8;
    fcm_write_u64(p, cm->seed); p += 8;
    fcm_write_u32(p, cm->segment_length); p += 4;
    fcm_write_u32(p, cm->segment_count);  p += 4;
    fcm_write_u32(p, cm->data_len);       p += 4;
    fcm_write_u32(p, 0);                  p += 4;  /* reserved padding */
    for (uint32_t i = 0; i < cm->data_len; i++) {
        fcm_write_u64(p, cm->data[i]); p += 8;
    }
    uint64_t sum = fcm_fnv1a64(start, (size_t)(p - start));
    fcm_write_u64(p, sum);
    return FCM_OK;
}

int fcm_constmap_read(fcm_constmap_t *out, const void *buf, size_t buf_len) {
    if (!out) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    if (buf_len < FCM_HEADER_SIZE + FCM_TRAILER_SIZE) return FCM_E_SHORT_BUFFER;

    const uint8_t *p = (const uint8_t *)buf;
    if (memcmp(p, fcm_magic_constmap, 8) != 0) return FCM_E_INVALID_FORMAT;

    uint64_t seed            = fcm_read_u64(p + 8);
    uint32_t segment_length  = fcm_read_u32(p + 16);
    uint32_t segment_count   = fcm_read_u32(p + 20);
    uint32_t data_len        = fcm_read_u32(p + 24);

    size_t expected = FCM_HEADER_SIZE + (size_t)data_len * 8u + FCM_TRAILER_SIZE;
    if (buf_len < expected) return FCM_E_SHORT_BUFFER;

    uint64_t got_sum      = fcm_read_u64(p + expected - 8);
    uint64_t expected_sum = fcm_fnv1a64(p, expected - 8);
    if (got_sum != expected_sum) return FCM_E_CHECKSUM;

    uint64_t *data = NULL;
    if (data_len > 0) {
        data = (uint64_t *)malloc((size_t)data_len * sizeof(uint64_t));
        if (!data) return FCM_E_NOMEM;
        const uint8_t *dp = p + FCM_HEADER_SIZE;
        for (uint32_t i = 0; i < data_len; i++) {
            data[i] = fcm_read_u64(dp + (size_t)i * 8);
        }
    }
    out->seed                 = seed;
    out->segment_length       = segment_length;
    out->segment_length_mask  = segment_length ? segment_length - 1 : 0;
    out->segment_count        = segment_count;
    out->segment_count_length = segment_count * segment_length;
    out->data_len             = data_len;
    out->data                 = data;
    return FCM_OK;
}

size_t fcm_verified_constmap_serialized_size(const fcm_verified_constmap_t *vm) {
    return FCM_HEADER_SIZE + (size_t)vm->data_len * 16u + FCM_TRAILER_SIZE;
}

int fcm_verified_constmap_write(const fcm_verified_constmap_t *vm, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    uint8_t *start = p;
    memcpy(p, fcm_magic_verified_constmap, 8); p += 8;
    fcm_write_u64(p, vm->seed); p += 8;
    fcm_write_u32(p, vm->segment_length); p += 4;
    fcm_write_u32(p, vm->segment_count);  p += 4;
    fcm_write_u32(p, vm->data_len);       p += 4;
    fcm_write_u32(p, 0);                  p += 4;  /* reserved padding */
    for (uint32_t i = 0; i < vm->data_len; i++) {
        fcm_write_u64(p, vm->data[i]); p += 8;
    }
    for (uint32_t i = 0; i < vm->data_len; i++) {
        fcm_write_u64(p, vm->checks[i]); p += 8;
    }
    uint64_t sum = fcm_fnv1a64(start, (size_t)(p - start));
    fcm_write_u64(p, sum);
    return FCM_OK;
}

int fcm_verified_constmap_read(fcm_verified_constmap_t *out, const void *buf, size_t buf_len) {
    if (!out) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    if (buf_len < FCM_HEADER_SIZE + FCM_TRAILER_SIZE) return FCM_E_SHORT_BUFFER;

    const uint8_t *p = (const uint8_t *)buf;
    if (memcmp(p, fcm_magic_verified_constmap, 8) != 0) return FCM_E_INVALID_FORMAT;

    uint64_t seed            = fcm_read_u64(p + 8);
    uint32_t segment_length  = fcm_read_u32(p + 16);
    uint32_t segment_count   = fcm_read_u32(p + 20);
    uint32_t data_len        = fcm_read_u32(p + 24);

    size_t expected = FCM_HEADER_SIZE + (size_t)data_len * 16u + FCM_TRAILER_SIZE;
    if (buf_len < expected) return FCM_E_SHORT_BUFFER;

    uint64_t got_sum      = fcm_read_u64(p + expected - 8);
    uint64_t expected_sum = fcm_fnv1a64(p, expected - 8);
    if (got_sum != expected_sum) return FCM_E_CHECKSUM;

    uint64_t *data   = NULL;
    uint64_t *checks = NULL;
    if (data_len > 0) {
        data   = (uint64_t *)malloc((size_t)data_len * sizeof(uint64_t));
        checks = (uint64_t *)malloc((size_t)data_len * sizeof(uint64_t));
        if (!data || !checks) { free(data); free(checks); return FCM_E_NOMEM; }
        const uint8_t *dp = p + FCM_HEADER_SIZE;
        for (uint32_t i = 0; i < data_len; i++) {
            data[i] = fcm_read_u64(dp + (size_t)i * 8);
        }
        const uint8_t *cp = dp + (size_t)data_len * 8;
        for (uint32_t i = 0; i < data_len; i++) {
            checks[i] = fcm_read_u64(cp + (size_t)i * 8);
        }
    }
    out->seed                 = seed;
    out->segment_length       = segment_length;
    out->segment_length_mask  = segment_length ? segment_length - 1 : 0;
    out->segment_count        = segment_count;
    out->segment_count_length = segment_count * segment_length;
    out->data_len             = data_len;
    out->data                 = data;
    out->checks               = checks;
    return FCM_OK;
}

/* ------------------------------------------------------------------------- */
/* Zero-copy views                                                           */
/* ------------------------------------------------------------------------- */

static inline int fcm_host_is_little_endian(void) {
    const uint16_t x = 1;
    return *(const uint8_t *)&x == 1;
}

int fcm_constmap_view(fcm_constmap_t *out, const void *buf, size_t buf_len) {
    if (!out) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    if (!fcm_host_is_little_endian()) return FCM_E_INVALID_FORMAT;
    if (buf_len < FCM_HEADER_SIZE + FCM_TRAILER_SIZE) return FCM_E_SHORT_BUFFER;

    const uint8_t *p = (const uint8_t *)buf;
    if (memcmp(p, fcm_magic_constmap, 8) != 0) return FCM_E_INVALID_FORMAT;

    uint32_t data_len = fcm_read_u32(p + 24);
    size_t expected = FCM_HEADER_SIZE + (size_t)data_len * 8u + FCM_TRAILER_SIZE;
    if (buf_len < expected) return FCM_E_SHORT_BUFFER;

    uint64_t got_sum      = fcm_read_u64(p + expected - 8);
    uint64_t expected_sum = fcm_fnv1a64(p, expected - 8);
    if (got_sum != expected_sum) return FCM_E_CHECKSUM;

    const uint8_t *dp = p + FCM_HEADER_SIZE;
    if (((uintptr_t)dp & 7u) != 0) return FCM_E_UNALIGNED;

    uint32_t segment_length   = fcm_read_u32(p + 16);
    out->seed                 = fcm_read_u64(p + 8);
    out->segment_length       = segment_length;
    out->segment_length_mask  = segment_length ? segment_length - 1 : 0;
    out->segment_count        = fcm_read_u32(p + 20);
    out->segment_count_length = out->segment_count * segment_length;
    out->data_len             = data_len;
    /* Borrowed pointer into `buf`; the integer round-trip launders away the
     * source const-ness. Lookups only read this array. */
    out->data = (uint64_t *)(uintptr_t)dp;
    return FCM_OK;
}

int fcm_verified_constmap_view(fcm_verified_constmap_t *out, const void *buf, size_t buf_len) {
    if (!out) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    if (!fcm_host_is_little_endian()) return FCM_E_INVALID_FORMAT;
    if (buf_len < FCM_HEADER_SIZE + FCM_TRAILER_SIZE) return FCM_E_SHORT_BUFFER;

    const uint8_t *p = (const uint8_t *)buf;
    if (memcmp(p, fcm_magic_verified_constmap, 8) != 0) return FCM_E_INVALID_FORMAT;

    uint32_t data_len = fcm_read_u32(p + 24);
    size_t expected = FCM_HEADER_SIZE + (size_t)data_len * 16u + FCM_TRAILER_SIZE;
    if (buf_len < expected) return FCM_E_SHORT_BUFFER;

    uint64_t got_sum      = fcm_read_u64(p + expected - 8);
    uint64_t expected_sum = fcm_fnv1a64(p, expected - 8);
    if (got_sum != expected_sum) return FCM_E_CHECKSUM;

    const uint8_t *dp = p + FCM_HEADER_SIZE;
    const uint8_t *cp = dp + (size_t)data_len * 8u;
    if (((uintptr_t)dp & 7u) != 0) return FCM_E_UNALIGNED;

    uint32_t segment_length   = fcm_read_u32(p + 16);
    out->seed                 = fcm_read_u64(p + 8);
    out->segment_length       = segment_length;
    out->segment_length_mask  = segment_length ? segment_length - 1 : 0;
    out->segment_count        = fcm_read_u32(p + 20);
    out->segment_count_length = out->segment_count * segment_length;
    out->data_len             = data_len;
    out->data   = (uint64_t *)(uintptr_t)dp;
    out->checks = (uint64_t *)(uintptr_t)cp;
    return FCM_OK;
}
