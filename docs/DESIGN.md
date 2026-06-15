# embedmq Design Notes

**[中文 ->](DESIGN_CN.md)**

Why embedmq is built the way it is — the decisions, the trade-offs, and the
things that bit me along the way. If `README.md` is *what* and `API.md` is
*how*, this is *why*.

- [The problem](#the-problem)
- [Architecture](#architecture)
- [Design decisions](#design-decisions)
  - [UUID dispatch instead of string matching](#1-uuid-dispatch-instead-of-string-matching)
  - [Zero-malloc static mode](#2-zero-malloc-static-mode)
  - [Semaphore wakeup instead of busy-wait](#3-semaphore-wakeup-instead-of-busy-wait)
  - [Ring buffer wrap handling](#4-ring-buffer-wrap-handling)
  - [PAL, not HAL](#5-pal-not-hal)
- [Porting to FreeRTOS: the war story](#porting-to-freertos-the-war-story)
- [Performance](#performance)

---

## The problem

Embedded Linux and RTOS projects almost always need **intra-process,
thread-to-thread** communication: a sensor thread produces data, a UI thread
and a control thread want to react to it, and you don't want every module
holding direct pointers to every other module.

The existing options didn't fit:

| Option | Why it doesn't fit |
|---|---|
| **DBus** | Needs a daemon + glib. Far too heavy; can't run on an MCU. |
| **POSIX `mqueue`** | Inter-process, kernel-backed, fixed capacity at creation. Overkill and not portable to bare-metal. |
| **ZeroMQ** | A networking library. Wrong layer entirely. |
| **Hand-rolled queue** | What everyone ends up writing — but with no unified dispatch, and the thread-safety re-debugged in every project. |

There was a gap for something **intra-process, zero-dependency, and small
enough to run on an MCU** — with one unified dispatch API. That's embedmq.

embedmq is explicitly *not* a network or cross-process IPC library. It moves
messages between threads inside one process.

---

## Architecture

Three pieces: a registry, a ring queue, and a dispatcher.

```
Producer thread(s)                Consumer thread (internal)
─────────────────                 ──────────────────────────
embedmq_post(q, name, data, n)    sem_take()  ← sleeps when queue is empty
  │                                 │
  ├─ hash(name) → uuid             ├─ ring_read() → uuid + payload
  ├─ mutex_lock                    ├─ binary_search(uuid) → handler
  ├─ ring_write(uuid|len|payload)  └─ handler(data, size, ctx)
  ├─ mutex_unlock
  └─ sem_give  ──────────────────▶ wakes consumer
```

1. **Registry** — `embedmq_register(q, name, fn, ctx)` hashes `name` to a
   `uint32_t` UUID once, and stores `{uuid, fn, ctx}` in a table kept sorted by
   UUID.
2. **Ring queue** — `post()` serializes `[uuid][len][payload]` into a circular
   byte buffer.
3. **Dispatcher** — an internal consumer thread blocks on a semaphore, wakes on
   each `post()`, reads one message, binary-searches the UUID, and calls the
   handler.

The wire format of a single message is deliberately tiny:

```
[ uint32_t uuid ][ uint16_t len ][ payload bytes ]
       4              2               len
```

6 bytes of overhead per message. Everything downstream — capacity planning,
wrap handling — falls out of this fixed header.

---

## Design decisions

Each decision below is framed as **problem → choice → trade-off**, because the
trade-off is the part that matters.

### 1. UUID dispatch instead of string matching

**Problem.** Event-bus APIs are nicest with string names (`"battery.changed"`),
but comparing strings on every dispatch is wasteful — and on an embedded hot
path, every cycle counts.

**Choice.** Hash the name to a `uint32_t` UUID **once, at registration time**
(FNV-1a). At runtime, `post()` and dispatch only ever compare integers. The
handler table is kept sorted by UUID, so dispatch is an `O(log n)` binary
search, not a linear string scan.

```c
/* FNV-1a, 32-bit */
uint32_t h = 0x811C9DC5u;
while (*s) { h ^= (uint8_t)*s++; h *= 0x01000193u; }
return h ? h : 1;   /* reserve 0 as an invalid/sentinel UUID */
```

There's also `embedmq_post_id()` — pass a cached UUID and skip the hash
entirely on the hottest paths.

**Trade-off.** Two real costs. (a) **Hash collisions** are theoretically
possible — two different names mapping to the same `uint32_t`. With FNV-1a and a
handful of event names this is astronomically unlikely, and `register()` rejects
a duplicate UUID, but I'm trading a tiny correctness risk for speed. (b) **No
runtime introspection** — once a name is hashed, embedmq can't recover the
original string for logging. If you want human-readable traces you keep the
name→UUID map yourself.

### 2. Zero-malloc static mode

**Problem.** MCUs and memory-constrained Linux systems want to avoid heap
fragmentation and non-deterministic allocation latency. Many can't `malloc` at
all after init.

**Choice.** Two creation paths over one memory layout. Everything embedmq needs
lives in a single contiguous block:

```
[ struct embedmq_s ][ handler table ][ ring buffer ][ dispatch scratch ]
```

- `embedmq_create()` `calloc`s that block (convenient, for Linux).
- `embedmq_create_static()` places it inside a caller-provided buffer — **no
  heap, ever.** `embedmq_mem_size(cfg)` tells you exactly how big the buffer
  must be, so you can put it in BSS.

```c
static embedmq_config_t cfg = { .queue_size = 4096, .max_handlers = 16 };
static uint8_t buf[ /* embedmq_mem_size(&cfg) */ 8192 ];
embedmq_t *q = embedmq_create_static(buf, sizeof(buf), &cfg);
```

**Trade-off.** The single-block layout means the config is **fixed at creation**
— you can't grow the ring buffer or add more handler slots later without tearing
down and rebuilding. For an embedded event bus that's the right default
(deterministic, bounded), but it's a real constraint compared to a dynamically
resizing structure.

I exposed `embedmq_mem_size()` as a runtime function rather than a
compile-time macro on purpose: the macro version would have to expose
`sizeof(struct embedmq_s)` in the public header, leaking internals. A function
keeps the struct opaque.

### 3. Semaphore wakeup instead of busy-wait

**Problem.** The consumer needs to wait for messages. Spinning burns CPU and
battery — unacceptable on an embedded device.

**Choice.** A counting semaphore. The consumer blocks in `sem_take()` when the
queue is empty; each `post()` does one `sem_give()`. The count tracks
unconsumed messages, so the consumer wakes exactly as often as needed and sleeps
otherwise. This is the same pattern FreeRTOS queues use internally.

**Trade-off.** One semaphore operation per message is not free — for an
extremely high-frequency, single-threaded, latency-insensitive workload, a
batched/polled design could have less syscall overhead. embedmq optimizes for
the common case (idle most of the time, wake promptly when work arrives) instead.
The bare-metal PAL sidesteps this entirely: no thread, no semaphore — you call
`embedmq_poll()` from your superloop.

### 4. Ring buffer wrap handling

**Problem.** A message near the end of the circular buffer may straddle the
wrap point — its bytes split across the buffer's end and start.

**Choice.** Handle it transparently with **two `memcpy`s**: copy up to the
buffer end, then the remainder from the start. No padding, no skipped tail
region, no "is there room before the wrap?" special case in the caller.

**Trade-off.** I considered a sentinel/padding scheme (skip to the start if a
message won't fit before the end). That keeps every message contiguous — nicer
for zero-copy reads — but wastes the tail bytes and adds a branch to every
write. The two-memcpy approach uses the buffer fully and keeps the logic in one
place; the cost is that a wrapped message is copied in two pieces instead of
one. For small messages that's negligible.

### 5. PAL, not HAL

**Problem.** The same core has to run on pthreads, on FreeRTOS, and on bare
metal. Those differ in their **OS primitives**, not their hardware.

**Choice.** A **Platform Abstraction Layer**: a small interface
(`pal/embedmq_pal.h`) of 8 functions — counting semaphore, mutex, thread — with
one implementation per platform selected at compile time:

```
pal/linux/     pthread_mutex + POSIX sem + pthread
pal/freertos/  xSemaphoreCreateCounting/Mutex + xTaskCreate
pal/none/      C11 atomics spinlock; no thread (use embedmq_poll())
```

I call it PAL rather than **HAL** on purpose. A *Hardware* Abstraction Layer
abstracts GPIO, SPI, timers — the silicon. embedmq never touches hardware; it
abstracts **OS primitives** (threads, semaphores). Calling that a HAL would be a
misnomer, and embedded folks notice.

**Trade-off.** The abstraction is the lowest common denominator of three very
different OSes, so it can't expose platform-specific niceties (FreeRTOS task
notifications, Linux `eventfd`, priority inheritance tuning). Adding a new RTOS
is cheap — implement 8 functions — but the core can't take advantage of anything
beyond those 8.

---

## Porting to FreeRTOS: the war story

The Linux and bare-metal backends were straightforward. FreeRTOS is where the
abstraction got tested for real — and where the interesting bugs lived. I
verified everything on the **FreeRTOS POSIX port (GCC_POSIX)**, which runs the
real FreeRTOS scheduler on a host machine with no hardware (see
[the simulator section](#how-it-is-verified)).

### A FreeRTOS task must never return

embedmq's consumer loop `break`s out and returns when the queue is torn down —
perfectly fine with pthreads. On FreeRTOS, a task function that returns is
undefined behaviour (it trips `configASSERT` / crashes). A task must delete
itself with `vTaskDelete(NULL)`.

So the FreeRTOS PAL wraps the consumer in a trampoline:

```c
static void task_trampoline(void *param) {
    embedmq_pal_thread_t *t = param;
    t->fn(t->arg);            /* run the consumer loop... */
    xSemaphoreGive(t->done);  /* ...signal we're done... */
    vTaskDelete(NULL);        /* ...and exit cleanly. Never returns. */
}
```

### FreeRTOS has no `pthread_join`

`embedmq_destroy()` must block until the consumer has actually stopped before it
frees anything — otherwise the consumer touches freed memory. With pthreads
that's `pthread_join()`. FreeRTOS has no equivalent.

The fix is the `done` semaphore above. The thread handle isn't a bare
`TaskHandle_t` — it carries the join primitive with it:

```c
typedef struct {
    TaskHandle_t      handle;
    SemaphoreHandle_t done;   /* given by the task right before vTaskDelete */
    void            (*fn)(void *);
    void             *arg;
} embedmq_pal_thread_t;
```

`join()` simply takes `done` and waits. The task self-deletes; the idle task
reclaims its TCB and stack. No `pthread_join`, no busy-polling of task state,
and — because `fn`/`arg` live in the handle — no heap allocation for the
trampoline context.

### The POSIX-simulator stack-size trap

This one cost the most time. The simulator hung immediately on startup with
*zero output* — the test task never ran.

The cause: in the GCC_POSIX port, the FreeRTOS stack buffer
(`depth × sizeof(StackType_t)`, where `StackType_t` is 8 bytes) doubles as the
pthread stack. I had set `configMINIMAL_STACK_SIZE` to `PTHREAD_STACK_MIN`
(~16 KB on modern glibc) used as a **depth** — so a "minimal" task asked for
`16384 × 8 = 128 KB`, and my multipliers pushed individual tasks past 0.5–1 MB.
That blew the FreeRTOS heap, `xTaskCreate` failed silently, and the scheduler
spun the idle task forever.

The port also clamps the real pthread stack up to `PTHREAD_STACK_MIN` anyway, so
the fix decouples the two: express the depth as
`PTHREAD_STACK_MIN / sizeof(StackType_t)` and give the host a generous heap.

```c
#define configMINIMAL_STACK_SIZE \
    ( ( unsigned short ) ( PTHREAD_STACK_MIN / sizeof( unsigned long ) ) )
```

**Lesson:** a "stack size" number means different things in different ports.
On Cortex-M it's words of real stack; on the POSIX port it's both a heap
allocation *and* a pthread stack request. Same field, different units.

### How it is verified

The simulator (`sim/freertos/`) builds the real embedmq core + FreeRTOS PAL
against the kernel's POSIX port and runs in CI on every push — covering basic
post, the `post_id` hot path, concurrent producers, and the destroy/join
handshake.

Be honest about what that proves: the simulator validates **PAL logic** —
semaphore wakeup, mutex, task create/exit, dispatch ordering. It does **not**
exercise real-hardware timing, ISR-context safety, or tight-RAM behaviour. The
README says "simulator-verified," not "hardware-verified," and that distinction
stays until embedmq runs on an actual board.

---

## Performance

Measured on x86-64 Linux, Release build, single producer + consumer thread:

| Benchmark | Result |
|---|---|
| `embedmq_post()` throughput | ~3.0M msgs/sec |
| `embedmq_post_id()` throughput | ~3.0M msgs/sec |
| End-to-end latency (post → handler), avg | ~38 µs |
| End-to-end latency, min | ~7 µs |
| `embedmq_uuid()` hash speed | ~45M hashes/sec (~22 ns) |

The throughput comes from keeping the hot path boring: an integer compare for
dispatch, a single `memcpy` into the ring (two only when wrapping), and one
semaphore op per message. There's no allocation, no string work, and no
syscall beyond the semaphore on the entire path.

Run it yourself: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/benchmark`.
