# embedmq 设计笔记

**[English ->](DESIGN.md)**

embedmq 为什么这么设计——背后的决策、权衡，以及踩过的坑。如果说 `README.md` 讲
*是什么*、`API.md` 讲 *怎么用*，这篇讲的是 *为什么*。

- [要解决的问题](#要解决的问题)
- [整体架构](#整体架构)
- [设计决策](#设计决策)
  - [UUID dispatch 而非字符串匹配](#1-uuid-dispatch-而非字符串匹配)
  - [零 malloc 静态模式](#2-零-malloc-静态模式)
  - [信号量唤醒而非忙等](#3-信号量唤醒而非忙等)
  - [环形缓冲的 wrap 处理](#4-环形缓冲的-wrap-处理)
  - [是 PAL，不是 HAL](#5-是-pal不是-hal)
- [移植到 FreeRTOS：踩坑实录](#移植到-freertos踩坑实录)
- [性能](#性能)

---

## 要解决的问题

嵌入式 Linux 和 RTOS 项目几乎都需要**进程内、线程间**通信：传感器线程产出数据，UI 线程和
控制线程想要响应它，而你不希望每个模块都直接持有其他所有模块的指针。

现有方案都不合适：

| 方案 | 为什么不合适 |
|---|---|
| **DBus** | 需要 daemon + glib，太重，MCU 上根本跑不了。 |
| **POSIX `mqueue`** | 跨进程、内核支撑、容量创建时固定。杀鸡用牛刀，且无法移植到裸机。 |
| **ZeroMQ** | 网络库，完全是另一个层次。 |
| **手写队列** | 大家最后都在写这个——但没有统一 dispatch，线程安全在每个项目里重新调一遍。 |

这里有个空档：需要一个**进程内、零依赖、小到能在 MCU 上跑**的东西，外加一套统一的 dispatch
API。这就是 embedmq。

embedmq 明确**不是**网络或跨进程 IPC 库。它在单个进程内、线程之间搬运消息。

---

## 整体架构

三个组件：注册表、环形队列、分发器。

```
生产者线程（任意数量）              消费者线程（内部）
──────────────────                  ──────────────────────
embedmq_post(q, name, data, n)      sem_take()  ← 队列空时睡眠
  │                                   │
  ├─ hash(name) → uuid               ├─ ring_read() → uuid + payload
  ├─ mutex_lock                      ├─ 二分查找(uuid) → handler
  ├─ ring_write(uuid|len|payload)    └─ handler(data, size, ctx)
  ├─ mutex_unlock
  └─ sem_give  ──────────────────▶ 唤醒消费者
```

1. **注册表** —— `embedmq_register(q, name, fn, ctx)` 把 `name` 一次性 hash 成 `uint32_t`
   UUID，并把 `{uuid, fn, ctx}` 存进一张按 UUID 排序的表。
2. **环形队列** —— `post()` 把 `[uuid][len][payload]` 序列化进一个循环字节缓冲区。
3. **分发器** —— 一个内部消费者线程阻塞在信号量上，每次 `post()` 唤醒它，读一条消息，
   二分查找 UUID，调用对应 handler。

单条消息的 wire format 刻意做得很小：

```
[ uint32_t uuid ][ uint16_t len ][ payload bytes ]
       4              2               len
```

每条消息 6 字节开销。下游的一切——容量规划、wrap 处理——都从这个固定头部推导出来。

---

## 设计决策

下面每个决策都用 **问题 → 选择 → 权衡** 三段式来讲，因为**权衡**才是最重要的部分。

### 1. UUID dispatch 而非字符串匹配

**问题：** 事件总线 API 用字符串名字最顺手（`"battery.changed"`），但每次分发都比较字符串很
浪费——在嵌入式热路径上，每个周期都金贵。

**选择：** 在**注册时一次性**把名字 hash 成 `uint32_t` UUID（FNV-1a）。运行时 `post()` 和分发
只比较整数。handler 表按 UUID 排序，所以分发是 `O(log n)` 二分查找，不是线性字符串扫描。

```c
/* FNV-1a, 32-bit */
uint32_t h = 0x811C9DC5u;
while (*s) { h ^= (uint8_t)*s++; h *= 0x01000193u; }
return h ? h : 1;   /* 把 0 保留为非法/哨兵 UUID */
```

还有 `embedmq_post_id()`——传入缓存好的 UUID，在最热的路径上连 hash 都省掉。

**权衡：** 两个真实代价。(a) **哈希碰撞**理论上可能——两个不同名字映射到同一个 `uint32_t`。
用 FNV-1a 加上少量事件名时这概率小到可以忽略，且 `register()` 会拒绝重复 UUID，但我确实是用
一点点正确性风险换速度。(b) **运行时无法反查**——名字一旦被 hash，embedmq 就无法还原原始字符串
用于日志。想要可读的 trace，得自己保留 name→UUID 的映射。

### 2. 零 malloc 静态模式

**问题：** MCU 和内存紧张的 Linux 系统希望避免堆碎片和不确定的分配延迟。很多平台在 init 之后
根本不能 `malloc`。

**选择：** 一套内存布局，两条创建路径。embedmq 需要的一切都在一整块连续内存里：

```
[ struct embedmq_s ][ handler 表 ][ 环形缓冲 ][ dispatch 暂存 ]
```

- `embedmq_create()` 用 `calloc` 分配这块（方便，给 Linux 用）。
- `embedmq_create_static()` 把它放进调用者提供的缓冲区——**完全不用堆**。`embedmq_mem_size(cfg)`
  告诉你缓冲区到底要多大，于是你可以把它放进 BSS 段。

```c
static embedmq_config_t cfg = { .queue_size = 4096, .max_handlers = 16 };
static uint8_t buf[ /* embedmq_mem_size(&cfg) */ 8192 ];
embedmq_t *q = embedmq_create_static(buf, sizeof(buf), &cfg);
```

**权衡：** 单块布局意味着配置在**创建时即固定**——不重建就无法扩大环形缓冲或增加 handler 槽位。
对一个嵌入式事件总线来说这是对的默认（确定、有界），但相比能动态扩容的结构，这确实是个约束。

我刻意把 `embedmq_mem_size()` 做成运行时函数而不是编译期宏：宏版本得在公开头文件里暴露
`sizeof(struct embedmq_s)`，泄露内部实现。函数版本能让结构体保持不透明。

### 3. 信号量唤醒而非忙等

**问题：** 消费者需要等待消息。空转会烧 CPU 和电——嵌入式设备上不可接受。

**选择：** 用计数信号量。队列空时消费者阻塞在 `sem_take()`；每次 `post()` 做一次 `sem_give()`。
计数值追踪未消费的消息数，于是消费者需要时才醒、不需要时就睡。这和 FreeRTOS 队列内部用的是
同一套模式。

**权衡：** 每条消息一次信号量操作并非免费——对极高频、单线程、不在乎延迟的负载，批量/轮询设计
能有更少的系统调用开销。embedmq 优化的是常见场景（大部分时间空闲、有活时及时唤醒）。裸机 PAL
则完全绕开这个：没有线程、没有信号量——你在超级循环里调 `embedmq_poll()`。

### 4. 环形缓冲的 wrap 处理

**问题：** 靠近缓冲区末尾的消息可能跨越 wrap 点——它的字节被切成两段，分布在缓冲区的尾和头。

**选择：** 用**两次 `memcpy`** 透明处理：先拷到缓冲区末尾，再从头拷剩余部分。没有填充、没有
跳过的尾部区域、调用方也没有"wrap 前还放得下吗"的特判。

**权衡：** 我考虑过哨兵/填充方案（放不下就跳到开头）。那能让每条消息保持连续——对零拷贝读取更
友好——但浪费尾部字节，并给每次写入加一个分支。两次 memcpy 方案把缓冲区用满、逻辑集中在一处；
代价是 wrap 的消息要分两段拷而不是一段。对小消息可以忽略。

### 5. 是 PAL，不是 HAL

**问题：** 同一份 core 要跑在 pthreads、FreeRTOS 和裸机上。它们的差异在于 **OS 原语**，不是硬件。

**选择：** 一个**平台抽象层（PAL）**：一个小接口（`pal/embedmq_pal.h`），8 个函数——计数信号量、
互斥、线程——每个平台一份实现，编译期选择：

```
pal/linux/     pthread_mutex + POSIX sem + pthread
pal/freertos/  xSemaphoreCreateCounting/Mutex + xTaskCreate
pal/none/      C11 原子 spinlock；无线程（用 embedmq_poll()）
```

我刻意叫它 PAL 而不是 **HAL**。*硬件*抽象层抽象的是 GPIO、SPI、定时器——硅片。embedmq 从不碰
硬件；它抽象的是 **OS 原语**（线程、信号量）。把它叫 HAL 是用词不当，而且嵌入式的人一眼能看出来。

**权衡：** 这层抽象是三个差异很大的 OS 的最小公约数，所以无法暴露平台专属的好东西（FreeRTOS 的
task notification、Linux 的 `eventfd`、优先级继承调优）。加一个新 RTOS 很便宜——实现 8 个函数——
但 core 也无法利用这 8 个之外的任何能力。

---

## 移植到 FreeRTOS：踩坑实录

Linux 和裸机后端都很直白。FreeRTOS 才是抽象真正受考验的地方——有意思的 bug 也都在这。我在
**FreeRTOS POSIX 端口（GCC_POSIX）** 上验证了一切，它在 host 上跑真实的 FreeRTOS 调度器、不需要
硬件（见[验证方式](#验证方式)）。

### FreeRTOS 任务函数绝不能 return

embedmq 的消费者循环在队列销毁时会 `break` 出来并 return——用 pthreads 完全没问题。在 FreeRTOS
上，一个 return 的任务函数是未定义行为（会触发 `configASSERT` / 崩溃）。任务必须用
`vTaskDelete(NULL)` 自删。

所以 FreeRTOS PAL 用一个 trampoline 把消费者包起来：

```c
static void task_trampoline(void *param) {
    embedmq_pal_thread_t *t = param;
    t->fn(t->arg);            /* 跑消费者循环…… */
    xSemaphoreGive(t->done);  /* ……发信号说我跑完了…… */
    vTaskDelete(NULL);        /* ……然后干净退出。不会 return。 */
}
```

### FreeRTOS 没有 `pthread_join`

`embedmq_destroy()` 必须阻塞到消费者真正停下来之后才能释放任何东西——否则消费者会访问已释放的
内存。用 pthreads 这是 `pthread_join()`。FreeRTOS 没有对应物。

解法就是上面那个 `done` 信号量。线程句柄不是裸的 `TaskHandle_t`——它把 join 原语一起带着：

```c
typedef struct {
    TaskHandle_t      handle;
    SemaphoreHandle_t done;   /* 任务在 vTaskDelete 之前 give 它 */
    void            (*fn)(void *);
    void             *arg;
} embedmq_pal_thread_t;
```

`join()` 只需 take `done` 并等待。任务自删，idle 任务回收它的 TCB 和栈。没有 `pthread_join`、
不用轮询任务状态，而且——因为 `fn`/`arg` 就存在句柄里——trampoline 上下文不需要任何堆分配。

### POSIX 模拟器的栈大小陷阱

这个最费时间。模拟器一启动就挂住、**零输出**——测试任务根本没跑起来。

原因：在 GCC_POSIX 端口里，FreeRTOS 的栈缓冲（`depth × sizeof(StackType_t)`，这里 `StackType_t`
是 8 字节）同时充当 pthread 栈。我把 `configMINIMAL_STACK_SIZE` 设成了 `PTHREAD_STACK_MIN`
（现代 glibc 上约 16 KB）当**深度**用——于是一个"最小"任务要了 `16384 × 8 = 128 KB`，再乘上我的
倍数，单个任务超过 0.5～1 MB。这撑爆了 FreeRTOS heap，`xTaskCreate` 静默失败，调度器就让 idle
任务空转到天荒地老。

而且这个端口本来就会把真实 pthread 栈 clamp 到至少 `PTHREAD_STACK_MIN`，所以修法是把两者解耦：
把深度写成 `PTHREAD_STACK_MIN / sizeof(StackType_t)`，并给 host 一个宽裕的 heap。

```c
#define configMINIMAL_STACK_SIZE \
    ( ( unsigned short ) ( PTHREAD_STACK_MIN / sizeof( unsigned long ) ) )
```

**教训：**"栈大小"这个数字在不同端口里含义不同。在 Cortex-M 上它是真实栈的字数；在 POSIX 端口上
它既是一次 heap 分配、又是一次 pthread 栈请求。同一个字段，不同单位。

### 验证方式

模拟器（`sim/freertos/`）把真实的 embedmq core + FreeRTOS PAL 对着内核的 POSIX 端口编译，每次
push 都在 CI 里跑——覆盖基本 post、`post_id` 热路径、并发 producer 和 destroy/join 握手。

对它能证明什么要诚实：模拟器验证的是 **PAL 逻辑**——信号量唤醒、互斥、任务创建/退出、dispatch
顺序。它**不**覆盖真实硬件时序、ISR 上下文安全、紧张 RAM 下的行为。README 写的是
"simulator-verified" 而不是 "hardware-verified"，这个区分会一直保留，直到 embedmq 真正跑在板子上。

---

## 性能

在 x86-64 Linux 上测量，Release 构建，单生产者 + 消费者线程：

| 测试项 | 结果 |
|---|---|
| `embedmq_post()` 吞吐量 | ~300 万条/秒 |
| `embedmq_post_id()` 吞吐量 | ~300 万条/秒 |
| 端到端延迟（post → handler），平均 | ~38 µs |
| 端到端延迟，最小 | ~7 µs |
| `embedmq_uuid()` hash 速度 | ~4500 万次/秒（~22 ns） |

吞吐量来自把热路径保持得足够无聊：分发是一次整数比较、写 ring 是一次 `memcpy`（wrap 时才两次）、
每条消息一次信号量操作。整条路径上没有分配、没有字符串操作、除信号量外没有系统调用。

自己跑：`cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/benchmark`。
