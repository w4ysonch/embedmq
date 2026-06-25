/*
 * basic.c — minimal embedmq example (C API)
 *
 * Demonstrates the three-step pattern:
 *   1. embedmq_create()   — start the dispatcher
 *   2. embedmq_register() — bind a name to a handler function
 *   3. embedmq_post()     — send a message from any thread
 *
 * Also shows the hot-path variant: cache the UUID once with
 * embedmq_uuid() and call embedmq_post_id() to skip the hash
 * on every post.
 *
 * Expected output:
 *   [handler] battery: level=85% voltage=4.05V
 *   [handler] battery: level=20% voltage=3.55V
 *   [handler] battery: level=5% voltage=3.20V
 *   done
 *
 * Build (from repo root):
 *   cmake -B build && cmake --build build && ./build/example_basic
 */

#include <stdio.h>
#include <unistd.h>
#include "embedmq.h"

typedef struct {
    int   level;
    float voltage;
} battery_info_t;

static void on_battery_changed(const void *data, size_t size, void *ctx)
{
    const battery_info_t *info = (const battery_info_t *)data;
    printf("[handler] battery: level=%d%% voltage=%.2fV\n",
           info->level, info->voltage);
}

int main(void)
{
    /* Create dispatcher with default config */
    embedmq_t *q = embedmq_create(NULL);
    if (!q) {
        fprintf(stderr, "embedmq_create failed\n");
        return 1;
    }

    /* Register handler */
    embedmq_register(q, "battery.changed", on_battery_changed, NULL);

    /* Post events from the main thread */
    battery_info_t info = { .level = 85, .voltage = 4.05f };
    embedmq_post(q, "battery.changed", &info, sizeof(info));

    info.level   = 20;
    info.voltage = 3.55f;
    embedmq_post(q, "battery.changed", &info, sizeof(info));

    /* Hot-path variant: cache UUID, skip hash on each post */
    uint32_t uuid = embedmq_uuid("battery.changed");
    info.level   = 5;
    info.voltage  = 3.20f;
    embedmq_post_id(q, uuid, &info, sizeof(info));

    /* Give consumer thread time to drain the queue */
    usleep(10000);

    embedmq_destroy(q);
    printf("done\n");
    return 0;
}
