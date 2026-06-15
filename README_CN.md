<div align="center">

# embedmq

### Linux 多线程、RTOS 多任务、裸机超级循环 — 一套 API 搞定线程间消息分发

[![Version](https://img.shields.io/github/v/release/w4ysonch/embedmq?color=blue&label=version)](https://github.com/w4ysonch/embedmq/releases)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20RTOS%20%7C%20bare--metal-lightgrey.svg)](#平台支持)
![CI](https://github.com/w4ysonch/embedmq/actions/workflows/ci.yml/badge.svg)

**[English ->](README.md)**

---

</div>

## 为什么用 embedmq？

嵌入式系统里，线程之间总要互相通知：传感器读到新数据、WiFi 连上了、按键被按下、定时器触发了。FreeRTOS 任务多了，每对任务之间手写 Queue 结构体太繁琐。裸机超级循环里 `if (flag1) ... if (flag2) ...` 越叠越乱。Linux 应用里模块间互相持有对方指针，改一处牵动全局。

embedmq 把这一切收拢成 3 个函数——`create`、`register`、`post`。你决定**谁发什么**、**谁接什么**，库替你处理队列、互斥、信号量和 dispatch。

- **Linux 多线程，不必引入 DBus** — 硬件监控线程、网络线程、配置线程——每个 `post` 自己的事件，`register` 自己关心的通知。一个头文件 + 一个 PAL 文件，链接 `-lpthread` 即用。零外部依赖，不用为线程间通信引入重型框架。

- **FreeRTOS 多任务通信，不再手写 Queue** — 传感器任务只做一件事：`post("sensor.temp", &data, sizeof(data))`。UI 任务只做一件事：`register("sensor.temp", on_temp, NULL)`。两边互不感知对方的 TaskHandle 或队列句柄。

- **裸机超级循环，不用堆砌 `if (flag)`** — 定时器 ISR post `"tick.10ms"`，按键 ISR post `"button.press"`，ADC 完成回调 post `"adc.done"`。主循环一行 `embedmq_poll(q)` 全部 dispatch，不再加一个 flag 就在 main 里加一个 if。

- **一套 API，三个平台** — Linux 上用 pthread + POSIX 信号量，FreeRTOS 上用计数信号量 + Task，裸机上用 C11 原子锁 + poll()。换平台只换一个 PAL 文件，核心代码、公开头文件、调用方式完全不变。

- **注册时 hash 一次，运行时零字符串** — name 注册时被 FNV-1a hash 成 `uint32_t` UUID 并插入排序数组，dispatch 做二分查找整数比较。缓存 UUID 后在紧循环里用 `post_id()` 投递——整个热路径没有字符串操作，一次信号量操作，一次 memcpy 入队。

- **初始化之后 zero-malloc** — `create_static()` 把所有内部状态放进调用者提供的 BSS 缓冲区。创建完成后再也不碰堆。适合 MCU 上禁止动态分配的场景。

| vs | **embedmq** | DBus | ZeroMQ | 手写队列 |
|---|---|---|---|---|
| 依赖 | 无 | daemon + glib | libzmq | 无 |
| 适用 | 进程内线程间 | 跨进程 | 跨网络 | 进程内 |
| 平台 | Linux / RTOS / 裸机 | Linux | 全平台 | 全平台 |
| 运行时 dispatch | **整数比较** | 字符串 | 字符串 | 手动 |
| 线程安全 | ✅ 内置 | ✅ | ✅ | 手动 |
| 零堆分配 | ✅ | ❌ | ❌ | 手动 |

embedmq 只做**进程内**线程到线程的消息分发。不适合跨进程 IPC、网络 RPC 或纯单线程同步代码。

---

## 快速开始

```c
embedmq_t *q = embedmq_create(NULL);
embedmq_register(q, "battery.changed", on_battery, NULL);
embedmq_post(q, "battery.changed", &info, sizeof(info));
```

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

static uint8_t mq_buf[4096]; /* 用 embedmq_mem_size(&cfg) 精确计算 */
static embedmq_t *q;

void app_init(void)
{
    q = embedmq_create_static(mq_buf, sizeof(mq_buf), &cfg);
    embedmq_register(q, "sensor.update", on_sensor, NULL);
}

/* 任意任务或 ISR 安全上下文均可调用 */
void sensor_isr(void)
{
    sensor_data_t d = read_sensor();
    embedmq_post(q, "sensor.update", &d, sizeof(d));
}
```

### 热路径变体（跳过每次 post 的 hash）

```c
/* 启动时算一次 UUID 并缓存 */
uint32_t uuid = embedmq_uuid("touch.point");

/* 紧循环里直接用 UUID 投递 — 无字符串操作 */
while (reading) {
    touch_t t = read_touch();
    embedmq_post_id(q, uuid, &t, sizeof(t));
}
```

### 裸机 / 无 OS（超级循环驱动）

```c
/* 编译时加 -DEMBEDMQ_PAL_NONE */
embedmq_t *q = embedmq_create(&cfg);
embedmq_register(q, "tick", on_tick, NULL);

while (1) {
    embedmq_post(q, "tick", NULL, 0);
    embedmq_poll(q);   /* 排空所有待处理消息 */
    sleep_until_next_tick();
}
```

### C++ wrapper（lambda + RAII）

```cpp
#include "embedmq.hpp"

embedmq::MQ q;

q.subscribe("battery.changed", [](const void *data, size_t size) {
    const auto *b = static_cast<const BatteryInfo *>(data);
    printf("level=%d\n", b->level);
});

q.publish("battery.changed", &info, sizeof(info));

// 热路径：缓存 UUID
uint32_t uuid = embedmq::MQ::uuid("battery.changed");
q.publish_id(uuid, &info, sizeof(info));

// 析构自动调用 embedmq_destroy()（RAII）
```

---

## 性能

x86-64 Linux、Release 构建、单生产者 + 消费线程：

| 测试项 | 结果 |
|---|---|
| `embedmq_post()` 吞吐量 | **300 万条/秒** |
| `embedmq_post_id()` 吞吐量（UUID 已缓存） | **300 万条/秒** |
| 端到端平均延迟（post → handler） | **~38 µs** |
| 端到端最小延迟 | **~7 µs** |
| `embedmq_uuid()` hash 速度 | **4500 万次/秒**（~22 ns/次） |

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/benchmark
```

---

## API 总览

```c
/* 创建 / 销毁 */
embedmq_t *embedmq_create(const embedmq_config_t *cfg);
embedmq_t *embedmq_create_static(void *mem, size_t mem_size, const embedmq_config_t *cfg);
size_t     embedmq_mem_size(const embedmq_config_t *cfg);
void       embedmq_destroy(embedmq_t *q);

/* 注册 handler（启动时完成，不要和 post 并发） */
int embedmq_register(embedmq_t *q, const char *name, embedmq_handler_fn fn, void *ctx);

/* 投递消息（线程安全，非阻塞，任意线程可调用） */
int embedmq_post(embedmq_t *q, const char *name, const void *data, size_t size);
int embedmq_post_id(embedmq_t *q, uint32_t uuid, const void *data, size_t size);

/* 工具 */
uint32_t embedmq_uuid(const char *name);  /* 无状态纯 hash */
int      embedmq_poll(embedmq_t *q);      /* 裸机超级循环驱动 */
```

**返回码：** `EMBEDMQ_OK (0)` · `EMBEDMQ_FULL (-2)` · `EMBEDMQ_EXIST (-4)` · `EMBEDMQ_INVAL (-5)`

**配置字段**（`embedmq_config_t`，传 `NULL` 使用默认值）：

| 字段 | 默认值 | 说明 |
|---|---|---|
| `queue_size` | 8192 | 环形缓冲区总字节数 |
| `max_msg_size` | 1024 | 单条 payload 上限 |
| `max_handlers` | 64 | 最大注册事件名数量 |
| `thread_priority` | 0 | 消费者线程优先级（0 = 系统默认） |

> 完整 API 文档（线程规则、生命周期）：**[docs/API_CN.md](docs/API_CN.md)** · 设计决策：**[docs/DESIGN_CN.md](docs/DESIGN_CN.md)**

---

## 构建

```bash
# Linux（默认）
cmake -B build && cmake --build build && ./build/test_embedmq

# 裸机 / 无 OS 模式
cmake -B build-none -DEMBEDMQ_PAL=none -DEMBEDMQ_BUILD_TESTS=OFF && cmake --build build-none

# FreeRTOS POSIX 模拟器测试（自动拉取 FreeRTOS-Kernel）
cmake -B build-frt -DEMBEDMQ_BUILD_FREERTOS_SIM=ON && cmake --build build-frt
./build-frt/sim/freertos/test_embedmq_freertos
```

不用 CMake：把 `src/embedmq.c`、`src/embedmq_hash.c`、`src/embedmq_queue.c`、对应平台的 `pal/<xxx>/embedmq_pal.c` 复制到项目，加 `-Iinclude -Ipal` 编译。Linux PAL 链接 `-lpthread`。

> 完整构建指南：**[docs/BUILD_CN.md](docs/BUILD_CN.md)**

---

## 平台支持

| PAL | 文件 | 说明 |
|---|---|---|
| Linux | `pal/linux/embedmq_pal.c` | pthread + POSIX 计数信号量 |
| FreeRTOS | `pal/freertos/embedmq_pal.c` | 计数信号量 + task；CI 中通过 POSIX 模拟器持续验证 |
| 裸机 | `pal/none/embedmq_pal.c` | C11 原子自旋锁；由 `embedmq_poll()` 驱动 |

> FreeRTOS PAL 验证了信号量唤醒、互斥、task 创建/退出、dispatch 流程的正确性，未覆盖真实硬件时序和 ISR 上下文。

---

## License

MIT
