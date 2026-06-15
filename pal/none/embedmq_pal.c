#include "../embedmq_pal.h"

#if defined(EMBEDMQ_PAL_NONE)

/*
 * No-OS / bare-metal PAL
 *
 * There is no real thread. embedmq_create() will call thread_create(),
 * which is a no-op here — the "consumer thread" never starts.
 *
 * You MUST call embedmq_poll(q) from your superloop or a periodic
 * task to drain the ring buffer and invoke handlers.
 *
 * Example superloop:
 *
 *   while (1) {
 *       embedmq_poll(q);
 *       do_other_work();
 *   }
 *
 * Thread safety:
 *   If your platform has interrupts that call embedmq_post(), you must
 *   disable interrupts around the post() call OR guarantee that your
 *   superloop and ISR never race on the queue. The spinlock mutex below
 *   uses C11 atomics which should work on most Cortex-M targets with
 *   single-core CPUs.
 */

#include <stdatomic.h>

/* ---------------------------------------------------------
 * Semaphore — counting, implemented as an atomic int.
 * sem_take() spins (busy-wait). For battery-sensitive devices,
 * replace with a WFI/WFE yield appropriate for your CPU.
 * --------------------------------------------------------- */

int embedmq_pal_sem_create(embedmq_pal_sem_t *sem)
{
    atomic_store(sem, 0);
    return 0;
}

void embedmq_pal_sem_destroy(embedmq_pal_sem_t *sem)
{
    (void)sem;
}

void embedmq_pal_sem_give(embedmq_pal_sem_t *sem)
{
    atomic_fetch_add(sem, 1);
}

void embedmq_pal_sem_take(embedmq_pal_sem_t *sem)
{
    /* Spin until a token is available */
    int expected;
    do {
        do {
            expected = atomic_load(sem);
        } while (expected <= 0);
    } while (!atomic_compare_exchange_weak(sem, &expected, expected - 1));
}

/* ---------------------------------------------------------
 * Mutex — spinlock using C11 atomic_flag
 * --------------------------------------------------------- */

int embedmq_pal_mutex_create(embedmq_pal_mutex_t *mutex)
{
    atomic_flag_clear(mutex);
    return 0;
}

void embedmq_pal_mutex_destroy(embedmq_pal_mutex_t *mutex)
{
    (void)mutex;
}

void embedmq_pal_mutex_lock(embedmq_pal_mutex_t *mutex)
{
    while (atomic_flag_test_and_set_explicit(mutex, memory_order_acquire))
        ; /* spin */
}

void embedmq_pal_mutex_unlock(embedmq_pal_mutex_t *mutex)
{
    atomic_flag_clear_explicit(mutex, memory_order_release);
}

/* ---------------------------------------------------------
 * Thread — no-op. embedmq_poll() drives dispatch instead.
 * --------------------------------------------------------- */

int embedmq_pal_thread_create(embedmq_pal_thread_t *thread,
                               void (*fn)(void *), void *arg,
                               int priority)
{
    (void)thread; (void)fn; (void)arg; (void)priority;
    return 0; /* success — no thread started, poll() drives dispatch */
}

void embedmq_pal_thread_join(embedmq_pal_thread_t *thread)
{
    (void)thread; /* nothing to join */
}

#endif /* EMBEDMQ_PAL_NONE */
