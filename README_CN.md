# embedmq

**[English ->](README.md)**

![CI](https://github.com/w4ysonch/embedmq/actions/workflows/ci.yml/badge.svg)

零依赖的嵌入式消息分发库，支持 Linux、RTOS 和裸机。

**3 个核心函数。静态模式零堆分配。UUID dispatch — 运行时零字符串操作。**

```c
embedmq_t *q = embedmq_create(NULL);
embedmq_register(q, "battery.changed", on_battery, NULL);
embedmq_post(q, "battery.changed", &info, sizeof(info));
```

---

## 为什么用 embedmq？

| | **embedmq** | DBus | ZeroMQ | 手写队列 |
|---|---|---|---|---|
| 依赖 | 无 | daemon + glib | libzmq | 无 |
| 平台 | Linux / RTOS / 裸机 | 仅 Linux | 全平台 | 全平台 |
| 零堆分配 | ✅ 静态模式 | ❌ | ❌ | 手动 |
| 运行时 dispatch | 整数比较 | 字符串 | 字符串 | 手动 |
| 线程安全 post | ✅ | ✅ | ✅ | 手动 |

embedmq 用于**进程内**线程间通信，不是网络或跨进程 IPC 库。

---

## 工作原理

```
生产者线程（任意数量）              消费者线程（内部）
──────────────────                  ──────────────────────
embedmq_post(q, name, data, n)      sem_wait()  ← 队列空时睡眠
  │                                   │
  ├─ hash(name) → uuid               ├─ ring_read() → uuid + payload
  ├─ mutex_lock                      ├─ 二分查找(uuid) → handler
  ├─ ring_write(uuid|len|payload)    └─ handler(data, size, ctx)
  ├─ mutex_unlock
  └─ sem_post  ──────────────────▶ 唤醒消费者
```

- **注册表**：`name` 在注册时一次性 hash 成 `uint32_t` UUID。运行时 dispatch 只比较整数，热路径零字符串操作。
- **环形缓冲**：单消费者无锁读，多生产者 mutex 保护写。消息可跨缓冲区末尾 wrap，透明处理。
- **信号量**：队列空时消费者睡眠；每次 `post()` 唤醒一次，不空转。

---

## 快速开始

### 动态模式（Linux / 有堆）

```c
#include "embedmq.h"

typedef struct { int level; float voltage; } battery_t;

static void on_battery(const void *data, size_t size, void *ctx)
{
    const battery_t *b = data;
    printf("电量: %d%% %.2fV\n", b->level, b->voltage);
}

int main(void)
{
    embedmq_t *q = embedmq_create(NULL);  /* NULL = 使用默认配置 */

    embedmq_register(q, "battery.changed", on_battery, NULL);

    battery_t b = { .level = 85, .voltage = 4.05f };
    embedmq_post(q, "battery.changed", &b, sizeof(b));

    sleep(1);
    embedmq_destroy(q);
}
```

### 静态模式（零堆分配 — MCU / RTOS）

```c
#include "embedmq.h"

static embedmq_config_t cfg = {
    .queue_size   = 2048,
    .max_msg_size = 64,
    .max_handlers = 8,
};

/* 放在 BSS 段，无动态分配 */
static uint8_t mq_buf[4096]; /* 用 embedmq_mem_size(&cfg) 精确计算大小 */
static embedmq_t *q;

void app_init(void)
{
    q = embedmq_create_static(mq_buf, sizeof(mq_buf), &cfg);
    embedmq_register(q, "sensor.update", on_sensor, NULL);
}

/* 可从任意任务或 ISR 安全上下文调用 */
void sensor_isr(void)
{
    sensor_data_t d = read_sensor();
    embedmq_post(q, "sensor.update", &d, sizeof(d));
}
```

### C++ wrapper（lambda + RAII）

```cpp
#include "embedmq.hpp"

embedmq::MQ q;

// lambda 捕获局部变量，不需要全局函数
int count = 0;
q.subscribe("battery.changed", [&count](const void *data, size_t size) {
    const auto *b = static_cast<const BatteryInfo *>(data);
    printf("level=%d\n", b->level);
    count++;
});

q.publish("battery.changed", &info, sizeof(info));

// 热路径：缓存 UUID，直接用 ID 投递
uint32_t uuid = embedmq::MQ::uuid("battery.changed");
q.publish_id(uuid, &info, sizeof(info));

// q 析构时自动调用 embedmq_destroy()，不会泄漏（RAII）
```

### 热路径变体（跳过每次 post 的 hash 计算）

```c
/* 启动时算一次 UUID 并缓存 */
uint32_t uuid = embedmq_uuid("touch.point");

/* 紧循环 — 无字符串 hash */
while (reading) {
    touch_t t = read_touch();
    embedmq_post_id(q, uuid, &t, sizeof(t));
}
```

### 裸机 / 无 OS（超级循环）

```c
/* 编译时加 -DEMBEDMQ_PAL_NONE */
embedmq_t *q = embedmq_create(&cfg);
embedmq_register(q, "tick", on_tick, NULL);

while (1) {
    embedmq_post(q, "tick", NULL, 0);
    embedmq_poll(q);   /* 排空所有待处理消息，调用对应 handler */
    sleep_until_next_tick();
}
```

---

## API 参考

```c
/* 创建 / 销毁 */
embedmq_t *embedmq_create(const embedmq_config_t *cfg);
embedmq_t *embedmq_create_static(void *mem, size_t mem_size,
                                  const embedmq_config_t *cfg);
size_t     embedmq_mem_size(const embedmq_config_t *cfg);
void       embedmq_destroy(embedmq_t *q);

/* 注册（在生产者启动前调用） */
int embedmq_register(embedmq_t *q, const char *name,
                     embedmq_handler_fn fn, void *ctx);

/* 投递（线程安全，非阻塞） */
int embedmq_post(embedmq_t *q, const char *name,
                 const void *data, size_t size);
int embedmq_post_id(embedmq_t *q, uint32_t uuid,         /* 热路径 */
                    const void *data, size_t size);

/* 工具函数 */
uint32_t embedmq_uuid(const char *name);  /* 无状态纯 hash */
int      embedmq_poll(embedmq_t *q);      /* 裸机超级循环驱动 */
```

**Handler 签名：**
```c
typedef void (*embedmq_handler_fn)(const void *data, size_t size, void *ctx);
```

**返回码：** `EMBEDMQ_OK (0)` · `EMBEDMQ_FULL (-2)` · `EMBEDMQ_EXIST (-4)` · `EMBEDMQ_INVAL (-5)`

**配置字段**（`embedmq_config_t`，传 `NULL` 使用默认值）：

| 字段 | 默认值 | 说明 |
|---|---|---|
| `queue_size` | 8192 | 环形缓冲区字节数 |
| `max_msg_size` | 1024 | 单条消息 payload 上限 |
| `max_handlers` | 64 | 最大注册事件名数量 |
| `thread_priority` | 0 | 消费线程优先级（0 = 系统默认） |

> 每个函数的完整参考（参数、返回值、线程规则）：**[docs/API_CN.md](docs/API_CN.md)**

---

## 性能数据

在 x86-64 Linux 上测量（Release 构建，单生产者 + 消费线程）：

| 测试项 | 结果 |
|---|---|
| `embedmq_post()` 吞吐量 | **300 万条/秒** |
| `embedmq_post_id()` 吞吐量（UUID 已缓存） | **300 万条/秒** |
| 端到端平均延迟（post → handler） | **~38 µs** |
| 端到端最小延迟 | **~7 µs** |
| `embedmq_uuid()` hash 速度 | **4500 万次/秒**（~22 ns/次） |

在自己机器上跑：
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make
./benchmark
```

---

## 构建

```bash
# Linux（默认）
mkdir build && cd build
cmake .. && make

# 运行测试
./test_embedmq

# 裸机 / 无 OS 模式
cmake .. -DEMBEDMQ_PAL=none && make
```

### 不用 CMake 直接集成

把以下文件复制到你的项目并一起编译：

```
src/embedmq.c
src/embedmq_hash.c
src/embedmq_queue.c
pal/linux/embedmq_pal.c      ← 或换成你的目标平台
```

编译器 flag 加上 `-Iinclude -Ipal`。

---

## 平台支持

| PAL | 文件 | 说明 |
|---|---|---|
| Linux | `pal/linux/embedmq_pal.c` | pthreads + POSIX 信号量 |
| FreeRTOS | `pal/freertos/embedmq_pal.c` | 计数信号量 + task；已在 FreeRTOS POSIX 模拟器（GCC_POSIX）验证 |
| 裸机 | `pal/none/embedmq_pal.c` | C11 原子 spinlock，需调用 `embedmq_poll()` |

> FreeRTOS PAL 在 CI 里用 FreeRTOS POSIX 端口（host 模拟器，无需硬件）验证 —— 见 `sim/freertos/`。
> 这验证的是 PAL 逻辑（信号量唤醒、互斥、task 创建/退出、dispatch 流程），**不**覆盖真实硬件时序与
> ISR 上下文。构建模拟器测试：
>
> ```bash
> cmake -B build -DEMBEDMQ_BUILD_FREERTOS_SIM=ON   # 会拉取 FreeRTOS-Kernel
> cmake --build build
> ./build/sim/freertos/test_embedmq_freertos
> ```

---

## License

MIT
