<div align="center">

# embedmq

### A single API for thread-to-thread message dispatch — across Linux threads, RTOS tasks, and bare-metal superloops

[![Version](https://img.shields.io/github/v/release/w4ysonch/embedmq?color=blue&label=version)](https://github.com/w4ysonch/embedmq/releases)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20RTOS%20%7C%20bare--metal-lightgrey.svg)](#platform-support)
![CI](https://github.com/w4ysonch/embedmq/actions/workflows/ci.yml/badge.svg)

**[中文 ->](README_CN.md)**

---

</div>

## Why embedmq?

In embedded systems, threads need to notify each other constantly: a sensor reads new data, WiFi connects, a button is pressed, a timer fires. With FreeRTOS, you end up hand-rolling a Queue struct for every pair of tasks. On bare metal, your superloop drowns in `if (flag1) ... if (flag2) ...`. On Linux, modules hold direct pointers to each other and a change in one ripples everywhere.

embedmq collapses all of this into 3 functions — `create`, `register`, `post`. You decide **who posts what** and **who handles what**. The library handles the queue, the mutex, the semaphore, and the dispatch.

- **Linux threads, no need for DBus** — Hardware monitor thread, network thread, config thread — each posts its own events, registers for the ones it cares about. One header + one PAL file, link `-lpthread`. Zero external deps, no heavy framework just for thread comms.

- **FreeRTOS tasks, no more hand-rolled Queues** — Your sensor task does one thing: `post("sensor.temp", &data, sizeof(data))`. Your UI task does one thing: `register("sensor.temp", on_temp, NULL)`. Neither knows the other's TaskHandle or queue handle.

- **Bare-metal superloops, no more `if (flag)` sprawl** — Timer ISR posts `"tick.10ms"`, button ISR posts `"button.press"`, ADC callback posts `"adc.done"`. Your main loop calls `embedmq_poll(q)` once to dispatch everything. No new flag → no new if-block in main.

- **One API, three platforms** — Linux uses pthread + POSIX semaphore. FreeRTOS uses counting semaphore + Task. Bare metal uses C11 atomic lock + `poll()`. Switch platforms by swapping one PAL file. Core code, public header, and calling convention stay identical.

- **Hash once, zero string ops at runtime** — A name is FNV-1a hashed to a `uint32_t` UUID at registration and inserted into a sorted array. Dispatch binary-searches integers. Cache the UUID and call `post_id()` in tight loops — the hot path has no string comparison, one semaphore op, one memcpy into the ring buffer.

- **Zero malloc after init** — `create_static()` places all internal state inside a caller-provided BSS buffer. After creation, the heap is untouched. Built for MCUs that forbid dynamic allocation past startup.

| vs | **embedmq** | DBus | ZeroMQ | hand-rolled queue |
|---|---|---|---|---|
| Dependencies | none | daemon + glib | libzmq | none |
| Scope | intra-process threads | cross-process | cross-network | intra-process |
| Platform | Linux / RTOS / bare-metal | Linux only | all | all |
| Runtime dispatch | **integer compare** | string | string | manual |
| Thread-safe post | ✅ built-in | ✅ | ✅ | manual |
| Zero heap | ✅ | ❌ | ❌ | manual |

embedmq is for **intra-process**, thread-to-thread message dispatch. It is not for cross-process IPC, network RPC, or single-threaded synchronous code.

---

## Quick start

```c
embedmq_t *q = embedmq_create(NULL);
embedmq_register(q, "battery.changed", on_battery, NULL);
embedmq_post(q, "battery.changed", &info, sizeof(info));
```

### Dynamic mode (Linux / heap available)

```c
#include "embedmq.h"

typedef struct { int level; float voltage; } battery_t;

static void on_battery(const void *data, size_t size, void *ctx)
{
    const battery_t *b = data;
    printf("battery: %d%% %.2fV\n", b->level, b->voltage);
}

int main(void)
{
    embedmq_t *q = embedmq_create(NULL);  /* NULL = use defaults */
    embedmq_register(q, "battery.changed", on_battery, NULL);

    battery_t b = { .level = 85, .voltage = 4.05f };
    embedmq_post(q, "battery.changed", &b, sizeof(b));

    sleep(1);
    embedmq_destroy(q);
}
```

### Static mode (zero heap — MCU / RTOS)

```c
#include "embedmq.h"

static embedmq_config_t cfg = {
    .queue_size   = 2048,
    .max_msg_size = 64,
    .max_handlers = 8,
};

static uint8_t mq_buf[4096]; /* size with embedmq_mem_size(&cfg) */
static embedmq_t *q;

void app_init(void)
{
    q = embedmq_create_static(mq_buf, sizeof(mq_buf), &cfg);
    embedmq_register(q, "sensor.update", on_sensor, NULL);
}

/* Thread-safe: call from any task */
void sensor_task(void)
{
    sensor_data_t d = read_sensor();
    embedmq_post(q, "sensor.update", &d, sizeof(d));
}
```

### Hot-path variant (skip the hash on every post)

```c
/* Hash once at startup, cache the UUID */
uint32_t uuid = embedmq_uuid("touch.point");

/* Tight loop — no string hashing */
while (reading) {
    touch_t t = read_touch();
    embedmq_post_id(q, uuid, &t, sizeof(t));
}
```

### No-OS / bare-metal (superloop-driven)

```c
/* Build with -DEMBEDMQ_PAL_NONE */
embedmq_t *q = embedmq_create(&cfg);
embedmq_register(q, "tick", on_tick, NULL);

while (1) {
    embedmq_post(q, "tick", NULL, 0);
    embedmq_poll(q);   /* drain all pending messages */
    sleep_until_next_tick();
}
```

### C++ wrapper (lambda + RAII)

```cpp
#include "embedmq.hpp"

embedmq::MQ q;

q.subscribe("battery.changed", [](const void *data, size_t size) {
    const auto *b = static_cast<const BatteryInfo *>(data);
    printf("level=%d\n", b->level);
});

q.publish("battery.changed", &info, sizeof(info));

// Hot path: cache UUID
uint32_t uuid = embedmq::MQ::uuid("battery.changed");
q.publish_id(uuid, &info, sizeof(info));

// Destructor calls embedmq_destroy() automatically (RAII)
```

---

## Performance

Measured on x86-64 Linux, Release build, single producer + consumer:

| Benchmark | Result |
|---|---|
| `embedmq_post()` throughput | **~3.0M msgs/sec** |
| `embedmq_post_id()` throughput (UUID cached) | **~3.0M msgs/sec** |
| End-to-end latency avg (post → handler) | **~38 µs** |
| End-to-end latency min | **~7 µs** |
| `embedmq_uuid()` hash speed | **~45M hashes/sec** (~22 ns/hash) |

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/benchmark
```

---

## API overview

```c
/* Create / destroy */
embedmq_t *embedmq_create(const embedmq_config_t *cfg);
embedmq_t *embedmq_create_static(void *mem, size_t mem_size, const embedmq_config_t *cfg);
size_t     embedmq_mem_size(const embedmq_config_t *cfg);
void       embedmq_destroy(embedmq_t *q);

/* Register handlers (do this at startup, before any posts) */
int embedmq_register(embedmq_t *q, const char *name, embedmq_handler_fn fn, void *ctx);

/* Post messages (thread-safe, non-blocking, any thread) */
int embedmq_post(embedmq_t *q, const char *name, const void *data, size_t size);
int embedmq_post_id(embedmq_t *q, uint32_t uuid, const void *data, size_t size);

/* Utilities */
uint32_t embedmq_uuid(const char *name);  /* stateless pure hash */
int      embedmq_poll(embedmq_t *q);      /* bare-metal superloop driver */
```

**Return codes:** `EMBEDMQ_OK (0)` · `EMBEDMQ_FULL (-2)` · `EMBEDMQ_EXIST (-4)` · `EMBEDMQ_INVAL (-5)`

**Configuration** (`embedmq_config_t`, pass `NULL` for defaults):

| Field | Default | Description |
|---|---|---|
| `queue_size` | 8192 | Total ring buffer bytes |
| `max_msg_size` | 1024 | Max payload per message |
| `max_handlers` | 64 | Max registered event names |
| `thread_priority` | 0 | Consumer thread priority (0 = OS default) |

> Full API docs (threading rules, lifecycle): **[docs/API.md](docs/API.md)** · Design notes: **[docs/DESIGN.md](docs/DESIGN.md)**

---

## Building

```bash
# Linux (default)
cmake -B build && cmake --build build && ./build/test_embedmq

# Bare-metal / no-OS
cmake -B build-none -DEMBEDMQ_PAL=none -DEMBEDMQ_BUILD_TESTS=OFF && cmake --build build-none

# FreeRTOS POSIX simulator test (auto-fetches FreeRTOS-Kernel)
cmake -B build-frt -DEMBEDMQ_BUILD_FREERTOS_SIM=ON && cmake --build build-frt
./build-frt/sim/freertos/test_embedmq_freertos
```

Without CMake: copy `src/embedmq.c`, `src/embedmq_hash.c`, `src/embedmq_queue.c`, and the matching `pal/<xxx>/embedmq_pal.c` into your project. Add `-Iinclude -Ipal` to compiler flags. Link `-lpthread` for the Linux PAL.

> Full build guide: **[docs/BUILD.md](docs/BUILD.md)**

---

## Platform support

| PAL | File | Notes |
|---|---|---|
| Linux | `pal/linux/embedmq_pal.c` | pthread + POSIX counting semaphore |
| FreeRTOS | `pal/freertos/embedmq_pal.c` | counting semaphore + task; continuously verified on the POSIX simulator in CI |
| Bare-metal | `pal/none/embedmq_pal.c` | C11 atomic spinlock; drive with `embedmq_poll()` |

> The FreeRTOS PAL verification covers semaphore wakeup, mutex, task create/exit, and dispatch correctness. It does not exercise real-hardware timing or ISR context.

---

## License

MIT
