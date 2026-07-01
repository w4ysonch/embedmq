/*
 * basic_cpp.cpp — minimal embedmq example (C++ wrapper)
 *
 * Demonstrates the C++ wrapper (embedmq.hpp):
 *   - embedmq::MQ        RAII wrapper; destructor calls embedmq_destroy()
 *   - subscribe()        register a lambda (with capture) as a handler
 *   - publish()          send a message by name
 *   - publish_id()       hot-path variant with a cached UUID
 *
 * Expected output:
 *   [handler] battery: level=85% voltage=4.05V
 *   [handler] battery: level=20% voltage=3.55V
 *   received 2 messages
 *
 * Build (from repo root):
 *   cmake -B build && cmake --build build && ./build/example_basic_cpp
 */

#include <cstdio>
#include "embedmq.hpp"

struct BatteryInfo {
    int   level;
    float voltage;
};

int main()
{
    int received = 0;

    {
        embedmq::MQ q;

        /* Lambda with capture */
        q.subscribe("battery.changed", [&received](const void *data, size_t size) {
            const auto *b = static_cast<const BatteryInfo *>(data);
            printf("[handler] battery: level=%d%% voltage=%.2fV\n",
                   b->level, b->voltage);
            received++;
        });

        /* Publish by name */
        BatteryInfo info = { .level = 85, .voltage = 4.05f };
        q.publish("battery.changed", &info, sizeof(info));

        /* Hot-path: cache UUID, publish by ID */
        uint32_t uuid = embedmq::MQ::uuid("battery.changed");
        info = { .level = 20, .voltage = 3.55f };
        q.publish_id(uuid, &info, sizeof(info));

    } /* q.~MQ() drains queue and calls embedmq_destroy() */

    printf("received %d messages\n", received);
    return 0;
}
