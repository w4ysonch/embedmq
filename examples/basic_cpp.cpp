#include <cstdio>
#include <unistd.h>
#include "embedmq.hpp"

struct BatteryInfo {
    int   level;
    float voltage;
};

int main()
{
    embedmq::MQ q;

    /* Lambda with capture */
    int received = 0;
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

    usleep(20000);
    printf("received %d messages\n", received);

    /* q.~MQ() calls embedmq_destroy() automatically */
    return 0;
}
