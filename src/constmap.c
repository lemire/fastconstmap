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

/* The paired layout reads each {value, check} slot as one 128-bit word. SSE2
 * is baseline on x86-64 and NEON on AArch64, so no runtime dispatch is needed;
 * other targets take the scalar path, which does the same six loads the split
 * layout does but from three cache lines. */
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  include <emmintrin.h>
#  define FCM_HAVE_SSE2 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#  include <arm_neon.h>
#  define FCM_HAVE_NEON 1
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

#if defined(_MSC_VER)
#  define FCM_FORCE_INLINE static __forceinline
#else
#  define FCM_FORCE_INLINE static inline __attribute__((always_inline))
#endif

/* Both key hashes are called through the vendored xxhash.h's force-inlined
 * internals rather than its public entry points: those are merely
 * `static inline` even under XXH_INLINE_ALL, and with this many call sites
 * clang leaves them out of line, so every lookup would pay a call, a register
 * spill and the loss of the hoisting the batched lookups count on.
 *
 * Contract: fcm_hash_key_xxh64 must compute exactly XXH64(key, len, 0) and
 * fcm_hash_key_xxh3 exactly XXH3_64bits(key, len), because that is what every
 * serialized map was built with (in Go and Rust as much as here). Both are
 * verbatim copies of those functions' bodies, and the version check makes a
 * header bump a build failure until someone has compared them again, since a
 * changed body would silently desynchronise every existing map. */
#if XXH_VERSION_NUMBER != 802
#  error "xxhash.h changed: re-check fcm_hash_key_xxh64/xxh3 against XXH64/XXH3_64bits, then update this version"
#endif

FCM_FORCE_INLINE uint64_t fcm_hash_key_xxh64(const char *key, size_t len) {
    return (uint64_t)XXH64_endian_align((const uint8_t *)key, len, 0, XXH_unaligned);
}

/* Out of line on purpose: only maps loaded from fastconstmap <= 0.9 files use
 * it, and keeping it out of the lookups keeps them small. */
static uint64_t fcm_hash_key_xxh3(const char *key, size_t len) {
    return (uint64_t)XXH3_64bits_internal(key, len, 0, XXH3_kSecret,
                                          sizeof(XXH3_kSecret),
                                          XXH3_hashLong_64b_default);
}

FCM_FORCE_INLINE uint64_t fcm_hash_key(uint32_t hash, const char *key, size_t len) {
    if (hash == FCM_HASH_XXH3) return fcm_hash_key_xxh3(key, len);
    return fcm_hash_key_xxh64(key, len);
}

/* Hashes and mixes a block of keys, choosing the hash once for the block
 * rather than per key, so the common XXH64 loop carries no branch and the
 * compiler can hoist what is loop-invariant out of it. */
FCM_FORCE_INLINE void fcm_hash_block(uint32_t hash, const fcm_key_t *keys, size_t n,
                                     uint64_t seed, uint64_t *out) {
    if (hash == FCM_HASH_XXH3) {
        for (size_t j = 0; j < n; j++) {
            out[j] = fcm_mixsplit(fcm_hash_key_xxh3(keys[j].bytes, keys[j].len), seed);
        }
    } else {
        for (size_t j = 0; j < n; j++) {
            out[j] = fcm_mixsplit(fcm_hash_key_xxh64(keys[j].bytes, keys[j].len), seed);
        }
    }
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
/* Construction (peeling) — shared by all three constructors                 */
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

/* XOR of three slots, as the paired lookup reads them. */
static inline fcm_slot_t fcm_slot_xor3(const fcm_slot_t *slots,
                                       uint32_t h0, uint32_t h1, uint32_t h2) {
    fcm_slot_t r;
#if defined(FCM_HAVE_SSE2)
    __m128i v = _mm_loadu_si128((const __m128i *)&slots[h0]);
    v = _mm_xor_si128(v, _mm_loadu_si128((const __m128i *)&slots[h1]));
    v = _mm_xor_si128(v, _mm_loadu_si128((const __m128i *)&slots[h2]));
    r.value = (uint64_t)_mm_cvtsi128_si64(v);
    r.check = (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(v, v));
#elif defined(FCM_HAVE_NEON)
    uint64x2_t v = vld1q_u64((const uint64_t *)&slots[h0]);
    v = veorq_u64(v, vld1q_u64((const uint64_t *)&slots[h1]));
    v = veorq_u64(v, vld1q_u64((const uint64_t *)&slots[h2]));
    r.value = vgetq_lane_u64(v, 0);
    r.check = vgetq_lane_u64(v, 1);
#else
    r.value = slots[h0].value ^ slots[h1].value ^ slots[h2].value;
    r.check = slots[h0].check ^ slots[h1].check ^ slots[h2].check;
#endif
    return r;
}

int fcm_constmap_new_with_hash(fcm_constmap_t *out,
                     const fcm_key_t *keys,
                     const uint64_t  *values,
                     size_t n, uint32_t hash) {
    if (!out) return FCM_E_LENGTH_MISMATCH;
    if (hash != FCM_HASH_XXH64 && hash != FCM_HASH_XXH3) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    out->hash = hash;
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
        hashed[i] = fcm_hash_key(hash, keys[i].bytes, keys[i].len);
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
    out->n                    = size;
    out->data                 = data;
    out->hash                 = hash;
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

int fcm_verified_constmap_new_with_hash(fcm_verified_constmap_t *out,
                              const fcm_key_t *keys,
                              const uint64_t  *values,
                              size_t n, uint32_t hash) {
    if (!out) return FCM_E_LENGTH_MISMATCH;
    if (hash != FCM_HASH_XXH64 && hash != FCM_HASH_XXH3) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    out->hash = hash;
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
        hashed[i] = fcm_hash_key(hash, keys[i].bytes, keys[i].len);
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
    out->n                    = size;
    out->data                 = data;
    out->checks               = checks;
    out->hash                 = hash;
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

int fcm_paired_verified_constmap_new_with_hash(fcm_paired_verified_constmap_t *out,
                                     const fcm_key_t *keys,
                                     const uint64_t  *values,
                                     size_t n, uint32_t hash) {
    if (!out) return FCM_E_LENGTH_MISMATCH;
    if (hash != FCM_HASH_XXH64 && hash != FCM_HASH_XXH3) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    out->hash = hash;
    if (n == 0) return FCM_OK;
    if (n > 0xFFFFFFFFu) return FCM_E_LENGTH_MISMATCH;

    uint32_t size = (uint32_t)n;
    int rc = FCM_OK;

    fcm_params_t params;
    fcm_init_params(&params, size);

    uint64_t *hashed        = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    fcm_slot_t *slots       = (fcm_slot_t *)calloc(params.array_len, sizeof(fcm_slot_t));
    uint32_t *alone         = (uint32_t *)malloc((size_t)params.array_len * sizeof(uint32_t));
    uint8_t  *t2count       = (uint8_t  *)calloc(params.array_len, sizeof(uint8_t));
    uint64_t *t2hash        = (uint64_t *)calloc(params.array_len, sizeof(uint64_t));
    uint8_t  *reverse_h     = (uint8_t  *)malloc((size_t)size);
    uint64_t *reverse_order = (uint64_t *)calloc((size_t)size + 1, sizeof(uint64_t));
    fcm_pair_t *pairs       = (fcm_pair_t *)malloc((size_t)n * sizeof(fcm_pair_t));

    if (!hashed || !slots || !alone || !t2count || !t2hash ||
        !reverse_h || !reverse_order || !pairs) {
        rc = FCM_E_NOMEM;
        goto cleanup;
    }

    for (size_t i = 0; i < n; i++) {
        hashed[i] = fcm_hash_key(hash, keys[i].bytes, keys[i].len);
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
        slots[h012[found]].value = val ^ slots[h012[found + 1]].value ^ slots[h012[found + 2]].value;
        slots[h012[found]].check = fp  ^ slots[h012[found + 1]].check ^ slots[h012[found + 2]].check;
    }

    out->seed                 = params.seed;
    out->segment_length       = params.segment_length;
    out->segment_length_mask  = params.segment_length_mask;
    out->segment_count        = params.segment_count;
    out->segment_count_length = params.segment_count_length;
    out->data_len             = params.array_len;
    out->n                    = size;
    out->slots                = slots;
    out->hash                 = hash;
    slots = NULL;  /* ownership transferred */

cleanup:
    free(hashed);
    free(slots);
    free(alone);
    free(t2count);
    free(t2hash);
    free(reverse_h);
    free(reverse_order);
    free(pairs);
    return rc;
}

int fcm_constmap_new(fcm_constmap_t *out, const fcm_key_t *keys,
                     const uint64_t *values, size_t n) {
    return fcm_constmap_new_with_hash(out, keys, values, n, FCM_HASH_XXH64);
}

int fcm_verified_constmap_new(fcm_verified_constmap_t *out, const fcm_key_t *keys,
                              const uint64_t *values, size_t n) {
    return fcm_verified_constmap_new_with_hash(out, keys, values, n, FCM_HASH_XXH64);
}

int fcm_paired_verified_constmap_new(fcm_paired_verified_constmap_t *out, const fcm_key_t *keys,
                                     const uint64_t *values, size_t n) {
    return fcm_paired_verified_constmap_new_with_hash(out, keys, values, n, FCM_HASH_XXH64);
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

void fcm_paired_verified_constmap_free(fcm_paired_verified_constmap_t *pm) {
    if (!pm) return;
    free(pm->slots);
    memset(pm, 0, sizeof(*pm));
}

/* ------------------------------------------------------------------------- */
/* Lookup                                                                    */
/* ------------------------------------------------------------------------- */

uint64_t fcm_constmap_lookup(const fcm_constmap_t *cm,
                             const char *key, size_t key_len) {
    if (cm->data_len == 0) return 0;
    uint64_t hash = fcm_mixsplit(fcm_hash_key(cm->hash, key, key_len), cm->seed);
    uint32_t h0, h1, h2;
    fcm_get_h012(hash, cm->segment_length, cm->segment_length_mask,
                 cm->segment_count_length, &h0, &h1, &h2);
    return cm->data[h0] ^ cm->data[h1] ^ cm->data[h2];
}

uint64_t fcm_verified_constmap_lookup(const fcm_verified_constmap_t *vm,
                                      const char *key, size_t key_len) {
    if (vm->data_len == 0) return FCM_NOT_FOUND;
    uint64_t hash = fcm_mixsplit(fcm_hash_key(vm->hash, key, key_len), vm->seed);
    uint32_t h0, h1, h2;
    fcm_get_h012(hash, vm->segment_length, vm->segment_length_mask,
                 vm->segment_count_length, &h0, &h1, &h2);
    uint64_t fp = vm->checks[h0] ^ vm->checks[h1] ^ vm->checks[h2];
    if (fp != fcm_fingerprint(hash)) return FCM_NOT_FOUND;
    return vm->data[h0] ^ vm->data[h1] ^ vm->data[h2];
}

uint64_t fcm_paired_verified_constmap_lookup(const fcm_paired_verified_constmap_t *pm,
                                             const char *key, size_t key_len) {
    if (pm->data_len == 0) return FCM_NOT_FOUND;
    uint64_t hash = fcm_mixsplit(fcm_hash_key(pm->hash, key, key_len), pm->seed);
    uint32_t h0, h1, h2;
    fcm_get_h012(hash, pm->segment_length, pm->segment_length_mask,
                 pm->segment_count_length, &h0, &h1, &h2);
    fcm_slot_t r = fcm_slot_xor3(pm->slots, h0, h1, h2);
    return r.check == fcm_fingerprint(hash) ? r.value : FCM_NOT_FOUND;
}

/* Batched lookup.
 *
 * A block of keys is hashed first, then its positions are computed, then the
 * values are gathered. Splitting the work into phases lets the array reads of
 * a whole block overlap: with a lookup per iteration, each key's loads cannot
 * start until the previous key's hash is finished, so the memory latency of
 * every key is paid end to end. Eight was the best or tied-best block size of
 * 4, 8, 16 and 32 on both an Apple M4 Max and an Intel Xeon Gold 6548N.
 *
 * This matches the MapMany/MapManyInto of the Go original (v1.1.0), minus its
 * batched hash routine: that exists because Go pays a call per key and reloads
 * the XXH64 primes each time, whereas fcm_hash_key_xxh64 is force-inlined
 * straight into the loop below, so the compiler already hoists what is
 * loop-invariant out of the block. */
#ifndef FCM_BATCH_BLOCK
#define FCM_BATCH_BLOCK 8
#endif

void fcm_constmap_lookup_many(const fcm_constmap_t *cm,
                              const fcm_key_t *keys, size_t n,
                              uint64_t *out) {
    if (cm->data_len == 0) {
        for (size_t i = 0; i < n; i++) out[i] = 0;
        return;
    }

    const uint64_t *data = cm->data;
    uint32_t h0[FCM_BATCH_BLOCK], h1[FCM_BATCH_BLOCK], h2[FCM_BATCH_BLOCK];
    uint64_t hashes[FCM_BATCH_BLOCK];

    size_t i = 0;
    for (; i + FCM_BATCH_BLOCK <= n; i += FCM_BATCH_BLOCK) {
        fcm_hash_block(cm->hash, keys + i, FCM_BATCH_BLOCK, cm->seed, hashes);
        for (size_t j = 0; j < FCM_BATCH_BLOCK; j++) {
            fcm_get_h012(hashes[j], cm->segment_length, cm->segment_length_mask,
                         cm->segment_count_length, &h0[j], &h1[j], &h2[j]);
        }
        for (size_t j = 0; j < FCM_BATCH_BLOCK; j++) {
            out[i + j] = data[h0[j]] ^ data[h1[j]] ^ data[h2[j]];
        }
    }
    /* Tail: fewer than one block left. */
    for (; i < n; i++) {
        out[i] = fcm_constmap_lookup(cm, keys[i].bytes, keys[i].len);
    }
}

void fcm_verified_constmap_lookup_many(const fcm_verified_constmap_t *vm,
                                       const fcm_key_t *keys, size_t n,
                                       uint64_t *out) {
    if (vm->data_len == 0) {
        for (size_t i = 0; i < n; i++) out[i] = FCM_NOT_FOUND;
        return;
    }

    const uint64_t *data   = vm->data;
    const uint64_t *checks = vm->checks;
    uint32_t h0[FCM_BATCH_BLOCK], h1[FCM_BATCH_BLOCK], h2[FCM_BATCH_BLOCK];
    uint64_t hashes[FCM_BATCH_BLOCK];

    size_t i = 0;
    for (; i + FCM_BATCH_BLOCK <= n; i += FCM_BATCH_BLOCK) {
        fcm_hash_block(vm->hash, keys + i, FCM_BATCH_BLOCK, vm->seed, hashes);
        for (size_t j = 0; j < FCM_BATCH_BLOCK; j++) {
            fcm_get_h012(hashes[j], vm->segment_length, vm->segment_length_mask,
                         vm->segment_count_length, &h0[j], &h1[j], &h2[j]);
        }
        for (size_t j = 0; j < FCM_BATCH_BLOCK; j++) {
            uint64_t fp = checks[h0[j]] ^ checks[h1[j]] ^ checks[h2[j]];
            uint64_t v  = data[h0[j]] ^ data[h1[j]] ^ data[h2[j]];
            out[i + j] = (fp == fcm_fingerprint(hashes[j])) ? v : FCM_NOT_FOUND;
        }
    }
    for (; i < n; i++) {
        out[i] = fcm_verified_constmap_lookup(vm, keys[i].bytes, keys[i].len);
    }
}

void fcm_paired_verified_constmap_lookup_many(const fcm_paired_verified_constmap_t *pm,
                                              const fcm_key_t *keys, size_t n,
                                              uint64_t *out) {
    if (pm->data_len == 0) {
        for (size_t i = 0; i < n; i++) out[i] = FCM_NOT_FOUND;
        return;
    }

    const fcm_slot_t *slots = pm->slots;
    uint32_t h0[FCM_BATCH_BLOCK], h1[FCM_BATCH_BLOCK], h2[FCM_BATCH_BLOCK];
    uint64_t hashes[FCM_BATCH_BLOCK];

    size_t i = 0;
    for (; i + FCM_BATCH_BLOCK <= n; i += FCM_BATCH_BLOCK) {
        fcm_hash_block(pm->hash, keys + i, FCM_BATCH_BLOCK, pm->seed, hashes);
        for (size_t j = 0; j < FCM_BATCH_BLOCK; j++) {
            fcm_get_h012(hashes[j], pm->segment_length, pm->segment_length_mask,
                         pm->segment_count_length, &h0[j], &h1[j], &h2[j]);
        }
        for (size_t j = 0; j < FCM_BATCH_BLOCK; j++) {
            fcm_slot_t r = fcm_slot_xor3(slots, h0[j], h1[j], h2[j]);
            out[i + j] = (r.check == fcm_fingerprint(hashes[j])) ? r.value : FCM_NOT_FOUND;
        }
    }
    for (; i < n; i++) {
        out[i] = fcm_paired_verified_constmap_lookup(pm, keys[i].bytes, keys[i].len);
    }
}


/* ------------------------------------------------------------------------- */
/* Serialisation                                                             */
/*                                                                           */
/* Binary format (little-endian), shared with github.com/lemire/constmap     */
/* (Go) and rsconstmap:                                                      */
/*   [8] magic                                                               */
/*   [8] seed                                                                */
/*   [4] segment_length                                                      */
/*   [4] segment_count                                                       */
/*   [4] data_len: number of words, or of slots for a paired map             */
/*   [4] original key count. Go and rsconstmap write zero here and ignore   */
/*       it on read, so a map loaded from their files reports n = 0. Absent  */
/*       from CMAP0001, whose header is 28 bytes.                            */
/*   [8 * data_len] data                                                     */
/*   (verified only) [8 * data_len] checks                                   */
/*   (paired only, in place of data and checks) [16 * data_len] slots, each  */
/*       a value word followed by its check word                             */
/*   [8] FNV-1a-64 checksum of all preceding bytes                           */
/*                                                                           */
/* The magic identifies the map type, the key hash and the header size:      */
/*                                                                           */
/*   magic     type      hash   header  written by                           */
/*   CMAP0003  ConstMap  XXH64  32      fastconstmap >= 0.10                 */
/*   CMAP0001  ConstMap  XXH64  28      constmap (Go), rsconstmap            */
/*   CMAP0002  ConstMap  XXH3   32      fastconstmap <= 0.9, and maps built  */
/*                                      with FCM_HASH_XXH3 since             */
/*   VMAP0001  Verified  XXH64  32      all three                            */
/*   VCMP0002  Verified  XXH3   32      fastconstmap <= 0.9, and maps built  */
/*                                      with FCM_HASH_XXH3 since             */
/*   PMAP0001  Paired    XXH64  32      all three                            */
/*   PVCM0001  Paired    XXH3   32      fastconstmap, maps built with XXH3   */
/*                                                                           */
/* Each reader accepts every magic of its type. Each writer produces the     */
/* magic of its type for the hash the map was built with: the shared XXH64   */
/* format normally, the legacy one for a map loaded from a fastconstmap 0.9  */
/* file, since its table only answers to XXH3 and cannot be converted (the   */
/* only way to a shared-format file is rebuilding from the keys). CMAP0003   */
/* exists because CMAP0001 has no room for the                               */
/* key count that fcm_constmap_t.n and Python's len() report. A 32-byte      */
/* header also keeps the array 8-byte aligned for the zero-copy views, which */
/* a CMAP0001 buffer is not unless it starts 4 bytes off an 8-byte boundary. */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t  magic[8];
    uint32_t hash;         /* FCM_HASH_* */
    uint32_t header_size;  /* 28 or 32 */
} fcm_format_t;

static const fcm_format_t fcm_constmap_formats[] = {
    { {'C','M','A','P','0','0','0','3'}, FCM_HASH_XXH64, 32 },
    { {'C','M','A','P','0','0','0','1'}, FCM_HASH_XXH64, 28 },
    { {'C','M','A','P','0','0','0','2'}, FCM_HASH_XXH3,  32 },
};
static const fcm_format_t fcm_verified_formats[] = {
    { {'V','M','A','P','0','0','0','1'}, FCM_HASH_XXH64, 32 },
    { {'V','C','M','P','0','0','0','2'}, FCM_HASH_XXH3,  32 },
};
static const fcm_format_t fcm_paired_formats[] = {
    { {'P','M','A','P','0','0','0','1'}, FCM_HASH_XXH64, 32 },
    { {'P','V','C','M','0','0','0','1'}, FCM_HASH_XXH3,  32 },
};
#define FCM_NFORMATS(a) (sizeof(a) / sizeof((a)[0]))

/* The format a writer uses for a map built with `hash`: the first entry of
 * the table with that hash, or NULL if the type has none for it. */
static const fcm_format_t *fcm_format_for(const fcm_format_t *formats, size_t nformats,
                                          uint32_t hash) {
    for (size_t i = 0; i < nformats; i++) {
        if (formats[i].hash == hash) return &formats[i];
    }
    return NULL;
}

#define FCM_HEADER_SIZE  32u  /* what the writers produce */
#define FCM_TRAILER_SIZE  8u  /* checksum */

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

/* Writes the 32-byte header every writer produces. */
static uint8_t *fcm_write_header(uint8_t *p, const fcm_format_t *fmt, uint64_t seed,
                                 uint32_t segment_length, uint32_t segment_count,
                                 uint32_t data_len, uint32_t n) {
    memcpy(p, fmt->magic, 8);            p += 8;
    fcm_write_u64(p, seed);              p += 8;
    fcm_write_u32(p, segment_length);    p += 4;
    fcm_write_u32(p, segment_count);     p += 4;
    fcm_write_u32(p, data_len);          p += 4;
    fcm_write_u32(p, n);                 p += 4;
    return p;
}

/* A parsed header, with the buffer already checked for length and checksum. */
typedef struct {
    const fcm_format_t *format;
    uint64_t seed;
    uint32_t segment_length;
    uint32_t segment_count;
    uint32_t data_len;
    uint32_t n;
    size_t   payload;   /* offset of the first array */
} fcm_header_t;

/* Recognises the magic among `formats`, reads the header, and verifies the
 * buffer holds the whole map and that its checksum matches. `entry_bytes` is
 * what one unit of data_len occupies: 8 for a ConstMap, 16 for the others. */
static int fcm_parse(const uint8_t *p, size_t buf_len,
                     const fcm_format_t *formats, size_t nformats,
                     size_t entry_bytes, fcm_header_t *h) {
    if (buf_len < 8) return FCM_E_SHORT_BUFFER;
    h->format = NULL;
    for (size_t i = 0; i < nformats; i++) {
        if (memcmp(p, formats[i].magic, 8) == 0) { h->format = &formats[i]; break; }
    }
    if (!h->format) return FCM_E_INVALID_FORMAT;

    size_t header_size = h->format->header_size;
    if (buf_len < header_size + FCM_TRAILER_SIZE) return FCM_E_SHORT_BUFFER;
    h->seed           = fcm_read_u64(p + 8);
    h->segment_length = fcm_read_u32(p + 16);
    h->segment_count  = fcm_read_u32(p + 20);
    h->data_len       = fcm_read_u32(p + 24);
    h->n              = header_size == 32 ? fcm_read_u32(p + 28) : 0;
    h->payload        = header_size;

    size_t expected = header_size + (size_t)h->data_len * entry_bytes + FCM_TRAILER_SIZE;
    if (buf_len < expected) return FCM_E_SHORT_BUFFER;

    uint64_t got_sum      = fcm_read_u64(p + expected - 8);
    uint64_t expected_sum = fcm_fnv1a64(p, expected - 8);
    if (got_sum != expected_sum) return FCM_E_CHECKSUM;
    return FCM_OK;
}

/* The segment parameters must describe exactly `slot_count` slots, so that
 * every position a lookup derives from them is in range: h0 is below
 * segment_count * segment_length, and h1 and h2 each one segment further, so
 * h2 < (segment_count + 2) * segment_length. That needs segment_length to be
 * a power of two (h1 and h2 are formed by XORing bits below it) and
 * segment_count to be at least one (with zero, h0 is always 0 and h2 lands in
 * a third segment that does not exist). The lookups index the slot array
 * without bounds checks, so this is enforced on every deserialized map; the
 * checksum catches accidental corruption, this catches a file that is
 * consistent but not ours. */
static int fcm_paired_params_ok(uint32_t segment_length, uint32_t segment_count,
                                uint32_t slot_count) {
    if (slot_count == 0) return 1;
    if (segment_length == 0 || (segment_length & (segment_length - 1)) != 0) return 0;
    if (segment_count == 0) return 0;
    return ((uint64_t)segment_count + 2) * (uint64_t)segment_length == (uint64_t)slot_count;
}

static inline int fcm_host_is_little_endian(void) {
    const uint16_t x = 1;
    return *(const uint8_t *)&x == 1;
}

/* ---- ConstMap ---- */

size_t fcm_constmap_serialized_size(const fcm_constmap_t *cm) {
    return FCM_HEADER_SIZE + (size_t)cm->data_len * 8u + FCM_TRAILER_SIZE;
}

int fcm_constmap_write(const fcm_constmap_t *cm, void *buf) {
    const fcm_format_t *fmt = fcm_format_for(fcm_constmap_formats, FCM_NFORMATS(fcm_constmap_formats), cm->hash);
    if (!fmt) return FCM_E_INVALID_FORMAT;
    uint8_t *start = (uint8_t *)buf;
    uint8_t *p = fcm_write_header(start, fmt, cm->seed,
                                  cm->segment_length, cm->segment_count,
                                  cm->data_len, cm->n);
    for (uint32_t i = 0; i < cm->data_len; i++) {
        fcm_write_u64(p, cm->data[i]); p += 8;
    }
    fcm_write_u64(p, fcm_fnv1a64(start, (size_t)(p - start)));
    return FCM_OK;
}

static void fcm_constmap_set_header(fcm_constmap_t *out, const fcm_header_t *h) {
    out->seed                 = h->seed;
    out->segment_length       = h->segment_length;
    out->segment_length_mask  = h->segment_length ? h->segment_length - 1 : 0;
    out->segment_count        = h->segment_count;
    out->segment_count_length = h->segment_count * h->segment_length;
    out->data_len             = h->data_len;
    out->n                    = h->n;
    out->hash                 = h->format->hash;
}

int fcm_constmap_read(fcm_constmap_t *out, const void *buf, size_t buf_len) {
    if (!out) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    const uint8_t *p = (const uint8_t *)buf;
    fcm_header_t h;
    int rc = fcm_parse(p, buf_len, fcm_constmap_formats, FCM_NFORMATS(fcm_constmap_formats), 8, &h);
    if (rc != FCM_OK) return rc;

    uint64_t *data = NULL;
    if (h.data_len > 0) {
        data = (uint64_t *)malloc((size_t)h.data_len * sizeof(uint64_t));
        if (!data) return FCM_E_NOMEM;
        const uint8_t *dp = p + h.payload;
        for (uint32_t i = 0; i < h.data_len; i++) {
            data[i] = fcm_read_u64(dp + (size_t)i * 8);
        }
    }
    fcm_constmap_set_header(out, &h);
    out->data = data;
    return FCM_OK;
}

int fcm_constmap_view(fcm_constmap_t *out, const void *buf, size_t buf_len) {
    if (!out) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    if (!fcm_host_is_little_endian()) return FCM_E_INVALID_FORMAT;
    const uint8_t *p = (const uint8_t *)buf;
    fcm_header_t h;
    int rc = fcm_parse(p, buf_len, fcm_constmap_formats, FCM_NFORMATS(fcm_constmap_formats), 8, &h);
    if (rc != FCM_OK) return rc;

    const uint8_t *dp = p + h.payload;
    if (((uintptr_t)dp & 7u) != 0) return FCM_E_UNALIGNED;

    fcm_constmap_set_header(out, &h);
    /* Borrowed pointer into `buf`; the integer round-trip launders away the
     * source const-ness. Lookups only read this array. */
    out->data = (uint64_t *)(uintptr_t)dp;
    return FCM_OK;
}

/* ---- VerifiedConstMap ---- */

size_t fcm_verified_constmap_serialized_size(const fcm_verified_constmap_t *vm) {
    return FCM_HEADER_SIZE + (size_t)vm->data_len * 16u + FCM_TRAILER_SIZE;
}

int fcm_verified_constmap_write(const fcm_verified_constmap_t *vm, void *buf) {
    const fcm_format_t *fmt = fcm_format_for(fcm_verified_formats, FCM_NFORMATS(fcm_verified_formats), vm->hash);
    if (!fmt) return FCM_E_INVALID_FORMAT;
    uint8_t *start = (uint8_t *)buf;
    uint8_t *p = fcm_write_header(start, fmt, vm->seed,
                                  vm->segment_length, vm->segment_count,
                                  vm->data_len, vm->n);
    for (uint32_t i = 0; i < vm->data_len; i++) {
        fcm_write_u64(p, vm->data[i]); p += 8;
    }
    for (uint32_t i = 0; i < vm->data_len; i++) {
        fcm_write_u64(p, vm->checks[i]); p += 8;
    }
    fcm_write_u64(p, fcm_fnv1a64(start, (size_t)(p - start)));
    return FCM_OK;
}

static void fcm_verified_set_header(fcm_verified_constmap_t *out, const fcm_header_t *h) {
    out->seed                 = h->seed;
    out->segment_length       = h->segment_length;
    out->segment_length_mask  = h->segment_length ? h->segment_length - 1 : 0;
    out->segment_count        = h->segment_count;
    out->segment_count_length = h->segment_count * h->segment_length;
    out->data_len             = h->data_len;
    out->n                    = h->n;
    out->hash                 = h->format->hash;
}

int fcm_verified_constmap_read(fcm_verified_constmap_t *out, const void *buf, size_t buf_len) {
    if (!out) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    const uint8_t *p = (const uint8_t *)buf;
    fcm_header_t h;
    int rc = fcm_parse(p, buf_len, fcm_verified_formats, FCM_NFORMATS(fcm_verified_formats), 16, &h);
    if (rc != FCM_OK) return rc;

    uint64_t *data   = NULL;
    uint64_t *checks = NULL;
    if (h.data_len > 0) {
        data   = (uint64_t *)malloc((size_t)h.data_len * sizeof(uint64_t));
        checks = (uint64_t *)malloc((size_t)h.data_len * sizeof(uint64_t));
        if (!data || !checks) { free(data); free(checks); return FCM_E_NOMEM; }
        const uint8_t *dp = p + h.payload;
        const uint8_t *cp = dp + (size_t)h.data_len * 8;
        for (uint32_t i = 0; i < h.data_len; i++) {
            data[i]   = fcm_read_u64(dp + (size_t)i * 8);
            checks[i] = fcm_read_u64(cp + (size_t)i * 8);
        }
    }
    fcm_verified_set_header(out, &h);
    out->data   = data;
    out->checks = checks;
    return FCM_OK;
}

int fcm_verified_constmap_view(fcm_verified_constmap_t *out, const void *buf, size_t buf_len) {
    if (!out) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    if (!fcm_host_is_little_endian()) return FCM_E_INVALID_FORMAT;
    const uint8_t *p = (const uint8_t *)buf;
    fcm_header_t h;
    int rc = fcm_parse(p, buf_len, fcm_verified_formats, FCM_NFORMATS(fcm_verified_formats), 16, &h);
    if (rc != FCM_OK) return rc;

    const uint8_t *dp = p + h.payload;
    const uint8_t *cp = dp + (size_t)h.data_len * 8u;
    if (((uintptr_t)dp & 7u) != 0) return FCM_E_UNALIGNED;

    fcm_verified_set_header(out, &h);
    out->data   = (uint64_t *)(uintptr_t)dp;
    out->checks = (uint64_t *)(uintptr_t)cp;
    return FCM_OK;
}

/* ---- PairedVerifiedConstMap ---- */

size_t fcm_paired_verified_constmap_serialized_size(const fcm_paired_verified_constmap_t *pm) {
    return FCM_HEADER_SIZE + (size_t)pm->data_len * 16u + FCM_TRAILER_SIZE;
}

int fcm_paired_verified_constmap_write(const fcm_paired_verified_constmap_t *pm, void *buf) {
    const fcm_format_t *fmt = fcm_format_for(fcm_paired_formats, FCM_NFORMATS(fcm_paired_formats), pm->hash);
    if (!fmt) return FCM_E_INVALID_FORMAT;
    uint8_t *start = (uint8_t *)buf;
    uint8_t *p = fcm_write_header(start, fmt, pm->seed,
                                  pm->segment_length, pm->segment_count,
                                  pm->data_len, pm->n);
    for (uint32_t i = 0; i < pm->data_len; i++) {
        fcm_write_u64(p, pm->slots[i].value); p += 8;
        fcm_write_u64(p, pm->slots[i].check); p += 8;
    }
    fcm_write_u64(p, fcm_fnv1a64(start, (size_t)(p - start)));
    return FCM_OK;
}

static void fcm_paired_set_header(fcm_paired_verified_constmap_t *out, const fcm_header_t *h) {
    out->seed                 = h->seed;
    out->segment_length       = h->segment_length;
    out->segment_length_mask  = h->segment_length ? h->segment_length - 1 : 0;
    out->segment_count        = h->segment_count;
    out->segment_count_length = h->segment_count * h->segment_length;
    out->data_len             = h->data_len;
    out->n                    = h->n;
    out->hash                 = h->format->hash;
}

int fcm_paired_verified_constmap_read(fcm_paired_verified_constmap_t *out, const void *buf, size_t buf_len) {
    if (!out) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    const uint8_t *p = (const uint8_t *)buf;
    fcm_header_t h;
    int rc = fcm_parse(p, buf_len, fcm_paired_formats, FCM_NFORMATS(fcm_paired_formats), 16, &h);
    if (rc != FCM_OK) return rc;
    if (!fcm_paired_params_ok(h.segment_length, h.segment_count, h.data_len)) return FCM_E_INVALID_PARAMS;

    fcm_slot_t *slots = NULL;
    if (h.data_len > 0) {
        slots = (fcm_slot_t *)malloc((size_t)h.data_len * sizeof(fcm_slot_t));
        if (!slots) return FCM_E_NOMEM;
        const uint8_t *sp = p + h.payload;
        for (uint32_t i = 0; i < h.data_len; i++) {
            slots[i].value = fcm_read_u64(sp + (size_t)i * 16);
            slots[i].check = fcm_read_u64(sp + (size_t)i * 16 + 8);
        }
    }
    fcm_paired_set_header(out, &h);
    out->slots = slots;
    return FCM_OK;
}

int fcm_paired_verified_constmap_view(fcm_paired_verified_constmap_t *out, const void *buf, size_t buf_len) {
    if (!out) return FCM_E_INVALID_FORMAT;
    memset(out, 0, sizeof(*out));
    if (!fcm_host_is_little_endian()) return FCM_E_INVALID_FORMAT;
    const uint8_t *p = (const uint8_t *)buf;
    fcm_header_t h;
    int rc = fcm_parse(p, buf_len, fcm_paired_formats, FCM_NFORMATS(fcm_paired_formats), 16, &h);
    if (rc != FCM_OK) return rc;
    if (!fcm_paired_params_ok(h.segment_length, h.segment_count, h.data_len)) return FCM_E_INVALID_PARAMS;

    /* 8-byte alignment is enough for correctness (the SIMD loads are
     * unaligned loads); a 16-byte aligned buffer keeps each slot inside one
     * cache line, which is the point of the layout. */
    const uint8_t *sp = p + h.payload;
    if (((uintptr_t)sp & 7u) != 0) return FCM_E_UNALIGNED;

    fcm_paired_set_header(out, &h);
    out->slots = (fcm_slot_t *)(uintptr_t)sp;
    return FCM_OK;
}
