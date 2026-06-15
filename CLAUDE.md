# embedmq — Lightweight Message Dispatch Library for Embedded Linux & RTOS

## Project Overview

**embedmq** is a zero-dependency C message dispatch library designed for embedded Linux and RTOS. Core mechanism: UUID dispatch + ring buffer + semaphore wakeup for asynchronous inter-thread communication.

**Author**: chenhuisheng1 (w4ysonch)
**Language**: C (C11), optional C++ wrapper planned
**License**: MIT

---

## Problem Statement

Embedded Linux / RTOS projects need inter-thread message passing, but existing solutions are either too heavy (DBus requires daemon + glib) or too primitive (hand-rolled queues lack thread safety and unified dispatch).

| | embedmq | DBus | ZeroMQ | hand-rolled |
|---|---|---|---|---|
| Dependencies | none | daemon + glib | libzmq | none |
| Platform | Linux / RTOS / bare-metal | Linux only | all | all |
| Zero heap | ✅ static mode | ❌ | ❌ | manual |
| API surface | 4 functions | very complex | medium | — |
| UUID dispatch | ✅ integer compare | ❌ string | ❌ | — |

---

## Core Design

Three components:

### 1. Registry
`name` is hashed to a `uint32_t` UUID at registration time. Runtime dispatch compares integers only — no string matching on the hot path.

```c
embedmq_register(q, "battery.changed", on_battery_changed, user_data);
```

### 2. Ring Queue
Circular buffer with mutex-protected writes (multi-producer safe) and lock-free reads (single consumer). Supports two allocation modes:
- **Dynamic mode**: internal malloc
- **Static mode**: caller provides buffer (zero malloc, suitable for bare-metal)

Wire format per message:
```
[ uint32_t uuid ][ uint16_t length ][ payload bytes ]
```
Messages may wrap around the buffer end; handled transparently.

### 3. Dispatcher
An internal consumer thread runs `while(1)`, blocking on a semaphore. When woken, it drains the queue and dispatches each message by UUID binary search → handler call.

```
Producer thread(s):
  embedmq_post(q, "battery.changed", &info, sizeof(info))
    → hash name → uuid
    → mutex_lock → ring_write(uuid|len|payload) → mutex_unlock
    → semaphore_give()  — wakes consumer

Consumer thread (internal):
  semaphore_wait()  ← sleeps here when queue is empty
    → ring_read() → uuid + payload
    → binary_search(uuid) → handler
    → handler(data, size, user_data)
```

---

## API

```c
// Create dispatcher (starts consumer thread internally)
embedmq_t *embedmq_create(const embedmq_config_t *cfg);

// Static / zero-malloc mode — caller provides memory
embedmq_t *embedmq_create_static(void *mem, size_t mem_size,
                                  const embedmq_config_t *cfg);

// Compute required buffer size for static mode
size_t embedmq_mem_size(const embedmq_config_t *cfg);

// Register a handler (call before producers start)
int embedmq_register(embedmq_t *q, const char *name,
                     embedmq_handler_fn fn, void *ctx);

// Post an event (non-blocking, thread-safe)
int embedmq_post(embedmq_t *q, const char *name,
                 const void *data, size_t size);

// Post by UUID — hot-path variant, skips hash
int embedmq_post_id(embedmq_t *q, uint32_t uuid,
                    const void *data, size_t size);

// Compute UUID for a name (stateless pure hash)
uint32_t embedmq_uuid(const char *name);

// Drive dispatch manually — for no-OS / superloop mode
int embedmq_poll(embedmq_t *q);

// Destroy (waits for consumer thread to exit)
void embedmq_destroy(embedmq_t *q);
```

Handler signature:
```c
typedef void (*embedmq_handler_fn)(const void *data, size_t size, void *ctx);
```

---

## Platform Abstraction Layer (PAL)

All OS primitives go through the PAL to support Linux, FreeRTOS, and bare-metal:

```
pal/linux/embedmq_pal.c    → pthread_mutex, sem_t, pthread_create
pal/freertos/embedmq_pal.c → xSemaphore, xTaskCreate  (planned)
pal/none/embedmq_pal.c     → C11 atomic spinlock; no thread; use embedmq_poll()
```

---

## Directory Structure

```
embedmq/
├── include/
│   └── embedmq.h              ← public API
├── src/
│   ├── embedmq.c              ← core: create/register/post/destroy/poll
│   ├── embedmq_internal.h     ← internal structs and declarations
│   ├── embedmq_queue.c        ← ring buffer (wrap-safe read/write)
│   └── embedmq_hash.c         ← FNV-1a hash → uint32_t UUID
├── pal/
│   ├── embedmq_pal.h          ← PAL interface
│   ├── linux/                 ← pthread implementation
│   ├── freertos/              ← FreeRTOS (planned)
│   └── none/                  ← bare-metal spinlock + poll()
├── examples/
│   └── basic.c                ← minimal working example
├── tests/
│   └── test_embedmq.c         ← 8 test cases incl. concurrent stress test
├── CMakeLists.txt
├── README.md                  ← English, published on GitHub
├── README_CN.md               ← Chinese
└── CLAUDE.md                  ← project context for AI agent, not uploaded
```

---

## Key Design Decisions

**Why UUID instead of string dispatch?**
Hash once at registration, compare integers at runtime. Zero string operations on the hot path — every cycle matters on embedded devices.

**Why support zero malloc?**
Embedded devices (MCU and memory-constrained Linux) need to avoid heap fragmentation and non-deterministic latency. Static mode lets the caller provide a fixed-size buffer with fully controlled lifetime.

**Why semaphore instead of busy-wait?**
Busy-wait wastes CPU. The consumer thread blocks on the semaphore and only wakes when a producer signals — same pattern as FreeRTOS queues.

**Why not POSIX mqueue (mq_open)?**
POSIX mqueue is for inter-process communication with kernel overhead and fixed capacity at creation time. embedmq is intra-process, lighter, and runtime-configurable.

---

## Build & Test

```bash
# Linux
mkdir build && cd build
cmake .. && make

# Run tests
./test_embedmq

# No-OS / bare-metal PAL
cmake .. -DEMBEDMQ_PAL=none && make
```

---

## Progress

- [x] Ring buffer (static + dynamic)
- [x] UUID hash registry
- [x] Linux PAL (pthreads)
- [x] Bare-metal PAL (C11 atomics + poll())
- [x] Core API (create / register / post / post_id / poll / destroy)
- [x] Basic example
- [x] Concurrent stress test (4 producers × 2000 msgs)
- [x] README (EN + CN)
- [x] CI (GitHub Actions — gcc + clang)
- [x] Benchmark (3M msgs/sec throughput, ~38µs latency)
- [x] C++ wrapper (lambda + RAII, header-only embedmq.hpp)
- [ ] FreeRTOS PAL
