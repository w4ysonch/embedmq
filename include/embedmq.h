#ifndef EMBEDMQ_H
#define EMBEDMQ_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* =========================================================
 * embedmq — Embedded Message Queue
 *
 * Lightweight, zero-dependency message dispatch library for
 * embedded Linux and RTOS.
 *
 * Core mechanism:
 *   register: name → uint32_t UUID (hash once at startup)
 *   post:     UUID + payload → ring buffer → semaphore signal
 *   dispatch: consumer thread wakes → UUID lookup → handler()
 *
 * Thread safety:
 *   embedmq_post() / embedmq_post_id() are thread-safe and
 *   may be called from any thread simultaneously.
 *   embedmq_register() must be called before starting producers.
 * ========================================================= */

/* ---------------------------------------------------------
 * Return codes
 * --------------------------------------------------------- */
#define EMBEDMQ_OK      0
#define EMBEDMQ_ERR    -1   /* generic error               */
#define EMBEDMQ_FULL   -2   /* ring buffer full            */
#define EMBEDMQ_NOMEM  -3   /* out of memory               */
#define EMBEDMQ_EXIST  -4   /* handler already registered  */
#define EMBEDMQ_INVAL  -5   /* invalid argument            */

/* ---------------------------------------------------------
 * Handler function type
 *
 * Called from the internal consumer thread.
 *   data  pointer to payload (valid only during the call)
 *   size  payload size in bytes
 *   ctx   user context passed to embedmq_register()
 * --------------------------------------------------------- */
typedef void (*embedmq_handler_fn)(const void *data, size_t size, void *ctx);

/* ---------------------------------------------------------
 * Configuration
 *
 * Pass NULL to use all defaults.
 * --------------------------------------------------------- */
typedef struct {
    size_t queue_size;      /* ring buffer bytes,     default: 8192 */
    size_t max_msg_size;    /* max payload per msg,   default: 1024 */
    size_t max_handlers;    /* max registered names,  default: 64   */
    int    thread_priority; /* consumer thread prio,  0 = OS default */
} embedmq_config_t;

/* Opaque handle — internals are platform-specific */
typedef struct embedmq_s embedmq_t;

/* ---------------------------------------------------------
 * Core API
 * --------------------------------------------------------- */

/*
 * embedmq_create — create a dispatcher (dynamic / malloc mode)
 *
 * Allocates all internal state with malloc and starts the
 * consumer thread.
 *
 * cfg  configuration, or NULL for all defaults
 * Returns handle on success, NULL on failure
 */
embedmq_t *embedmq_create(const embedmq_config_t *cfg);

/*
 * embedmq_create_static — create a dispatcher (zero-malloc mode)
 *
 * All internal state is placed inside the caller-provided buffer.
 * No heap allocation is performed after this call.
 *
 * Use embedmq_mem_size() to compute the required buffer size.
 *
 * mem       caller-provided buffer, pointer-size aligned
 * mem_size  size of mem in bytes
 * cfg       must not be NULL; set queue_size and max_handlers explicitly
 * Returns handle (== mem) on success, NULL on failure
 */
embedmq_t *embedmq_create_static(void *mem, size_t mem_size,
                                  const embedmq_config_t *cfg);

/*
 * embedmq_mem_size — compute required buffer size for static mode
 *
 * Call with the same cfg you will pass to embedmq_create_static().
 * Example (BSS allocation):
 *
 *   static embedmq_config_t cfg = { .queue_size = 4096,
 *                                   .max_handlers = 16 };
 *   static uint8_t buf[4096 + 16 * 32 + 256]; // or use the function
 *   size_t needed = embedmq_mem_size(&cfg);
 *   embedmq_t *q = embedmq_create_static(buf, needed, &cfg);
 */
size_t embedmq_mem_size(const embedmq_config_t *cfg);

/*
 * embedmq_register — bind a name to a handler
 *
 * Hashes name to a UUID once at call time. Subsequent posts using
 * the same name will wake this handler.
 *
 * Must be called before any producer calls embedmq_post().
 * Not thread-safe with respect to other register calls.
 *
 * Returns EMBEDMQ_OK, EMBEDMQ_EXIST, or EMBEDMQ_ERR
 */
int embedmq_register(embedmq_t *q, const char *name,
                     embedmq_handler_fn fn, void *ctx);

/*
 * embedmq_post — enqueue an event (non-blocking, thread-safe)
 *
 * Hashes name to UUID, writes [uuid|len|payload] to the ring
 * buffer, signals the consumer thread.
 *
 * Returns EMBEDMQ_OK, EMBEDMQ_FULL, or EMBEDMQ_INVAL
 */
int embedmq_post(embedmq_t *q, const char *name,
                 const void *data, size_t size);

/*
 * embedmq_post_id — enqueue by UUID (hot-path variant)
 *
 * Skips the name→UUID hash. Obtain the UUID once with embedmq_uuid()
 * and cache it, then use this on the hot path.
 *
 * Returns EMBEDMQ_OK, EMBEDMQ_FULL, or EMBEDMQ_INVAL
 */
int embedmq_post_id(embedmq_t *q, uint32_t uuid,
                    const void *data, size_t size);

/*
 * embedmq_uuid — compute UUID for a name (stateless, pure hash)
 *
 * Returns the same uint32_t used internally by embedmq_register()
 * and embedmq_post(). Safe to call from any context.
 */
uint32_t embedmq_uuid(const char *name);

/*
 * embedmq_poll — manually dispatch pending messages (no-OS mode)
 *
 * In normal operation (Linux / RTOS with a real thread), the internal
 * consumer thread calls this automatically — you do not need it.
 *
 * In bare-metal / no-OS builds (EMBEDMQ_PAL_NONE), no thread is created.
 * Call embedmq_poll() from your superloop or a periodic task to drain
 * the ring buffer and invoke handlers.
 *
 * Returns the number of messages dispatched (0 if queue was empty).
 */
int embedmq_poll(embedmq_t *q);

/*
 * embedmq_destroy — stop the consumer thread and free resources
 *
 * Blocks until the consumer thread exits cleanly, then releases
 * all resources (dynamic mode) or zeroes the handle (static mode).
 */
void embedmq_destroy(embedmq_t *q);

#ifdef __cplusplus
}
#endif

#endif /* EMBEDMQ_H */
