# embedmq

**[中文 ->](README_CN.md)**

![CI](https://github.com/w4ysonch/embedmq/actions/workflows/ci.yml/badge.svg)

Lightweight, zero-dependency message dispatch library for embedded Linux and RTOS.

**3 core functions. Zero heap in static mode. UUID dispatch — no string comparison at runtime.**

```c
embedmq_t *q = embedmq_create(NULL);
embedmq_register(q, "battery.changed", on_battery, NULL);
embedmq_post(q, "battery.changed", &info, sizeof(info));
```

---

## Why embedmq?

| | **embedmq** | DBus | ZeroMQ | hand-rolled queue |
|---|---|---|---|---|
| Dependencies | none | daemon + glib | libzmq | none |
| Platform | Linux / RTOS / bare-metal | Linux only | all | all |
| Zero heap | ✅ static mode | ❌ | ❌ | manual |
| Runtime dispatch | integer compare | string | string | manual |
| Thread-safe post | ✅ | ✅ | ✅ | manual |

embedmq is for **intra-process** thread-to-thread communication. It is not a network or IPC library.

---

## How it works

```
Producer thread(s)                Consumer thread (internal)
─────────────────                 ──────────────────────────
embedmq_post(q, name, data, n)    sem_wait()  ← sleeps when queue is empty
  │                                 │
  ├─ hash(name) → uuid             ├─ ring_read() → uuid + payload
  ├─ mutex_lock                    ├─ binary_search(uuid) → handler
  ├─ ring_write(uuid|len|payload)  └─ handler(data, size, ctx)
  ├─ mutex_unlock
  └─ sem_post  ──────────────────▶ wakes consumer
```

- **Registry**: `name` is hashed to a `uint32_t` UUID at registration time. Runtime dispatch compares integers — no string operations on the hot path.
- **Ring buffer**: lock-free read (single consumer), mutex-protected write (multiple producers). Messages may wrap around the buffer end transparently.
- **Semaphore**: consumer sleeps when the queue is empty; each `post()` wakes it once.

---

## Quick start

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

/* Place in BSS — no dynamic allocation */
static uint8_t mq_buf[4096]; /* use embedmq_mem_size(&cfg) to size exactly */
static embedmq_t *q;

void app_init(void)
{
    q = embedmq_create_static(mq_buf, sizeof(mq_buf), &cfg);
    embedmq_register(q, "sensor.update", on_sensor, NULL);
}

/* Call from any task / ISR-safe context */
void sensor_isr(void)
{
    sensor_data_t d = read_sensor();
    embedmq_post(q, "sensor.update", &d, sizeof(d));
}
```

### C++ wrapper (lambda + RAII)

```cpp
#include "embedmq.hpp"

embedmq::MQ q;

// Lambda with capture — no global function needed
int count = 0;
q.subscribe("battery.changed", [&count](const void *data, size_t size) {
    const auto *b = static_cast<const BatteryInfo *>(data);
    printf("level=%d\n", b->level);
    count++;
});

q.publish("battery.changed", &info, sizeof(info));

// Hot path: cache UUID once, publish by ID
uint32_t uuid = embedmq::MQ::uuid("battery.changed");
q.publish_id(uuid, &info, sizeof(info));

// q destructor calls embedmq_destroy() automatically (RAII)
```

### Hot-path variant (skip hash on every post)

```c
/* Cache UUID once at startup */
uint32_t uuid = embedmq_uuid("touch.point");

/* Tight loop — no string hash */
while (reading) {
    touch_t t = read_touch();
    embedmq_post_id(q, uuid, &t, sizeof(t));
}
```

### No-OS / bare-metal (superloop)

```c
/* Build with -DEMBEDMQ_PAL_NONE */
embedmq_t *q = embedmq_create(&cfg);
embedmq_register(q, "tick", on_tick, NULL);

while (1) {
    embedmq_post(q, "tick", NULL, 0);
    embedmq_poll(q);   /* dispatch all pending messages */
    sleep_until_next_tick();
}
```

---

## API reference

```c
/* Create / destroy */
embedmq_t *embedmq_create(const embedmq_config_t *cfg);
embedmq_t *embedmq_create_static(void *mem, size_t mem_size,
                                  const embedmq_config_t *cfg);
size_t     embedmq_mem_size(const embedmq_config_t *cfg);
void       embedmq_destroy(embedmq_t *q);

/* Register (call before producers start) */
int embedmq_register(embedmq_t *q, const char *name,
                     embedmq_handler_fn fn, void *ctx);

/* Post (thread-safe, non-blocking) */
int embedmq_post(embedmq_t *q, const char *name,
                 const void *data, size_t size);
int embedmq_post_id(embedmq_t *q, uint32_t uuid,         /* hot path */
                    const void *data, size_t size);

/* Utility */
uint32_t embedmq_uuid(const char *name);  /* stateless hash */
int      embedmq_poll(embedmq_t *q);      /* no-OS superloop driver */
```

**Handler signature:**
```c
typedef void (*embedmq_handler_fn)(const void *data, size_t size, void *ctx);
```

**Return codes:** `EMBEDMQ_OK (0)` · `EMBEDMQ_FULL (-2)` · `EMBEDMQ_EXIST (-4)` · `EMBEDMQ_INVAL (-5)`

**Configuration** (`embedmq_config_t`, pass `NULL` for defaults):

| Field | Default | Description |
|---|---|---|
| `queue_size` | 8192 | Ring buffer size in bytes |
| `max_msg_size` | 1024 | Max payload per message |
| `max_handlers` | 64 | Max registered event names |
| `thread_priority` | 0 | Consumer thread priority (0 = OS default) |

> Full per-function reference (parameters, return values, threading rules): **[docs/API.md](docs/API.md)**

---

## Performance

Measured on x86-64 Linux (Release build, single producer + consumer thread):

| Benchmark | Result |
|---|---|
| `embedmq_post()` throughput | **3.0M msgs/sec** |
| `embedmq_post_id()` throughput (UUID cached) | **3.0M msgs/sec** |
| End-to-end latency avg (post → handler) | **~38 µs** |
| End-to-end latency min | **~7 µs** |
| `embedmq_uuid()` hash speed | **45M hashes/sec** (~22 ns/hash) |

Run on your own hardware:
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make
./benchmark
```

---

## Building

```bash
# Linux (default)
mkdir build && cd build
cmake .. && make

# Run tests
./test_embedmq

# Static-only / no malloc (conceptual — set EMBEDMQ_STATIC_ONLY in your code)
cmake .. -DEMBEDMQ_PAL=none && make

# Specify PAL explicitly
cmake .. -DEMBEDMQ_PAL=linux && make    # pthreads (default)
cmake .. -DEMBEDMQ_PAL=none  && make    # no-OS spinlock
```

### Integrating without CMake

Copy these files into your project and compile together:

```
src/embedmq.c
src/embedmq_hash.c
src/embedmq_queue.c
pal/linux/embedmq_pal.c      ← or your target platform
```

Add `-Iinclude -Ipal` to your compiler flags.

---

## Platform support

| PAL | File | Notes |
|---|---|---|
| Linux | `pal/linux/embedmq_pal.c` | pthreads + POSIX semaphore |
| FreeRTOS | `pal/freertos/embedmq_pal.c` | planned |
| Bare-metal | `pal/none/embedmq_pal.c` | C11 atomics spinlock; use `embedmq_poll()` |

---

## License

MIT
