# embedmq API Reference

**[中文 ->](API_CN.md)**

Complete reference for the public C API declared in `include/embedmq.h`.
For the C++ wrapper, see [the C++ section](#c-wrapper) at the end.

- [Concepts](#concepts)
- [Types](#types)
- [Return codes](#return-codes)
- [Configuration](#configuration)
- [Functions](#functions)
  - [embedmq_create](#embedmq_create)
  - [embedmq_create_static](#embedmq_create_static)
  - [embedmq_mem_size](#embedmq_mem_size)
  - [embedmq_register](#embedmq_register)
  - [embedmq_post](#embedmq_post)
  - [embedmq_post_id](#embedmq_post_id)
  - [embedmq_uuid](#embedmq_uuid)
  - [embedmq_poll](#embedmq_poll)
  - [embedmq_destroy](#embedmq_destroy)
- [Lifecycle & threading rules](#lifecycle--threading-rules)
- [C++ wrapper](#c-wrapper)

---

## Concepts

embedmq dispatches messages **within a single process**, between threads.

- **register** — a string `name` is hashed once to a `uint32_t` UUID and bound to a handler.
- **post** — a payload is tagged with a UUID, written to a ring buffer, and the consumer is signalled.
- **dispatch** — an internal consumer thread wakes, looks up the UUID, and calls the handler.

Runtime dispatch compares integers, not strings — there are no string operations on the hot path.

---

## Types

### `embedmq_t`

```c
typedef struct embedmq_s embedmq_t;
```

Opaque dispatcher handle. Internals are platform-specific and must not be accessed directly.
Obtain one from `embedmq_create()` or `embedmq_create_static()`.

### `embedmq_handler_fn`

```c
typedef void (*embedmq_handler_fn)(const void *data, size_t size, void *ctx);
```

Handler callback, invoked from the internal consumer thread.

| Parameter | Description |
|---|---|
| `data` | Pointer to the payload. **Valid only for the duration of the call** — copy it if you need to keep it. |
| `size` | Payload size in bytes. |
| `ctx`  | The user context pointer passed to `embedmq_register()`. |

> The handler runs on the consumer thread, not the posting thread. Keep it short; offload heavy work.

---

## Return codes

| Macro | Value | Meaning |
|---|---|---|
| `EMBEDMQ_OK`    | `0`  | Success |
| `EMBEDMQ_ERR`   | `-1` | Generic error |
| `EMBEDMQ_FULL`  | `-2` | Ring buffer full — message dropped |
| `EMBEDMQ_NOMEM` | `-3` | Out of memory |
| `EMBEDMQ_EXIST` | `-4` | A handler is already registered for this name |
| `EMBEDMQ_INVAL` | `-5` | Invalid argument (e.g. `NULL` handle, payload exceeds `max_msg_size`) |

---

## Configuration

```c
typedef struct {
    size_t queue_size;      /* ring buffer bytes,     default: 8192     */
    size_t max_msg_size;    /* max payload per msg,   default: 1024     */
    size_t max_handlers;    /* max registered names,  default: 64       */
    int    thread_priority; /* consumer thread prio,  0 = OS default    */
} embedmq_config_t;
```

| Field | Default | Notes |
|---|---|---|
| `queue_size` | 8192 | Total ring buffer size in bytes. Holds many small messages or fewer large ones. Per-message overhead is 6 bytes (`uint32_t` UUID + `uint16_t` length). |
| `max_msg_size` | 1024 | Upper bound on a single payload. A `post()` with `size` greater than this returns `EMBEDMQ_INVAL`. |
| `max_handlers` | 64 | Maximum number of distinct names that can be registered. |
| `thread_priority` | 0 | Consumer thread priority. `0` means OS default. Ignored in no-OS builds. |

Pass `NULL` to `embedmq_create()` to use all defaults. For `embedmq_create_static()` the config must not be `NULL` — set `queue_size` and `max_handlers` explicitly so the buffer size is deterministic.

---

## Functions

### `embedmq_create`

```c
embedmq_t *embedmq_create(const embedmq_config_t *cfg);
```

Create a dispatcher in **dynamic (malloc) mode** and start the internal consumer thread.

| Parameter | Description |
|---|---|
| `cfg` | Configuration, or `NULL` for all defaults. |

**Returns:** a valid handle on success, or `NULL` on allocation/thread-creation failure.

**Threading:** call once during startup. The returned handle may then be shared across threads for posting.

```c
embedmq_t *q = embedmq_create(NULL);
if (!q) { /* handle error */ }
```

---

### `embedmq_create_static`

```c
embedmq_t *embedmq_create_static(void *mem, size_t mem_size,
                                 const embedmq_config_t *cfg);
```

Create a dispatcher in **static (zero-malloc) mode**. All internal state lives inside the
caller-provided buffer — no heap allocation occurs after this call. Suitable for MCU / RTOS
targets that avoid dynamic memory.

| Parameter | Description |
|---|---|
| `mem` | Caller-provided buffer, pointer-size aligned. |
| `mem_size` | Size of `mem` in bytes — must be at least `embedmq_mem_size(cfg)`. |
| `cfg` | **Must not be `NULL`.** Set `queue_size` and `max_handlers` explicitly. |

**Returns:** a handle (equal to `mem`) on success, or `NULL` if the buffer is too small or arguments are invalid.

```c
static embedmq_config_t cfg = { .queue_size = 4096, .max_handlers = 16 };
static uint8_t buf[8192];                       /* sized via embedmq_mem_size() */
embedmq_t *q = embedmq_create_static(buf, sizeof(buf), &cfg);
```

> In no-OS builds (`EMBEDMQ_PAL_NONE`) no consumer thread is created; drive dispatch with `embedmq_poll()`.

---

### `embedmq_mem_size`

```c
size_t embedmq_mem_size(const embedmq_config_t *cfg);
```

Compute the buffer size required by `embedmq_create_static()` for a given config.
Call with the **same** `cfg` you will pass to `embedmq_create_static()`.

| Parameter | Description |
|---|---|
| `cfg` | The configuration whose required size you want to compute. |

**Returns:** the required buffer size in bytes.

```c
static embedmq_config_t cfg = { .queue_size = 4096, .max_handlers = 16 };
size_t needed = embedmq_mem_size(&cfg);
uint8_t *buf = my_static_alloc(needed);
embedmq_t *q = embedmq_create_static(buf, needed, &cfg);
```

---

### `embedmq_register`

```c
int embedmq_register(embedmq_t *q, const char *name,
                     embedmq_handler_fn fn, void *ctx);
```

Bind a `name` to a handler. The name is hashed to a UUID once at call time; later posts using
the same name (or the equivalent UUID) wake this handler.

| Parameter | Description |
|---|---|
| `q` | Dispatcher handle. |
| `name` | Event name. Hashed with FNV-1a to a `uint32_t`. |
| `fn` | Handler callback. |
| `ctx` | User context, passed back to the handler on every dispatch. May be `NULL`. |

**Returns:** `EMBEDMQ_OK`, `EMBEDMQ_EXIST` (name already registered), or `EMBEDMQ_ERR`
(handler table full / invalid argument).

> **Must be called before any producer calls `embedmq_post()`.** Not thread-safe with respect to
> other `embedmq_register()` calls — do all registration during single-threaded startup.

```c
embedmq_register(q, "battery.changed", on_battery, &app_state);
```

---

### `embedmq_post`

```c
int embedmq_post(embedmq_t *q, const char *name,
                 const void *data, size_t size);
```

Enqueue an event by name. Non-blocking and thread-safe. Hashes `name` to a UUID, copies the
payload into the ring buffer, and signals the consumer thread.

| Parameter | Description |
|---|---|
| `q` | Dispatcher handle. |
| `name` | Event name (same string used at registration). |
| `data` | Pointer to payload, or `NULL` for a zero-length notification. |
| `size` | Payload size in bytes. Must be `<= max_msg_size`. |

**Returns:** `EMBEDMQ_OK`, `EMBEDMQ_FULL` (ring buffer has no room — message dropped, caller decides whether to retry), or `EMBEDMQ_INVAL`.

> The payload is **copied** into the queue, so `data` need not outlive the call.
> Posting a name that no handler registered is not an error — the message is simply dispatched to nobody.

```c
battery_t b = { .level = 85 };
embedmq_post(q, "battery.changed", &b, sizeof(b));
embedmq_post(q, "shutter.pressed", NULL, 0);   /* zero-length event */
```

---

### `embedmq_post_id`

```c
int embedmq_post_id(embedmq_t *q, uint32_t uuid,
                    const void *data, size_t size);
```

Hot-path variant of `embedmq_post()` that takes a precomputed UUID and skips the name hash.
Obtain the UUID once with `embedmq_uuid()` and cache it.

| Parameter | Description |
|---|---|
| `q` | Dispatcher handle. |
| `uuid` | Precomputed UUID from `embedmq_uuid()`. |
| `data` | Pointer to payload, or `NULL`. |
| `size` | Payload size in bytes. Must be `<= max_msg_size`. |

**Returns:** `EMBEDMQ_OK`, `EMBEDMQ_FULL`, or `EMBEDMQ_INVAL`.

```c
uint32_t id = embedmq_uuid("touch.point");      /* once */
while (reading) {
    embedmq_post_id(q, id, &pt, sizeof(pt));     /* tight loop, no hashing */
}
```

---

### `embedmq_uuid`

```c
uint32_t embedmq_uuid(const char *name);
```

Compute the UUID for a name. Stateless pure hash (FNV-1a) — returns the same value used
internally by `embedmq_register()` and `embedmq_post()`. Safe to call from any context, including
before any dispatcher exists.

| Parameter | Description |
|---|---|
| `name` | Event name. |

**Returns:** the `uint32_t` UUID. (The implementation maps a `0` result to `1` so `0` can be used as a sentinel.)

```c
uint32_t id = embedmq_uuid("battery.changed");
```

---

### `embedmq_poll`

```c
int embedmq_poll(embedmq_t *q);
```

Manually drain the ring buffer and invoke handlers for all pending messages.

In normal builds (Linux / RTOS with a real thread) the internal consumer thread calls this
automatically — **you do not need it.** It exists for bare-metal / no-OS builds
(`EMBEDMQ_PAL_NONE`), where no thread is created: call `embedmq_poll()` from your superloop or a
periodic task.

| Parameter | Description |
|---|---|
| `q` | Dispatcher handle. |

**Returns:** the number of messages dispatched (`0` if the queue was empty).

```c
/* no-OS superloop */
while (1) {
    read_inputs_and_post(q);
    embedmq_poll(q);          /* dispatch everything queued this tick */
    wait_for_next_tick();
}
```

---

### `embedmq_destroy`

```c
void embedmq_destroy(embedmq_t *q);
```

Stop the consumer thread and release resources. Blocks until the consumer thread has exited
cleanly, then frees all memory (dynamic mode) or zeroes the handle (static mode — the
caller's buffer itself is not freed).

| Parameter | Description |
|---|---|
| `q` | Dispatcher handle. Passing `NULL` is a no-op. |

```c
embedmq_destroy(q);
```

> After destroy, the handle is invalid. In static mode the buffer may be reused for a new
> `embedmq_create_static()`.

---

## Lifecycle & threading rules

1. **Create once**, during startup: `embedmq_create()` / `embedmq_create_static()`.
2. **Register all handlers next**, still single-threaded. `embedmq_register()` is *not* safe to
   call concurrently with itself or with posting.
3. **Start producers.** `embedmq_post()` / `embedmq_post_id()` are thread-safe and may be called
   from any number of threads simultaneously.
4. Handlers run on the **consumer thread**, one at a time, in FIFO order. The `data` pointer is
   valid only during the handler call.
5. **Destroy once**, when shutting down: `embedmq_destroy()` joins the consumer thread.

| Function | Thread-safe? | When to call |
|---|---|---|
| `embedmq_create*` | — | startup, once |
| `embedmq_register` | ❌ no | startup, before any post |
| `embedmq_post` / `embedmq_post_id` | ✅ yes | any time, any thread |
| `embedmq_uuid` | ✅ yes (stateless) | any time |
| `embedmq_poll` | ❌ single caller | no-OS mode only |
| `embedmq_destroy` | — | shutdown, once |

---

## C++ wrapper

A header-only RAII wrapper is provided in `include/embedmq.hpp` (namespace `embedmq`).

```cpp
#include "embedmq.hpp"

embedmq::MQ q;                                  // default config, RAII-managed

q.subscribe("battery.changed", [](const void *data, size_t size) {
    const auto *b = static_cast<const battery_t *>(data);
    printf("level=%d\n", b->level);
});

q.publish("battery.changed", &b, sizeof(b));

uint32_t id = embedmq::MQ::uuid("touch.point"); // hot-path
q.publish_id(id, &pt, sizeof(pt));
```

| Method | Maps to | Notes |
|---|---|---|
| `MQ()` / `MQ(cfg)` / `MQ(mem, size, cfg)` | `embedmq_create*` | Throws `std::runtime_error` on failure. |
| `subscribe(name, Handler)` | `embedmq_register` | `Handler` is `std::function<void(const void*, size_t)>`; supports capturing lambdas. |
| `publish(name, data, size)` | `embedmq_post` | |
| `publish_id(uuid, data, size)` | `embedmq_post_id` | |
| `static uuid(name)` | `embedmq_uuid` | |
| `poll()` | `embedmq_poll` | |
| `native()` | — | Returns the underlying `embedmq_t*`. |
| `~MQ()` | `embedmq_destroy` | Automatic (RAII). Movable, non-copyable. |

> The wrapper stores each `Handler` for the lifetime of the `MQ` instance and bridges it to the
> C handler via a static trampoline. Do not let an `MQ` outlive handlers that capture references
> to objects with shorter lifetimes.
