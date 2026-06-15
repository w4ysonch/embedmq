#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "embedmq.h"

/* ---- helpers --------------------------------------------------- */

static int g_count = 0;

static void handler_count(const void *data, size_t size, void *ctx)
{
    (void)data; (void)size; (void)ctx;
    __atomic_fetch_add(&g_count, 1, __ATOMIC_SEQ_CST);
}

typedef struct { int value; } int_msg_t;
static int g_last_value = -1;

static void handler_value(const void *data, size_t size, void *ctx)
{
    assert(size == sizeof(int_msg_t));
    (void)ctx;
    g_last_value = ((const int_msg_t *)data)->value;
}

/* ---- basic tests ----------------------------------------------- */

static void test_uuid_deterministic(void)
{
    uint32_t a = embedmq_uuid("battery.changed");
    uint32_t b = embedmq_uuid("battery.changed");
    uint32_t c = embedmq_uuid("button.press");
    assert(a == b);
    assert(a != c);
    assert(a != 0);
    assert(c != 0);
    printf("  [PASS] test_uuid_deterministic\n");
}

static void test_create_destroy(void)
{
    embedmq_t *q = embedmq_create(NULL);
    assert(q != NULL);
    embedmq_destroy(q);
    printf("  [PASS] test_create_destroy\n");
}

static void test_register_and_post(void)
{
    g_count = 0;
    embedmq_t *q = embedmq_create(NULL);
    assert(q);

    int r = embedmq_register(q, "evt.a", handler_count, NULL);
    assert(r == EMBEDMQ_OK);

    r = embedmq_register(q, "evt.a", handler_count, NULL);
    assert(r == EMBEDMQ_EXIST);

    embedmq_post(q, "evt.a", NULL, 0);
    embedmq_post(q, "evt.a", NULL, 0);
    embedmq_post(q, "evt.a", NULL, 0);

    usleep(20000);
    assert(g_count == 3);

    embedmq_destroy(q);
    printf("  [PASS] test_register_and_post\n");
}

static void test_payload_received(void)
{
    g_last_value = -1;
    embedmq_t *q = embedmq_create(NULL);
    assert(q);

    embedmq_register(q, "int.msg", handler_value, NULL);

    int_msg_t m = { .value = 42 };
    embedmq_post(q, "int.msg", &m, sizeof(m));

    usleep(20000);
    assert(g_last_value == 42);

    embedmq_destroy(q);
    printf("  [PASS] test_payload_received\n");
}

static void test_post_id_hot_path(void)
{
    g_count = 0;
    embedmq_t *q = embedmq_create(NULL);
    assert(q);

    embedmq_register(q, "hot.event", handler_count, NULL);
    uint32_t uuid = embedmq_uuid("hot.event");

    for (int i = 0; i < 10; i++)
        embedmq_post_id(q, uuid, NULL, 0);

    usleep(20000);
    assert(g_count == 10);

    embedmq_destroy(q);
    printf("  [PASS] test_post_id_hot_path\n");
}

static void test_static_mode(void)
{
    g_count = 0;

    embedmq_config_t cfg = {
        .queue_size   = 4096,
        .max_msg_size = 256,
        .max_handlers = 8,
    };
    size_t needed = embedmq_mem_size(&cfg);
    static uint8_t buf[4096 + 8 * 64 + 256 + 256];
    assert(needed <= sizeof(buf));

    embedmq_t *q = embedmq_create_static(buf, needed, &cfg);
    assert(q != NULL);

    embedmq_register(q, "static.evt", handler_count, NULL);
    embedmq_post(q, "static.evt", NULL, 0);
    embedmq_post(q, "static.evt", NULL, 0);

    usleep(20000);
    assert(g_count == 2);

    embedmq_destroy(q);
    printf("  [PASS] test_static_mode\n");
}

static void test_queue_full(void)
{
    embedmq_config_t cfg = {
        .queue_size   = 64,
        .max_msg_size = 16,
        .max_handlers = 4,
    };
    size_t needed = embedmq_mem_size(&cfg);
    static uint8_t buf[512];
    embedmq_t *q = embedmq_create_static(buf, needed, &cfg);
    assert(q);

    embedmq_register(q, "flood", handler_count, NULL);
    uint32_t uuid = embedmq_uuid("flood");

    int full_seen = 0;
    for (int i = 0; i < 100; i++) {
        int r = embedmq_post_id(q, uuid, NULL, 0);
        if (r == EMBEDMQ_FULL) { full_seen = 1; break; }
    }
    assert(full_seen);

    embedmq_destroy(q);
    printf("  [PASS] test_queue_full\n");
}

/* ---- concurrent stress test ------------------------------------ */

#include <pthread.h>
#include <stdint.h>

#define STRESS_PRODUCERS  4
#define STRESS_MSGS_EACH  2000

typedef struct {
    embedmq_t *q;
    uint32_t   uuid;
} stress_arg_t;

static void *stress_producer(void *arg)
{
    stress_arg_t *a = (stress_arg_t *)arg;
    for (int i = 0; i < STRESS_MSGS_EACH; i++) {
        /* Retry on FULL — tiny queue in this test is intentional */
        while (embedmq_post_id(a->q, a->uuid, NULL, 0) == EMBEDMQ_FULL)
            usleep(100);
    }
    return NULL;
}

static void test_concurrent_producers(void)
{
    g_count = 0;

    embedmq_t *q = embedmq_create(NULL);
    assert(q);
    embedmq_register(q, "stress.evt", handler_count, NULL);

    uint32_t uuid = embedmq_uuid("stress.evt");

    pthread_t threads[STRESS_PRODUCERS];
    stress_arg_t args[STRESS_PRODUCERS];

    for (int i = 0; i < STRESS_PRODUCERS; i++) {
        args[i].q    = q;
        args[i].uuid = uuid;
        pthread_create(&threads[i], NULL, stress_producer, &args[i]);
    }
    for (int i = 0; i < STRESS_PRODUCERS; i++)
        pthread_join(threads[i], NULL);

    /* Give consumer thread time to drain the remaining queue */
    usleep(100000);

    int expected = STRESS_PRODUCERS * STRESS_MSGS_EACH;
    assert(g_count == expected);

    embedmq_destroy(q);
    printf("  [PASS] test_concurrent_producers (%d msgs from %d threads)\n",
           expected, STRESS_PRODUCERS);
}

/* ---- main ------------------------------------------------------ */

int main(void)
{
    printf("embedmq test suite\n");
    test_uuid_deterministic();
    test_create_destroy();
    test_register_and_post();
    test_payload_received();
    test_post_id_hot_path();
    test_static_mode();
    test_queue_full();
    test_concurrent_producers();
    printf("All tests passed.\n");
    return 0;
}
