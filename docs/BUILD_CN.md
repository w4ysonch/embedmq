# 构建 embedmq

**[English ->](BUILD.md)**

如何构建库、示例、测试和 FreeRTOS 模拟器，以及如何把 embedmq 集成进不用 CMake 的项目。

- [环境要求](#环境要求)
- [快速开始](#快速开始)
- [CMake 选项](#cmake-选项)
- [构建配方](#构建配方)
- [构建目标](#构建目标)
- [运行测试](#运行测试)
- [不用 CMake 集成](#不用-cmake-集成)
- [实用提示](#实用提示)

---

## 环境要求

| | 最低 |
|---|---|
| CMake | 3.14+（FreeRTOS 模拟器用到 FetchContent） |
| C 编译器 | C11（gcc / clang） |
| C++ 编译器 | C++14（仅 C++ wrapper / 其测试需要） |
| 网络 | 仅 `EMBEDMQ_BUILD_FREERTOS_SIM=ON` 时需要（拉取 FreeRTOS-Kernel） |

---

## 快速开始

```bash
cmake -B build
cmake --build build
ctest --test-dir build      # 跑全部测试
```

---

## CMake 选项

配置时以 `-D<选项>=<值>` 传入。

| 选项 | 取值 | 默认 | 作用 |
|---|---|---|---|
| `EMBEDMQ_PAL` | `linux` / `freertos` / `none` | `linux` | 选 PAL 后端（`pal/<值>/embedmq_pal.c`）并设 `EMBEDMQ_PAL_<大写>` 编译宏。 |
| `EMBEDMQ_BUILD_EXAMPLES` | `ON` / `OFF` | `ON` | 是否编译示例（basic、benchmark、basic_cpp）。 |
| `EMBEDMQ_BUILD_TESTS` | `ON` / `OFF` | `ON` | 是否编译测试可执行文件。 |
| `EMBEDMQ_BUILD_FREERTOS_SIM` | `ON` / `OFF` | `OFF` | 拉取 FreeRTOS-Kernel 并编译 POSIX 模拟器测试。 |
| `EMBEDMQ_ENABLE_TSAN` | `ON` / `OFF` | `OFF` | 启用 ThreadSanitizer（需 GCC/Clang 11+），检测数据竞争。与 ASan 互斥。 |
| `EMBEDMQ_ENABLE_ASAN` | `ON` / `OFF` | `OFF` | 启用 AddressSanitizer，检测内存错误。与 TSan 互斥。 |
| `CMAKE_BUILD_TYPE` | `Debug` / `Release` / … | 空 | 标准 CMake 选项。跑 benchmark 用 `Release`。 |
| `CMAKE_C_COMPILER` | `gcc` / `clang` / … | 系统默认 | 选 C 编译器。 |

选项存在构建目录的 CMake cache 里——设过一次后，重新 configure 不必再传 `-D`。删掉构建目录即可重置。

---

## 构建配方

### 默认（Linux PAL，含示例和测试）
```bash
cmake -B build
cmake --build build
```

### Benchmark（必须 Release，否则数据没意义）
```bash
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
./build-rel/benchmark
```

### FreeRTOS POSIX 模拟器（无需硬件）
拉取 FreeRTOS-Kernel（固定到 V11.1.0），用 `GCC_POSIX` 端口编译。
```bash
cmake -B build-frt -DEMBEDMQ_BUILD_FREERTOS_SIM=ON
cmake --build build-frt
./build-frt/sim/freertos/test_embedmq_freertos      # 打印 PASS，退出码 0
```

### 裸机 / 无 OS（none PAL）
`none` 后端没有消费者线程——你用 `embedmq_poll()` 驱动分发。必须关掉测试（见[注意事项](#注意none-pal--测试)）：
```bash
cmake -B build-none -DEMBEDMQ_PAL=none -DEMBEDMQ_BUILD_TESTS=OFF
cmake --build build-none
```

### 用 clang 编译（CI 就这么做）
```bash
cmake -B build-clang -DCMAKE_C_COMPILER=clang
cmake --build build-clang
```

### 只编库（把 embedmq 当依赖用时）
```bash
cmake -B build
cmake --build build --target embedmq        # 生成 libembedmq.a
```

---

## 构建目标

| 目标 | 来源 | 何时构建 |
|---|---|---|
| `embedmq` | 静态库 `libembedmq.a` | 总是 |
| `example_basic` | `examples/basic.c` | `EMBEDMQ_BUILD_EXAMPLES=ON` |
| `example_basic_cpp` | `examples/basic_cpp.cpp` | `EMBEDMQ_BUILD_EXAMPLES=ON` |
| `benchmark` | `examples/benchmark.c` | `EMBEDMQ_BUILD_EXAMPLES=ON` |
| `test_embedmq` | `tests/test_embedmq.c` | `EMBEDMQ_BUILD_TESTS=ON` |
| `test_embedmq_cpp` | `tests/test_embedmq_cpp.cpp` | `EMBEDMQ_BUILD_TESTS=ON` |
| `test_embedmq_freertos` | `sim/freertos/main.c` | `EMBEDMQ_BUILD_FREERTOS_SIM=ON` |

单独编某个目标：`cmake --build build --target <名字>`。

---

## 运行测试

```bash
ctest --test-dir build              # 跑已注册的 C 与 C++ 测试
# 或直接跑可执行文件：
./build/test_embedmq                # C 测试（含 4 生产者压力测试）
./build/test_embedmq_cpp            # C++ wrapper 测试
```

FreeRTOS 模拟器测试（独立构建，见上面配方）：
```bash
./build-frt/sim/freertos/test_embedmq_freertos
```

---

## 不用 CMake 集成

embedmq 不依赖任何构建系统——直接编译源文件即可。把以下文件拷进你的项目一起编译：

```
src/embedmq.c
src/embedmq_hash.c
src/embedmq_queue.c
pal/<平台>/embedmq_pal.c      # linux | freertos | none
```

编译 flag：
```
-Iinclude -Ipal -DEMBEDMQ_PAL_<平台大写>
```
`<平台大写>` 是 `LINUX`、`FREERTOS` 或 `NONE`。Linux PAL 另需链接 `-lpthread`。
FreeRTOS PAL 则要求你的构建已在 include 路径上提供 `FreeRTOS.h`、`task.h`、`semphr.h`。

示例（Linux，手动编译）：
```bash
cc -Iinclude -Ipal -DEMBEDMQ_PAL_LINUX \
   src/embedmq.c src/embedmq_hash.c src/embedmq_queue.c \
   pal/linux/embedmq_pal.c your_app.c -lpthread -o your_app
```

---

## 实用提示

### 注意：none PAL + 测试
`tests/test_embedmq.c` 的并发压力测试直接起了裸 POSIX 线程，因此它只能链接 Linux PAL。用
`-DEMBEDMQ_PAL=none` 编译测试会在链接期失败（`undefined reference to pthread_create`）。
用 `-DEMBEDMQ_PAL=none` 时务必同时加 `-DEMBEDMQ_BUILD_TESTS=OFF`。`none` 的库和示例本身能正常编译。

### 用不同的构建目录
每种配置用独立目录（`build`、`build-rel`、`build-frt` …），互不覆盖，也省去反复重配。
所有 `build*/` 目录都已被 gitignore 忽略。

### IDE 跳转（clangd / VS Code）
生成编译数据库并软链到项目根目录：
```bash
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -sf build/compile_commands.json .
```
