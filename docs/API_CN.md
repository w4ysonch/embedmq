# embedmq API 参考

**[English ->](API.md)**

`include/embedmq.h` 中公开 C API 的完整参考。
C++ 封装见文末 [C++ 封装](#c-封装) 一节。

- [核心概念](#核心概念)
- [类型](#类型)
- [返回码](#返回码)
- [配置](#配置)
- [函数](#函数)
  - [embedmq_create](#embedmq_create)
  - [embedmq_create_static](#embedmq_create_static)
  - [embedmq_mem_size](#embedmq_mem_size)
  - [embedmq_register](#embedmq_register)
  - [embedmq_post](#embedmq_post)
  - [embedmq_post_id](#embedmq_post_id)
  - [embedmq_uuid](#embedmq_uuid)
  - [embedmq_poll](#embedmq_poll)
  - [embedmq_destroy](#embedmq_destroy)
- [生命周期与线程规则](#生命周期与线程规则)
- [C++ 封装](#c-封装)

---

## 核心概念

embedmq 在**单个进程内**、线程之间分发消息。

- **register（注册）** —— 字符串 `name` 一次性 hash 成 `uint32_t` UUID，并绑定到一个 handler。
- **post（投递）** —— payload 打上 UUID，写入环形缓冲区，唤醒消费者。
- **dispatch（分发）** —— 内部消费者线程被唤醒，按 UUID 查表，调用 handler。

运行时 dispatch 比较的是整数而非字符串 —— 热路径上没有任何字符串操作。

---

## 类型

### `embedmq_t`

```c
typedef struct embedmq_s embedmq_t;
```

不透明的分发器句柄。内部结构与平台相关，禁止直接访问。
通过 `embedmq_create()` 或 `embedmq_create_static()` 获取。

### `embedmq_handler_fn`

```c
typedef void (*embedmq_handler_fn)(const void *data, size_t size, void *ctx);
```

Handler 回调，从内部消费者线程被调用。

| 参数 | 说明 |
|---|---|
| `data` | 指向 payload 的指针。**仅在本次调用期间有效** —— 需要保留请自行拷贝。 |
| `size` | payload 字节数。 |
| `ctx`  | 注册时传入 `embedmq_register()` 的用户上下文。 |

> handler 运行在消费者线程，而非投递线程。请保持简短，重活另外开线程处理。

---

## 返回码

| 宏 | 值 | 含义 |
|---|---|---|
| `EMBEDMQ_OK`    | `0`  | 成功 |
| `EMBEDMQ_ERR`   | `-1` | 通用错误 |
| `EMBEDMQ_FULL`  | `-2` | 环形缓冲区满 —— 消息被丢弃 |
| `EMBEDMQ_NOMEM` | `-3` | 内存不足 |
| `EMBEDMQ_EXIST` | `-4` | 该名字已注册过 handler |
| `EMBEDMQ_INVAL` | `-5` | 参数非法（如句柄为 `NULL`、payload 超过 `max_msg_size`） |

---

## 配置

```c
typedef struct {
    size_t queue_size;      /* 环形缓冲字节数，   默认: 8192     */
    size_t max_msg_size;    /* 单条消息最大 payload，默认: 1024  */
    size_t max_handlers;    /* 最大注册名字数，   默认: 64       */
    int    thread_priority; /* 消费线程优先级，   0 = 系统默认   */
} embedmq_config_t;
```

| 字段 | 默认值 | 说明 |
|---|---|---|
| `queue_size` | 8192 | 环形缓冲区总字节数。可装很多小消息或较少的大消息。每条消息有 6 字节开销（`uint32_t` UUID + `uint16_t` 长度）。 |
| `max_msg_size` | 1024 | 单条 payload 上限。`post()` 的 `size` 超过此值返回 `EMBEDMQ_INVAL`。 |
| `max_handlers` | 64 | 可注册的不同名字数量上限。 |
| `thread_priority` | 0 | 消费线程优先级。`0` 表示系统默认。无 OS 构建下忽略。 |

`embedmq_create()` 传 `NULL` 即用全部默认值。`embedmq_create_static()` 的 config 不能为 `NULL` ——
需显式设置 `queue_size` 和 `max_handlers`，使缓冲区大小可确定计算。

---

## 函数

### `embedmq_create`

```c
embedmq_t *embedmq_create(const embedmq_config_t *cfg);
```

以**动态（malloc）模式**创建分发器，并启动内部消费者线程。

| 参数 | 说明 |
|---|---|
| `cfg` | 配置，或传 `NULL` 使用全部默认值。 |

**返回：** 成功返回有效句柄，分配或建线程失败返回 `NULL`。

**线程：** 启动期调用一次。返回的句柄之后可在多个线程间共享用于投递。

```c
embedmq_t *q = embedmq_create(NULL);
if (!q) { /* 处理错误 */ }
```

---

### `embedmq_create_static`

```c
embedmq_t *embedmq_create_static(void *mem, size_t mem_size,
                                 const embedmq_config_t *cfg);
```

以**静态（零 malloc）模式**创建分发器。所有内部状态都放在调用者提供的缓冲区里 ——
此调用之后不再有任何堆分配。适用于规避动态内存的 MCU / RTOS 目标。

| 参数 | 说明 |
|---|---|
| `mem` | 调用者提供的缓冲区，需按指针大小对齐。 |
| `mem_size` | `mem` 的字节数 —— 至少要等于 `embedmq_mem_size(cfg)`。 |
| `cfg` | **不能为 `NULL`。** 需显式设置 `queue_size` 和 `max_handlers`。 |

**返回：** 成功返回句柄（等于 `mem`）；缓冲区太小或参数非法返回 `NULL`。

```c
static embedmq_config_t cfg = { .queue_size = 4096, .max_handlers = 16 };
static uint8_t buf[8192];                       /* 用 embedmq_mem_size() 定大小 */
embedmq_t *q = embedmq_create_static(buf, sizeof(buf), &cfg);
```

> 无 OS 构建（`EMBEDMQ_PAL_NONE`）下不会创建消费者线程，需用 `embedmq_poll()` 驱动分发。

---

### `embedmq_mem_size`

```c
size_t embedmq_mem_size(const embedmq_config_t *cfg);
```

计算 `embedmq_create_static()` 在给定 config 下所需的缓冲区大小。
传入的 `cfg` 必须与之后传给 `embedmq_create_static()` 的**完全一致**。

| 参数 | 说明 |
|---|---|
| `cfg` | 要计算大小的那份配置。 |

**返回：** 所需缓冲区字节数。

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

把一个 `name` 绑定到 handler。name 在调用时一次性 hash 成 UUID；之后用同一个 name
（或等价的 UUID）投递就会唤醒此 handler。

| 参数 | 说明 |
|---|---|
| `q` | 分发器句柄。 |
| `name` | 事件名。用 FNV-1a hash 成 `uint32_t`。 |
| `fn` | handler 回调。 |
| `ctx` | 用户上下文，每次分发时回传给 handler。可为 `NULL`。 |

**返回：** `EMBEDMQ_OK`、`EMBEDMQ_EXIST`（名字已注册）或 `EMBEDMQ_ERR`（handler 表满 / 参数非法）。

> **必须在任何生产者调用 `embedmq_post()` 之前调用。** 与其他 `embedmq_register()` 调用之间
> 非线程安全 —— 请在单线程启动期完成所有注册。

```c
embedmq_register(q, "battery.changed", on_battery, &app_state);
```

---

### `embedmq_post`

```c
int embedmq_post(embedmq_t *q, const char *name,
                 const void *data, size_t size);
```

按名字投递事件。非阻塞、线程安全。将 `name` hash 成 UUID，把 payload 拷入环形缓冲区，
唤醒消费者线程。

| 参数 | 说明 |
|---|---|
| `q` | 分发器句柄。 |
| `name` | 事件名（与注册时用的字符串一致）。 |
| `data` | payload 指针，或传 `NULL` 表示零长度通知。 |
| `size` | payload 字节数。必须 `<= max_msg_size`。 |

**返回：** `EMBEDMQ_OK`、`EMBEDMQ_FULL`（缓冲区无空间 —— 消息被丢弃，是否重试由调用者决定）或 `EMBEDMQ_INVAL`。

> payload 会被**拷贝**进队列，因此 `data` 不需要在调用之后继续存活。
> 投递一个没有任何 handler 注册的名字不算错误 —— 该消息只是分发给空（无人接收）。

```c
battery_t b = { .level = 85 };
embedmq_post(q, "battery.changed", &b, sizeof(b));
embedmq_post(q, "shutter.pressed", NULL, 0);   /* 零长度事件 */
```

---

### `embedmq_post_id`

```c
int embedmq_post_id(embedmq_t *q, uint32_t uuid,
                    const void *data, size_t size);
```

`embedmq_post()` 的热路径变体：接收预先算好的 UUID，跳过 name hash。
用 `embedmq_uuid()` 算一次 UUID 并缓存起来即可。

| 参数 | 说明 |
|---|---|
| `q` | 分发器句柄。 |
| `uuid` | `embedmq_uuid()` 算出的预计算 UUID。 |
| `data` | payload 指针，或 `NULL`。 |
| `size` | payload 字节数。必须 `<= max_msg_size`。 |

**返回：** `EMBEDMQ_OK`、`EMBEDMQ_FULL` 或 `EMBEDMQ_INVAL`。

```c
uint32_t id = embedmq_uuid("touch.point");      /* 一次 */
while (reading) {
    embedmq_post_id(q, id, &pt, sizeof(pt));     /* 紧循环，无 hash */
}
```

---

### `embedmq_uuid`

```c
uint32_t embedmq_uuid(const char *name);
```

计算一个名字的 UUID。无状态纯 hash（FNV-1a）—— 返回值与 `embedmq_register()`、
`embedmq_post()` 内部使用的一致。可在任意上下文调用，包括分发器还不存在时。

| 参数 | 说明 |
|---|---|
| `name` | 事件名。 |

**返回：** `uint32_t` UUID。（实现会把结果为 `0` 的情况映射成 `1`，以便 `0` 用作哨兵值。）

```c
uint32_t id = embedmq_uuid("battery.changed");
```

---

### `embedmq_poll`

```c
int embedmq_poll(embedmq_t *q);
```

手动排空环形缓冲区，为所有待处理消息调用 handler。

在正常构建（Linux / 带真实线程的 RTOS）下，内部消费者线程会自动调用它 —— **你用不到。**
它是为裸机 / 无 OS 构建（`EMBEDMQ_PAL_NONE`）准备的：那种场景下不会创建线程，
需要你在超级循环或周期任务里调用 `embedmq_poll()`。

| 参数 | 说明 |
|---|---|
| `q` | 分发器句柄。 |

**返回：** 本次分发的消息数（队列为空则返回 `0`）。

```c
/* 无 OS 超级循环 */
while (1) {
    read_inputs_and_post(q);
    embedmq_poll(q);          /* 分发本 tick 排入的所有消息 */
    wait_for_next_tick();
}
```

---

### `embedmq_destroy`

```c
void embedmq_destroy(embedmq_t *q);
```

停止消费者线程并释放资源。阻塞直到消费者线程干净退出，然后释放所有内存（动态模式）
或把句柄清零（静态模式 —— 调用者的缓冲区本身不会被释放）。

| 参数 | 说明 |
|---|---|
| `q` | 分发器句柄。传 `NULL` 为空操作。 |

```c
embedmq_destroy(q);
```

> destroy 之后句柄即失效。静态模式下该缓冲区可重新用于新的 `embedmq_create_static()`。

---

## 生命周期与线程规则

1. **创建一次**，在启动期：`embedmq_create()` / `embedmq_create_static()`。
2. **接着注册所有 handler**，仍处于单线程。`embedmq_register()` 与自身、与投递之间**都不**安全并发。
3. **启动生产者。** `embedmq_post()` / `embedmq_post_id()` 线程安全，可被任意数量线程同时调用。
4. handler 在**消费者线程**上运行，逐条、按 FIFO 顺序执行。`data` 指针仅在 handler 调用期间有效。
5. **销毁一次**，关机时：`embedmq_destroy()` 会 join 消费者线程。

| 函数 | 线程安全？ | 何时调用 |
|---|---|---|
| `embedmq_create*` | — | 启动期，一次 |
| `embedmq_register` | ❌ 否 | 启动期，任何投递之前 |
| `embedmq_post` / `embedmq_post_id` | ✅ 是 | 任意时刻，任意线程 |
| `embedmq_uuid` | ✅ 是（无状态） | 任意时刻 |
| `embedmq_poll` | ❌ 单一调用者 | 仅无 OS 模式 |
| `embedmq_destroy` | — | 关机，一次 |

---

## C++ 封装

`include/embedmq.hpp`（命名空间 `embedmq`）提供 header-only 的 RAII 封装。

```cpp
#include "embedmq.hpp"

embedmq::MQ q;                                  // 默认配置，RAII 管理

q.subscribe("battery.changed", [](const void *data, size_t size) {
    const auto *b = static_cast<const battery_t *>(data);
    printf("level=%d\n", b->level);
});

q.publish("battery.changed", &b, sizeof(b));

uint32_t id = embedmq::MQ::uuid("touch.point"); // 热路径
q.publish_id(id, &pt, sizeof(pt));
```

| 方法 | 对应 | 说明 |
|---|---|---|
| `MQ()` / `MQ(cfg)` / `MQ(mem, size, cfg)` | `embedmq_create*` | 失败抛 `std::runtime_error`。 |
| `subscribe(name, Handler)` | `embedmq_register` | `Handler` 是 `std::function<void(const void*, size_t)>`，支持带捕获的 lambda。 |
| `publish(name, data, size)` | `embedmq_post` | |
| `publish_id(uuid, data, size)` | `embedmq_post_id` | |
| `static uuid(name)` | `embedmq_uuid` | |
| `poll()` | `embedmq_poll` | |
| `native()` | — | 返回底层 `embedmq_t*`。 |
| `~MQ()` | `embedmq_destroy` | 自动（RAII）。可移动，不可拷贝。 |

> 封装会在 `MQ` 实例的整个生命周期内保存每个 `Handler`，并通过一个静态 trampoline 桥接到
> C handler。不要让 `MQ` 的存活时间超过它所捕获引用的那些更短命对象。
