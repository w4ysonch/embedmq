/*
 * benchmark.c — embedmq performance benchmark
 *
 * Runs four measurements and prints the results:
 *   1. embedmq_post() throughput   — 500k messages, includes name→UUID hash
 *   2. embedmq_post_id() throughput — 500k messages, UUID pre-cached (hot path)
 *   3. End-to-end latency          — 10k messages, measures post→handler delay
 *   4. embedmq_uuid() hash speed   — 5M hashes, measures raw FNV-1a speed
 *
 * Build (from repo root):
 *   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/benchmark
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "embedmq.h"

/* ---------------------------------------------------------
 * Timing helpers
 * --------------------------------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------
 * Shared state
 * --------------------------------------------------------- */

static volatile int      g_received;
static volatile uint64_t g_last_recv_ns;

static void handler_noop(const void *data, size_t size, void *ctx)
{
    (void)data; (void)size; (void)ctx;
    __atomic_fetch_add(&g_received, 1, __ATOMIC_RELAXED);
    g_last_recv_ns = now_ns();
}

/* ---------------------------------------------------------
 * Benchmark 1: post() throughput (by name, includes hash)
 * --------------------------------------------------------- */

static void bench_post_throughput(void)
{
    printf("=== Benchmark 1: embedmq_post() throughput ===\n");

    embedmq_t *q = embedmq_create(NULL);
    embedmq_register(q, "bench.evt", handler_noop, NULL);

    const int N = 500000;
    g_received = 0;

    uint64_t t0 = now_ns();
    for (int i = 0; i < N; i++) {
        while (embedmq_post(q, "bench.evt", NULL, 0) == EMBEDMQ_FULL)
            usleep(1);
    }
    /* Wait for consumer to drain */
    while (g_received < N)
        usleep(100);
    uint64_t t1 = now_ns();

    double elapsed_s = (t1 - t0) / 1e9;
    double msgs_per_s = N / elapsed_s;
    printf("  Messages  : %d\n", N);
    printf("  Time      : %.3f s\n", elapsed_s);
    printf("  Throughput: %.0f msgs/sec\n\n", msgs_per_s);

    embedmq_destroy(q);
}

/* ---------------------------------------------------------
 * Benchmark 2: post_id() throughput (UUID cached, hot path)
 * --------------------------------------------------------- */

static void bench_post_id_throughput(void)
{
    printf("=== Benchmark 2: embedmq_post_id() throughput (hot path) ===\n");

    embedmq_t *q = embedmq_create(NULL);
    embedmq_register(q, "bench.evt", handler_noop, NULL);
    uint32_t uuid = embedmq_uuid("bench.evt");

    const int N = 500000;
    g_received = 0;

    uint64_t t0 = now_ns();
    for (int i = 0; i < N; i++) {
        while (embedmq_post_id(q, uuid, NULL, 0) == EMBEDMQ_FULL)
            usleep(1);
    }
    while (g_received < N)
        usleep(100);
    uint64_t t1 = now_ns();

    double elapsed_s = (t1 - t0) / 1e9;
    double msgs_per_s = N / elapsed_s;
    printf("  Messages  : %d\n", N);
    printf("  Time      : %.3f s\n", elapsed_s);
    printf("  Throughput: %.0f msgs/sec\n\n", msgs_per_s);

    embedmq_destroy(q);
}

/* ---------------------------------------------------------
 * Benchmark 3: end-to-end latency (post → handler called)
 * --------------------------------------------------------- */

static volatile uint64_t g_post_ns;

static void handler_latency(const void *data, size_t size, void *ctx)
{
    (void)data; (void)size; (void)ctx;
    g_last_recv_ns = now_ns();
    __atomic_fetch_add(&g_received, 1, __ATOMIC_RELAXED);
}

static void bench_latency(void)
{
    printf("=== Benchmark 3: end-to-end latency (post → handler) ===\n");

    embedmq_t *q = embedmq_create(NULL);
    embedmq_register(q, "latency.evt", handler_latency, NULL);
    uint32_t uuid = embedmq_uuid("latency.evt");

    const int N = 10000;
    uint64_t total_ns = 0;
    uint64_t min_ns = UINT64_MAX;
    uint64_t max_ns = 0;

    for (int i = 0; i < N; i++) {
        g_received = 0;
        g_post_ns  = now_ns();
        embedmq_post_id(q, uuid, NULL, 0);

        while (!g_received)
            ; /* spin-wait for this single message */

        uint64_t lat = g_last_recv_ns - g_post_ns;
        total_ns += lat;
        if (lat < min_ns) min_ns = lat;
        if (lat > max_ns) max_ns = lat;

        usleep(10); /* brief pause between samples */
    }

    printf("  Samples   : %d\n", N);
    printf("  Avg       : %.2f us\n", total_ns / (double)N / 1000.0);
    printf("  Min       : %.2f us\n", min_ns / 1000.0);
    printf("  Max       : %.2f us\n\n", max_ns / 1000.0);

    embedmq_destroy(q);
}

/* ---------------------------------------------------------
 * Benchmark 4: uuid() hash speed
 * --------------------------------------------------------- */

static void bench_uuid_hash(void)
{
    printf("=== Benchmark 4: embedmq_uuid() hash speed ===\n");

    const int N = 5000000;
    volatile uint32_t sink = 0;

    uint64_t t0 = now_ns();
    for (int i = 0; i < N; i++)
        sink ^= embedmq_uuid("battery.voltage.changed");
    uint64_t t1 = now_ns();

    (void)sink;
    double ns_per_hash = (t1 - t0) / (double)N;
    printf("  Iterations: %d\n", N);
    printf("  ns/hash   : %.2f\n", ns_per_hash);
    printf("  Hashes/sec: %.0f\n\n", 1e9 / ns_per_hash);
}

/* ---------------------------------------------------------
 * main
 * --------------------------------------------------------- */

int main(void)
{
    printf("embedmq benchmark\n");
    printf("=================\n\n");

    bench_post_throughput();
    bench_post_id_throughput();
    bench_latency();
    bench_uuid_hash();

    return 0;
}
