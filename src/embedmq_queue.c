#include <string.h>
#include "embedmq_internal.h"

/* ---------------------------------------------------------
 * Ring buffer — variable-length message store
 *
 * Wire format per message:
 *   [ uint32_t uuid (4 B) ][ uint16_t length (2 B) ][ payload (length B) ]
 *
 * Messages may wrap around the end of the buffer. Both
 * emq_ring_write and emq_ring_read split the copy into at
 * most two memcpy calls to handle the wrap transparently.
 *
 * Invariants:
 *   head — advanced only by the consumer thread (no lock needed)
 *   tail — advanced only under mutex held by a producer
 *   buf_size - 1 — max usable bytes (one byte reserved to
 *                  distinguish full from empty: used == buf_size-1)
 * --------------------------------------------------------- */

/* Number of bytes currently in the buffer */
static size_t ring_used(const embedmq_t *q)
{
    if (q->tail >= q->head)
        return q->tail - q->head;
    return q->buf_size - q->head + q->tail;
}

size_t emq_ring_free(const embedmq_t *q)
{
    return q->buf_size - 1 - ring_used(q);
}

/* Copy n bytes into the ring buffer at tail, advancing tail */
static void ring_write_bytes(embedmq_t *q, const void *src, size_t n)
{
    const uint8_t *s    = (const uint8_t *)src;
    size_t         end  = q->buf_size - q->tail;

    if (end >= n) {
        memcpy(q->buf + q->tail, s, n);
        q->tail = (q->tail + n == q->buf_size) ? 0 : q->tail + n;
    } else {
        memcpy(q->buf + q->tail, s, end);
        memcpy(q->buf, s + end, n - end);
        q->tail = n - end;
    }
}

/* Copy n bytes from the ring buffer at head, advancing head */
static void ring_read_bytes(embedmq_t *q, void *dst, size_t n)
{
    uint8_t *d   = (uint8_t *)dst;
    size_t   end = q->buf_size - q->head;

    if (end >= n) {
        memcpy(d, q->buf + q->head, n);
        q->head = (q->head + n == q->buf_size) ? 0 : q->head + n;
    } else {
        memcpy(d, q->buf + q->head, end);
        memcpy(d + end, q->buf, n - end);
        q->head = n - end;
    }
}

/* ---------------------------------------------------------
 * Public ring API (called under mutex from embedmq.c)
 * --------------------------------------------------------- */

/*
 * emq_ring_write — enqueue one message
 *
 * Called with mutex held. Returns EMBEDMQ_OK or EMBEDMQ_FULL.
 */
int emq_ring_write(embedmq_t *q, uint32_t uuid,
                   const void *data, size_t size)
{
    size_t msg_size = 4 + 2 + size; /* uuid + len + payload */

    if (emq_ring_free(q) < msg_size)
        return EMBEDMQ_FULL;

    /* Write header: uuid (4 B) + length (2 B) */
    uint8_t hdr[6];
    uint16_t len16 = (uint16_t)size;
    memcpy(hdr,     &uuid,  4);
    memcpy(hdr + 4, &len16, 2);
    ring_write_bytes(q, hdr, 6);

    /* Write payload */
    if (size > 0)
        ring_write_bytes(q, data, size);

    return EMBEDMQ_OK;
}

/*
 * emq_ring_read — dequeue one message
 *
 * Called with mutex held. Copies payload into buf (which must be
 * at least max_msg_size bytes). Returns EMBEDMQ_OK or EMBEDMQ_ERR.
 */
int emq_ring_read(embedmq_t *q, uint32_t *uuid_out,
                  void *buf, size_t *size_out)
{
    if (ring_used(q) < 6)
        return EMBEDMQ_ERR;

    /* Read header */
    uint8_t hdr[6];
    ring_read_bytes(q, hdr, 6);

    uint32_t uuid;
    uint16_t len16;
    memcpy(&uuid,  hdr,     4);
    memcpy(&len16, hdr + 4, 2);

    size_t payload = (size_t)len16;

    if (ring_used(q) < payload)
        return EMBEDMQ_ERR; /* corrupt state — should never happen */

    if (payload > 0)
        ring_read_bytes(q, buf, payload);

    *uuid_out = uuid;
    *size_out = payload;
    return EMBEDMQ_OK;
}
