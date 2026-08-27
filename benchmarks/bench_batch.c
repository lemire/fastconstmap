/*
 * bench_batch — C-level microbenchmark for the batched lookups.
 *
 * Mirrors the Go original's batch_bench_test.go (constmap v1.1.0): a 2000-key
 * batch against a 1,000,000-key map, reported in ns/key.
 *
 *   cold — rotates through 64 distinct random batches, so the cache lines the
 *          lookups touch are not already resident and memory latency shows.
 *   hot  — replays one batch, so the touched region of the array stays
 *          cache-resident and hashing dominates instead.
 *
 * Build:
 *   cc -O3 -std=c11 -Isrc benchmarks/bench_batch.c src/constmap.c -lm -o bench_batch
 */
/* clock_gettime is POSIX; -std=c11 alone does not expose it. */
#define _POSIX_C_SOURCE 200809L

#include "constmap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAP_N        1000000
#define BATCH_SIZE   2000
#define NUM_POOLS    64
#define REPEATS      2000

static uint64_t rng_state = 0x853c49e6748fea9bULL;

static uint64_t next_rand(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* Key text lives in one arena, in index order, exactly as a caller's key list
 * would. Each batch clones the keys it draws into its own arena so that a
 * lookup is not also charged for a scattered walk over the arena — that cost
 * belongs to whatever produced the keys, not to the map. */
typedef struct {
    char      *arena;
    fcm_key_t *keys;
    size_t     n;
} keyset_t;

static void keyset_free(keyset_t *ks) {
    free(ks->arena);
    free(ks->keys);
}

static int keyset_build(keyset_t *ks, size_t n) {
    const size_t stride = 32;
    ks->n     = n;
    ks->arena = malloc(n * stride);
    ks->keys  = malloc(n * sizeof(fcm_key_t));
    if (!ks->arena || !ks->keys) return -1;
    for (size_t i = 0; i < n; i++) {
        char *p = ks->arena + i * stride;
        int   len = snprintf(p, stride, "key-%zu-%08x", i,
                             (unsigned)(next_rand() & 0x3FFFFFFF));
        ks->keys[i].bytes = p;
        ks->keys[i].len   = (size_t)len;
    }
    return 0;
}

/* Draw BATCH_SIZE keys at random from `all`, cloned into a compact arena. */
static int batch_build(keyset_t *batch, const keyset_t *all) {
    const size_t stride = 32;
    batch->n     = BATCH_SIZE;
    batch->arena = malloc(BATCH_SIZE * stride);
    batch->keys  = malloc(BATCH_SIZE * sizeof(fcm_key_t));
    if (!batch->arena || !batch->keys) return -1;
    for (size_t i = 0; i < BATCH_SIZE; i++) {
        size_t     src = (size_t)(next_rand() % all->n);
        char      *p   = batch->arena + i * stride;
        memcpy(p, all->keys[src].bytes, all->keys[src].len);
        batch->keys[i].bytes = p;
        batch->keys[i].len   = all->keys[src].len;
    }
    return 0;
}

static volatile uint64_t sink;

static void report(const char *label, double elapsed, size_t keys, double baseline) {
    double ns = elapsed * 1e9 / (double)keys;
    if (baseline > 0.0) {
        printf("  %-34s %7.2f ns/key   %+5.0f%%\n", label, ns,
               100.0 * (baseline - ns) / baseline);
    } else {
        printf("  %-34s %7.2f ns/key\n", label, ns);
    }
    (void)keys;
}

int main(int argc, char **argv) {
    size_t map_n = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : MAP_N;

    keyset_t all;
    if (keyset_build(&all, map_n) < 0) { fprintf(stderr, "oom\n"); return 1; }
    uint64_t *values = malloc(map_n * sizeof(uint64_t));
    if (!values) { fprintf(stderr, "oom\n"); return 1; }
    for (size_t i = 0; i < map_n; i++) values[i] = i;

    fcm_constmap_t          cm;
    fcm_verified_constmap_t vm;
    if (fcm_constmap_new(&cm, all.keys, values, map_n) != FCM_OK) {
        fprintf(stderr, "construction failed\n"); return 1;
    }
    if (fcm_verified_constmap_new(&vm, all.keys, values, map_n) != FCM_OK) {
        fprintf(stderr, "verified construction failed\n"); return 1;
    }

    keyset_t pools[NUM_POOLS];
    for (int p = 0; p < NUM_POOLS; p++) {
        if (batch_build(&pools[p], &all) < 0) { fprintf(stderr, "oom\n"); return 1; }
    }

    uint64_t *out = malloc(BATCH_SIZE * sizeof(uint64_t));
    if (!out) { fprintf(stderr, "oom\n"); return 1; }

    printf("=== fastconstmap batched lookups — %zu keys, batches of %d ===\n\n",
           map_n, BATCH_SIZE);

    double t0, loop_ns;
    size_t total = (size_t)REPEATS * BATCH_SIZE;

    /* ---- ConstMap, cold ---- */
    printf("ConstMap:\n");
    t0 = now_sec();
    for (int i = 0; i < REPEATS; i++) {
        const keyset_t *q = &pools[i % NUM_POOLS];
        for (size_t j = 0; j < q->n; j++)
            out[j] = fcm_constmap_lookup(&cm, q->keys[j].bytes, q->keys[j].len);
        sink = out[0];
    }
    loop_ns = now_sec() - t0;
    report("loop over lookup (cold)", loop_ns, total, 0.0);

    t0 = now_sec();
    for (int i = 0; i < REPEATS; i++) {
        const keyset_t *q = &pools[i % NUM_POOLS];
        fcm_constmap_lookup_many(&cm, q->keys, q->n, out);
        sink = out[0];
    }
    report("lookup_many (cold)", now_sec() - t0, total,
           loop_ns * 1e9 / (double)total);

    /* ---- ConstMap, hot ---- */
    const keyset_t *hot = &pools[0];
    t0 = now_sec();
    for (int i = 0; i < REPEATS; i++) {
        for (size_t j = 0; j < hot->n; j++)
            out[j] = fcm_constmap_lookup(&cm, hot->keys[j].bytes, hot->keys[j].len);
        sink = out[0];
    }
    loop_ns = now_sec() - t0;
    report("loop over lookup (hot)", loop_ns, total, 0.0);

    t0 = now_sec();
    for (int i = 0; i < REPEATS; i++) {
        fcm_constmap_lookup_many(&cm, hot->keys, hot->n, out);
        sink = out[0];
    }
    report("lookup_many (hot)", now_sec() - t0, total,
           loop_ns * 1e9 / (double)total);

    /* ---- VerifiedConstMap, cold ---- */
    printf("\nVerifiedConstMap:\n");
    t0 = now_sec();
    for (int i = 0; i < REPEATS; i++) {
        const keyset_t *q = &pools[i % NUM_POOLS];
        for (size_t j = 0; j < q->n; j++)
            out[j] = fcm_verified_constmap_lookup(&vm, q->keys[j].bytes, q->keys[j].len);
        sink = out[0];
    }
    loop_ns = now_sec() - t0;
    report("loop over lookup (cold)", loop_ns, total, 0.0);

    t0 = now_sec();
    for (int i = 0; i < REPEATS; i++) {
        const keyset_t *q = &pools[i % NUM_POOLS];
        fcm_verified_constmap_lookup_many(&vm, q->keys, q->n, out);
        sink = out[0];
    }
    report("lookup_many (cold)", now_sec() - t0, total,
           loop_ns * 1e9 / (double)total);

    /* ---- VerifiedConstMap, hot ---- */
    t0 = now_sec();
    for (int i = 0; i < REPEATS; i++) {
        for (size_t j = 0; j < hot->n; j++)
            out[j] = fcm_verified_constmap_lookup(&vm, hot->keys[j].bytes, hot->keys[j].len);
        sink = out[0];
    }
    loop_ns = now_sec() - t0;
    report("loop over lookup (hot)", loop_ns, total, 0.0);

    t0 = now_sec();
    for (int i = 0; i < REPEATS; i++) {
        fcm_verified_constmap_lookup_many(&vm, hot->keys, hot->n, out);
        sink = out[0];
    }
    report("lookup_many (hot)", now_sec() - t0, total,
           loop_ns * 1e9 / (double)total);

    for (int p = 0; p < NUM_POOLS; p++) keyset_free(&pools[p]);
    keyset_free(&all);
    free(values);
    free(out);
    fcm_constmap_free(&cm);
    fcm_verified_constmap_free(&vm);
    return 0;
}
