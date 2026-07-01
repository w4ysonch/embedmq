# Building embedmq

**[中文 ->](BUILD_CN.md)**

How to build the library, examples, tests, and the FreeRTOS simulator,
plus how to drop embedmq into a project that doesn't use CMake.

- [Requirements](#requirements)
- [Quick start](#quick-start)
- [CMake options](#cmake-options)
- [Build recipes](#build-recipes)
- [Build targets](#build-targets)
- [Running tests](#running-tests)
- [Integrating without CMake](#integrating-without-cmake)
- [Tips](#tips)

---

## Requirements

| | Minimum |
|---|---|
| CMake | 3.14+ (FetchContent is used for the FreeRTOS simulator) |
| C compiler | C11 (gcc / clang) |
| C++ compiler | C++14 (only for the C++ wrapper / its test) |
| Internet | only for `EMBEDMQ_BUILD_FREERTOS_SIM=ON` (fetches FreeRTOS-Kernel) |

---

## Quick start

```bash
cmake -B build
cmake --build build
ctest --test-dir build      # run all tests
```

---

## CMake options

Pass each as `-D<option>=<value>` at configure time.

| Option | Values | Default | Effect |
|---|---|---|---|
| `EMBEDMQ_PAL` | `linux` / `freertos` / `none` | `linux` | Selects the PAL backend (`pal/<value>/embedmq_pal.c`) and the `EMBEDMQ_PAL_<UPPER>` compile definition. |
| `EMBEDMQ_BUILD_EXAMPLES` | `ON` / `OFF` | `ON` | Build the examples (basic, benchmark, basic_cpp). |
| `EMBEDMQ_BUILD_TESTS` | `ON` / `OFF` | `ON` | Build the test executables. |
| `EMBEDMQ_BUILD_FREERTOS_SIM` | `ON` / `OFF` | `OFF` | Fetch FreeRTOS-Kernel and build the POSIX simulator test. |
| `EMBEDMQ_ENABLE_TSAN` | `ON` / `OFF` | `OFF` | Build with ThreadSanitizer (GCC/Clang 11+). Detects data races. Mutually exclusive with ASan. |
| `EMBEDMQ_ENABLE_ASAN` | `ON` / `OFF` | `OFF` | Build with AddressSanitizer. Detects memory errors. Mutually exclusive with TSan. |
| `CMAKE_BUILD_TYPE` | `Debug` / `Release` / … | (empty) | Standard CMake. Use `Release` for benchmarks. |
| `CMAKE_C_COMPILER` | `gcc` / `clang` / … | system default | Pick the C compiler. |

Options are stored in the build directory's CMake cache — once set, they persist on
re-configure without re-passing `-D`. Delete the build directory to reset.

---

## Build recipes

### Default (Linux PAL, examples + tests)
```bash
cmake -B build
cmake --build build
```

### Benchmark (must be Release, or the numbers are meaningless)
```bash
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
./build-rel/benchmark
```

### FreeRTOS POSIX simulator (no hardware required)
Fetches FreeRTOS-Kernel (pinned to V11.1.0) and builds against the `GCC_POSIX` port.
```bash
cmake -B build-frt -DEMBEDMQ_BUILD_FREERTOS_SIM=ON
cmake --build build-frt
./build-frt/sim/freertos/test_embedmq_freertos      # prints PASS, exit 0
```

### Bare-metal / no-OS (none PAL)
The `none` backend has no consumer thread — you drive dispatch with `embedmq_poll()`.
You must turn tests off (see [caveat](#caveat-none-pal--tests)):
```bash
cmake -B build-none -DEMBEDMQ_PAL=none -DEMBEDMQ_BUILD_TESTS=OFF
cmake --build build-none
```

### Build with clang (what CI does)
```bash
cmake -B build-clang -DCMAKE_C_COMPILER=clang
cmake --build build-clang
```

### Library only (when consuming embedmq as a dependency)
```bash
cmake -B build
cmake --build build --target embedmq        # produces libembedmq.a
```

---

## Build targets

| Target | Source | Built when |
|---|---|---|
| `embedmq` | static lib `libembedmq.a` | always |
| `example_basic` | `examples/basic.c` | `EMBEDMQ_BUILD_EXAMPLES=ON` |
| `example_basic_cpp` | `examples/basic_cpp.cpp` | `EMBEDMQ_BUILD_EXAMPLES=ON` |
| `benchmark` | `examples/benchmark.c` | `EMBEDMQ_BUILD_EXAMPLES=ON` |
| `test_embedmq` | `tests/test_embedmq.c` | `EMBEDMQ_BUILD_TESTS=ON` |
| `test_embedmq_cpp` | `tests/test_embedmq_cpp.cpp` | `EMBEDMQ_BUILD_TESTS=ON` |
| `test_embedmq_freertos` | `sim/freertos/main.c` | `EMBEDMQ_BUILD_FREERTOS_SIM=ON` |

Build one target with `cmake --build build --target <name>`.

---

## Running tests

```bash
ctest --test-dir build              # run the registered C and C++ tests
# or run the executables directly:
./build/test_embedmq                # C tests (incl. 4-producer stress test)
./build/test_embedmq_cpp            # C++ wrapper tests
```

FreeRTOS simulator test (separate build, see recipe above):
```bash
./build-frt/sim/freertos/test_embedmq_freertos
```

---

## Integrating without CMake

embedmq has no build-system dependency — just compile the sources directly.
Copy these into your project and compile together:

```
src/embedmq.c
src/embedmq_hash.c
src/embedmq_queue.c
pal/<platform>/embedmq_pal.c      # linux | freertos | none
```

Compiler flags:
```
-Iinclude -Ipal -DEMBEDMQ_PAL_<PLATFORM>
```
where `<PLATFORM>` is `LINUX`, `FREERTOS`, or `NONE`. For the Linux PAL also link `-lpthread`.
For the FreeRTOS PAL, your build must already provide `FreeRTOS.h`, `task.h`, `semphr.h` on the
include path.

Example (Linux, hand-compiled):
```bash
cc -Iinclude -Ipal -DEMBEDMQ_PAL_LINUX \
   src/embedmq.c src/embedmq_hash.c src/embedmq_queue.c \
   pal/linux/embedmq_pal.c your_app.c -lpthread -o your_app
```

---

## Tips

### Caveat: none PAL + tests
`tests/test_embedmq.c` spins up raw POSIX threads for its concurrency stress test, so it only
links against the Linux PAL. Building tests with `-DEMBEDMQ_PAL=none` fails at link time
(`undefined reference to pthread_create`). Always pair `-DEMBEDMQ_PAL=none` with
`-DEMBEDMQ_BUILD_TESTS=OFF`. The `none` library and examples build fine.

### Separate build directories
Use a distinct directory per configuration (`build`, `build-rel`, `build-frt`, …) so they don't
clobber each other and you avoid repeated reconfigures. All `build*/` directories are gitignored.

### IDE navigation (clangd / VS Code)
Generate a compile database and symlink it to the project root:
```bash
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -sf build/compile_commands.json .
```
