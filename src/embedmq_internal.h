#ifndef EMBEDMQ_INTERNAL_H
#define EMBEDMQ_INTERNAL_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "../include/embedmq.h"
#include "../pal/embedmq_pal.h"

/* ---------------------------------------------------------
 * Handler registry entry
 * --------------------------------------------------------- */
typedef struct {
    uint32_t           uuid;
    embedmq_handler_fn fn;
    void              *ctx;
} embedmq_handler_entry_t;

/* ---------------------------------------------------------
 * Core dispatcher state
 *
 * Memory layout (static mode):
 *   [ struct embedmq_s ][ handler table ][ ring buffer ][ dispatch buf ]
 * --------------------------------------------------------- */
struct embedmq_s {
    /* PAL handles */
    embedmq_pal_sem_t    sem;     /* counts pending messages      */
    embedmq_pal_mutex_t  mutex;   /* protects ring buffer writes  */
    embedmq_pal_thread_t thread;

    /* Handler registry — sorted by uuid for binary search */
    embedmq_handler_entry_t *handlers;
    size_t                   handler_count;
    size_t                   max_handlers;

    /* Ring buffer */
    uint8_t *buf;
    size_t   buf_size;
    size_t   head;   /* read pos  — only touched by consumer thread */
    size_t   tail;   /* write pos — protected by mutex              */

    /* Scratch buffer for copying payload before calling handler.
     * Avoids holding mutex during handler execution. */
    uint8_t *dispatch_buf;
    size_t   max_msg_size;

    /* Lifecycle */
    atomic_int running;
    int        is_static; /* 0 = malloc'd, 1 = static mem */
};

/* ---------------------------------------------------------
 * Internal function declarations (implemented in embedmq_queue.c)
 * --------------------------------------------------------- */
int  emq_ring_write(embedmq_t *q, uint32_t uuid,
                    const void *data, size_t size);
int  emq_ring_read(embedmq_t *q, uint32_t *uuid_out,
                   void *buf, size_t *size_out);
size_t emq_ring_free(const embedmq_t *q);

/* ---------------------------------------------------------
 * Internal function declarations (implemented in embedmq_hash.c)
 * --------------------------------------------------------- */
uint32_t emq_hash(const char *name);

#endif /* EMBEDMQ_INTERNAL_H */
