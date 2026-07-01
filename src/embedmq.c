#include <stdlib.h>
#include <string.h>

#include "embedmq_internal.h"

/* ---------------------------------------------------------
 * Default configuration values
 * --------------------------------------------------------- */
#define DEFAULT_QUEUE_SIZE    8192
#define DEFAULT_MAX_MSG_SIZE  1024
#define DEFAULT_MAX_HANDLERS  64

static void fill_defaults(embedmq_config_t *cfg)
{
    if (cfg->queue_size    == 0) cfg->queue_size    = DEFAULT_QUEUE_SIZE;
    if (cfg->max_msg_size  == 0) cfg->max_msg_size  = DEFAULT_MAX_MSG_SIZE;
    if (cfg->max_handlers  == 0) cfg->max_handlers  = DEFAULT_MAX_HANDLERS;
}

/* ---------------------------------------------------------
 * Memory layout (static mode)
 *   [ struct embedmq_s ][ handler table ][ ring buffer ][ dispatch buf ]
 * --------------------------------------------------------- */

size_t embedmq_mem_size(const embedmq_config_t *cfg)
{
    if (!cfg) return 0;
    return sizeof(struct embedmq_s)
         + cfg->max_handlers * sizeof(embedmq_handler_entry_t)
         + cfg->queue_size
         + cfg->max_msg_size; /* dispatch scratch buffer */
}

/* ---------------------------------------------------------
 * Dispatch one message from the ring buffer (shared by
 * the consumer thread and embedmq_poll).
 *
 * Returns 1 if a message was dispatched, 0 if queue empty.
 * --------------------------------------------------------- */

static int dispatch_one(embedmq_t *q)
{
    uint32_t uuid;
    size_t   size;

    embedmq_pal_mutex_lock(&q->mutex);
    int r = emq_ring_read(q, &uuid, q->dispatch_buf, &size);
    embedmq_pal_mutex_unlock(&q->mutex);

    if (r != EMBEDMQ_OK)
        return 0;

    /* Binary search handler table (sorted by uuid) */
    size_t lo = 0, hi = q->handler_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (q->handlers[mid].uuid == uuid) {
            q->handlers[mid].fn(q->dispatch_buf, size,
                                q->handlers[mid].ctx);
            return 1;
        } else if (q->handlers[mid].uuid < uuid) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 1; /* message consumed even if no handler matched */
}

/* ---------------------------------------------------------
 * Consumer thread (Linux / RTOS mode)
 * --------------------------------------------------------- */

static void consumer_thread(void *arg)
{
    embedmq_t *q = (embedmq_t *)arg;

    while (1) {
        embedmq_pal_sem_take(&q->sem);
        if (!atomic_load(&q->running)) {
            while (dispatch_one(q))
                ;
            break;
        }
        dispatch_one(q);
    }
}

/* ---------------------------------------------------------
 * Common init path (shared by create and create_static)
 * --------------------------------------------------------- */

static int init_handle(embedmq_t *q, const embedmq_config_t *cfg)
{
    q->handler_count = 0;
    q->max_handlers  = cfg->max_handlers;
    q->buf_size      = cfg->queue_size;
    q->max_msg_size  = cfg->max_msg_size;
    q->head          = 0;
    q->tail          = 0;
    atomic_store(&q->running, 1);

    if (embedmq_pal_sem_create(&q->sem) != 0)
        return -1;
    if (embedmq_pal_mutex_create(&q->mutex) != 0) {
        embedmq_pal_sem_destroy(&q->sem);
        return -1;
    }
    if (embedmq_pal_thread_create(&q->thread, consumer_thread, q,
                                  cfg->thread_priority) != 0) {
        embedmq_pal_mutex_destroy(&q->mutex);
        embedmq_pal_sem_destroy(&q->sem);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------
 * Public: create (dynamic mode)
 * --------------------------------------------------------- */

embedmq_t *embedmq_create(const embedmq_config_t *cfg)
{
    embedmq_config_t c = cfg ? *cfg : (embedmq_config_t){0};
    fill_defaults(&c);

    size_t total = embedmq_mem_size(&c);
    uint8_t *mem = calloc(1, total);
    if (!mem) return NULL;

    embedmq_t *q = (embedmq_t *)mem;
    mem += sizeof(struct embedmq_s);

    q->handlers      = (embedmq_handler_entry_t *)mem;
    mem += c.max_handlers * sizeof(embedmq_handler_entry_t);

    q->buf           = mem;
    mem += c.queue_size;

    q->dispatch_buf  = mem;

    q->is_static = 0;

    if (init_handle(q, &c) != 0) {
        free(q);
        return NULL;
    }
    return q;
}

/* ---------------------------------------------------------
 * Public: create_static (zero-malloc mode)
 * --------------------------------------------------------- */

embedmq_t *embedmq_create_static(void *mem, size_t mem_size,
                                  const embedmq_config_t *cfg)
{
    if (!mem || !cfg) return NULL;

    embedmq_config_t c = *cfg;
    if (c.queue_size == 0 || c.max_handlers == 0) return NULL;
    if (c.max_msg_size == 0) c.max_msg_size = DEFAULT_MAX_MSG_SIZE;

    if (mem_size < embedmq_mem_size(&c)) return NULL;

    uint8_t *p = (uint8_t *)mem;
    memset(p, 0, mem_size);

    embedmq_t *q = (embedmq_t *)p;
    p += sizeof(struct embedmq_s);

    q->handlers     = (embedmq_handler_entry_t *)p;
    p += c.max_handlers * sizeof(embedmq_handler_entry_t);

    q->buf          = p;
    p += c.queue_size;

    q->dispatch_buf = p;

    q->is_static = 1;

    if (init_handle(q, &c) != 0)
        return NULL;

    return q;
}

/* ---------------------------------------------------------
 * Public: register
 * --------------------------------------------------------- */

int embedmq_register(embedmq_t *q, const char *name,
                     embedmq_handler_fn fn, void *ctx)
{
    if (!q || !name || !fn) return EMBEDMQ_INVAL;
    if (q->handler_count >= q->max_handlers) return EMBEDMQ_ERR;

    uint32_t uuid = embedmq_uuid(name);

    /* Check for duplicate and find insertion point (keep sorted by uuid) */
    size_t i = 0;
    while (i < q->handler_count) {
        if (q->handlers[i].uuid == uuid)
            return EMBEDMQ_EXIST;
        if (q->handlers[i].uuid > uuid)
            break;
        i++;
    }

    /* Shift right to make room */
    if (i < q->handler_count)
        memmove(&q->handlers[i + 1], &q->handlers[i],
                (q->handler_count - i) * sizeof(embedmq_handler_entry_t));

    q->handlers[i].uuid = uuid;
    q->handlers[i].fn   = fn;
    q->handlers[i].ctx  = ctx;
    q->handler_count++;

    return EMBEDMQ_OK;
}

/* ---------------------------------------------------------
 * Public: post (by name)
 * --------------------------------------------------------- */

int embedmq_post(embedmq_t *q, const char *name,
                 const void *data, size_t size)
{
    if (!q || !name) return EMBEDMQ_INVAL;
    return embedmq_post_id(q, embedmq_uuid(name), data, size);
}

/* ---------------------------------------------------------
 * Public: post_id (by UUID — hot path)
 * --------------------------------------------------------- */

int embedmq_post_id(embedmq_t *q, uint32_t uuid,
                    const void *data, size_t size)
{
    if (!q || uuid == 0) return EMBEDMQ_INVAL;
    if (size > q->max_msg_size) return EMBEDMQ_INVAL;
    if (size > 0 && !data) return EMBEDMQ_INVAL;

    embedmq_pal_mutex_lock(&q->mutex);
    int r = emq_ring_write(q, uuid, data, size);
    embedmq_pal_mutex_unlock(&q->mutex);

    if (r == EMBEDMQ_OK)
        embedmq_pal_sem_give(&q->sem);

    return r;
}

/* ---------------------------------------------------------
 * Public: poll (no-OS / bare-metal mode)
 * --------------------------------------------------------- */

int embedmq_poll(embedmq_t *q)
{
    if (!q) return 0;
    int n = 0;
    while (dispatch_one(q))
        n++;
    return n;
}

/* ---------------------------------------------------------
 * Public: destroy
 * --------------------------------------------------------- */

void embedmq_destroy(embedmq_t *q)
{
    if (!q) return;

    /* Signal consumer to exit */
    atomic_store(&q->running, 0);
    embedmq_pal_sem_give(&q->sem);

    embedmq_pal_thread_join(&q->thread);
    embedmq_pal_mutex_destroy(&q->mutex);
    embedmq_pal_sem_destroy(&q->sem);

    if (!q->is_static)
        free(q);
    else
        memset(q, 0, sizeof(struct embedmq_s)); /* reset control block */
}
