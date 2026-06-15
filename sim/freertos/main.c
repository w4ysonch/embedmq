/*
 * embedmq — FreeRTOS PAL verification under the POSIX (GCC_POSIX) port.
 *
 * Runs the real embedmq core + FreeRTOS PAL on a host machine, no hardware.
 * The test logic mirrors a subset of tests/test_embedmq.c, but must run
 * inside a task because embedmq's consumer task only exists once the FreeRTOS
 * scheduler is running.
 *
 * Exit code 0 + "PASS" on success; non-zero + "FAIL" otherwise (for CI).
 */
#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

#include "embedmq.h"

/* ---------------------------------------------------------
 * Test assertion helper
 * --------------------------------------------------------- */
#define CHECK(cond, name)                               \
    do {                                                \
        if (!(cond)) {                                  \
            printf("FAIL: %s\n", (name));               \
            fflush(stdout);                             \
            exit(1);                                    \
        }                                               \
        printf("  ok: %s\n", (name));                   \
        fflush(stdout);                                 \
    } while (0)

/* ---------------------------------------------------------
 * Shared test state (single-core FreeRTOS: only one task runs at a
 * time, so plain volatile counters are safe across tasks)
 * --------------------------------------------------------- */
typedef struct { int seq; } msg_t;

static volatile int g_received   = 0;
static volatile int g_last_value = 0;

static void on_event(const void *data, size_t size, void *ctx)
{
    (void)ctx;
    if (size == sizeof(msg_t))
        g_last_value = ((const msg_t *)data)->seq;
    g_received++;
}

/* ---------------------------------------------------------
 * Concurrent producers
 * --------------------------------------------------------- */
#define N_PRODUCERS      3
#define PER_PRODUCER     100

static embedmq_t       *g_q = NULL;
static volatile int     g_producers_done = 0;

static void producer_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < PER_PRODUCER; i++) {
        msg_t m = { .seq = i };
        embedmq_post(g_q, "sensor.update", &m, sizeof(m));
    }
    g_producers_done++;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------
 * Main test task
 * --------------------------------------------------------- */
static void test_task(void *arg)
{
    (void)arg;
    int r;

    printf("== embedmq FreeRTOS (POSIX) simulator test ==\n");

    /* ---- create / register ---- */
    embedmq_t *q = embedmq_create(NULL);
    CHECK(q != NULL, "embedmq_create starts consumer task");

    r = embedmq_register(q, "sensor.update", on_event, NULL);
    CHECK(r == EMBEDMQ_OK, "embedmq_register");

    /* ---- 1) basic post by name ---- */
    const int N = 100;
    for (int i = 1; i <= N; i++) {
        msg_t m = { .seq = i };
        r = embedmq_post(q, "sensor.update", &m, sizeof(m));
        CHECK(r == EMBEDMQ_OK, "embedmq_post accepted");
    }
    vTaskDelay(pdMS_TO_TICKS(200));   /* let consumer drain */
    CHECK(g_received == N,   "all posted messages dispatched");
    CHECK(g_last_value == N, "payload delivered intact (last seq)");

    /* ---- 2) hot-path post_id ---- */
    g_received = 0;
    uint32_t uuid = embedmq_uuid("sensor.update");
    for (int i = 0; i < 50; i++) {
        msg_t m = { .seq = i };
        embedmq_post_id(q, uuid, &m, sizeof(m));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    CHECK(g_received == 50, "post_id messages dispatched");

    /* ---- 3) concurrent producers ---- */
    g_received       = 0;
    g_producers_done = 0;
    g_q              = q;
    for (int i = 0; i < N_PRODUCERS; i++) {
        xTaskCreate(producer_task, "prod", configMINIMAL_STACK_SIZE * 2,
                    NULL, tskIDLE_PRIORITY + 1, NULL);
    }
    /* wait for producers, then for the consumer to drain */
    while (g_producers_done < N_PRODUCERS)
        vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(300));
    CHECK(g_received == N_PRODUCERS * PER_PRODUCER,
          "concurrent producers: all messages dispatched");

    /* ---- 4) destroy joins the consumer task cleanly ---- */
    embedmq_destroy(q);
    CHECK(1, "embedmq_destroy joined consumer task without hang");

    printf("PASS\n");
    fflush(stdout);
    exit(0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: readable CI logs */

    /* test_task runs above the consumer/producer tasks so its vTaskDelay
     * windows let the lower-priority workers run. */
    xTaskCreate(test_task, "test", configMINIMAL_STACK_SIZE * 8,
                NULL, tskIDLE_PRIORITY + 2, NULL);

    vTaskStartScheduler();

    /* Only reached if the scheduler could not start (e.g. out of heap). */
    printf("FAIL: scheduler did not start\n");
    return 1;
}
