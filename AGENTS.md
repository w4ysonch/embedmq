# AGENTS.md

This file provides guidance to AI coding agents (Claude Code, Cursor, etc.) when
working with code in this repository. The docs below are the **single source of
truth** — read and update them rather than duplicating their content here.

## Project Overview

**embedmq** is a zero-dependency C11 library (with a header-only C++ wrapper) for
**intra-process, thread-to-thread** message dispatch. Mechanism: UUID dispatch +
ring buffer + semaphore wakeup. MIT-licensed.

---

## Documentation map

| Doc | Covers |
|---|---|
| [README.md](README.md) / [README_CN.md](README_CN.md) | What it is, quick start, feature overview |
| [docs/API.md](docs/API.md) / [API_CN.md](docs/API_CN.md) | Full per-function API reference, lifecycle & threading rules |
| [docs/BUILD.md](docs/BUILD.md) / [BUILD_CN.md](docs/BUILD_CN.md) | All CMake options, build recipes, no-CMake integration |
| [docs/DESIGN.md](docs/DESIGN.md) / [DESIGN_CN.md](docs/DESIGN_CN.md) | Design decisions & trade-offs, architecture, FreeRTOS porting notes |

---

## Architecture in one paragraph

`register` hashes a name → `uint32_t` UUID once and stores `{uuid, fn, ctx}` in a
table sorted by UUID. `post` serializes `[uuid|len|payload]` into a ring buffer
under a mutex, then gives a semaphore. An internal consumer thread wakes,
binary-searches the UUID, and calls the handler. Runtime dispatch compares
integers only. Full reasoning, diagram, and wire format: [docs/DESIGN.md](docs/DESIGN.md).

---

## Directory structure

```
embedmq/
├── include/
│   ├── embedmq.h              ← public C API (keep free of internal types)
│   └── embedmq.hpp            ← header-only C++ wrapper (lambdas + RAII)
├── src/
│   ├── embedmq.c              ← core: create/register/post/destroy/poll
│   ├── embedmq_internal.h     ← internal structs and declarations
│   ├── embedmq_queue.c        ← ring buffer (wrap-safe read/write)
│   └── embedmq_hash.c         ← FNV-1a hash → uint32_t UUID
├── pal/
│   ├── embedmq_pal.h          ← PAL interface (8 functions)
│   ├── linux/                 ← pthread + POSIX semaphore
│   ├── freertos/              ← FreeRTOS (counting sem + task)
│   └── none/                  ← bare-metal spinlock + poll()
├── sim/
│   └── freertos/              ← FreeRTOS POSIX simulator test (FreeRTOSConfig.h, main.c, CMakeLists.txt)
├── examples/                  ← basic.c, basic_cpp.cpp, benchmark.c
├── tests/                     ← test_embedmq.c, test_embedmq_cpp.cpp
├── docs/                      ← API / BUILD / DESIGN (each EN + CN)
├── CMakeLists.txt
├── README.md / README_CN.md
└── AGENTS.md                  ← this file (CLAUDE.md symlinks here)
```

---

## PAL backends

Selected at compile time with `-DEMBEDMQ_PAL=<name>`:

| Name | Backend |
|---|---|
| `linux` (default) | pthread mutex + POSIX semaphore + pthread |
| `freertos` | `xSemaphoreCreateCounting/Mutex` + `xTaskCreate` (+ `vTaskDelete`/sem join) |
| `none` | C11 atomics spinlock; no thread — driven by `embedmq_poll()` |

PAL design rationale (and why it's a PAL, not a HAL) is in [docs/DESIGN.md](docs/DESIGN.md).

---

## Working in this repo (conventions)

- **Docs are bilingual.** Every change to an `*.md` must be mirrored in its
  `*_CN.md` counterpart (and vice versa). Each pair has language-switch links at
  the top.
- **Core stays PAL-agnostic.** Code in `src/` must only use the interface in
  `pal/embedmq_pal.h` — never call a platform primitive directly.
- **Public header stays clean.** `include/embedmq.h` must not expose internal
  types; internals live in `src/embedmq_internal.h`.
- **Honest platform claims.** FreeRTOS is verified on the POSIX simulator only —
  do not claim real-hardware support until it runs on a board.

---

## Roadmap / progress

- [x] Core (ring buffer static+dynamic, UUID registry, dispatch API)
- [x] Linux PAL · bare-metal PAL · FreeRTOS PAL
- [x] C++ wrapper (header-only, lambda + RAII)
- [x] Tests (C, C++) + concurrent stress test
- [x] CI (gcc + clang) + FreeRTOS POSIX simulator job
- [x] Benchmark (~3M msgs/sec, ~38µs latency)
- [x] Docs: README / API / BUILD / DESIGN (all EN + CN)
- [x] Release v0.1.0
- [ ] Other RTOS backends (Zephyr / ThreadX / RT-Thread) — add one at a time, each verified
- [ ] Verify on real hardware (currently simulator-only)
- [ ] v2: multi-priority queue (urgent/normal — two instances or a priority field)
```
