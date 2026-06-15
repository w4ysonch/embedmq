#ifndef EMBEDMQ_PAL_H
#define EMBEDMQ_PAL_H

/* ---------------------------------------------------------
 * Platform Abstraction Layer (PAL)
 *
 * Provides semaphore, mutex, and thread primitives.
 * Select a backend by defining one of:
 *
 *   EMBEDMQ_PAL_LINUX    — pthreads + POSIX semaphore (default)
 *   EMBEDMQ_PAL_FREERTOS — FreeRTOS primitives
 *   EMBEDMQ_PAL_NONE     — no-OS spinlock; user drives dispatch
 *                          via embedmq_poll() in their superloop
 *
 * If none is defined, EMBEDMQ_PAL_LINUX is assumed.
 * --------------------------------------------------------- */

#if !defined(EMBEDMQ_PAL_LINUX)    && \
    !defined(EMBEDMQ_PAL_FREERTOS) && \
    !defined(EMBEDMQ_PAL_NONE)
#define EMBEDMQ_PAL_LINUX
#endif

/* ---- Linux / pthreads ---------------------------------- */
#if defined(EMBEDMQ_PAL_LINUX)

#include <pthread.h>
#include <semaphore.h>

typedef sem_t           embedmq_pal_sem_t;
typedef pthread_mutex_t embedmq_pal_mutex_t;
typedef pthread_t       embedmq_pal_thread_t;

/* ---- FreeRTOS ------------------------------------------ */
#elif defined(EMBEDMQ_PAL_FREERTOS)

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

typedef SemaphoreHandle_t embedmq_pal_sem_t;
typedef SemaphoreHandle_t embedmq_pal_mutex_t;
typedef TaskHandle_t      embedmq_pal_thread_t;

/* ---- No OS (polling / superloop) ----------------------- */
#elif defined(EMBEDMQ_PAL_NONE)

#include <stdatomic.h>

typedef atomic_int  embedmq_pal_sem_t;
typedef atomic_flag embedmq_pal_mutex_t;
typedef void       *embedmq_pal_thread_t; /* unused */

#endif /* PAL selection */

/* ---------------------------------------------------------
 * PAL interface — implemented per-platform in
 * pal/<platform>/embedmq_pal.c
 * --------------------------------------------------------- */

/* Semaphore — counting, initially 0 */
int  embedmq_pal_sem_create(embedmq_pal_sem_t *sem);
void embedmq_pal_sem_destroy(embedmq_pal_sem_t *sem);
void embedmq_pal_sem_give(embedmq_pal_sem_t *sem);
void embedmq_pal_sem_take(embedmq_pal_sem_t *sem); /* blocks until > 0 */

/* Mutex */
int  embedmq_pal_mutex_create(embedmq_pal_mutex_t *mutex);
void embedmq_pal_mutex_destroy(embedmq_pal_mutex_t *mutex);
void embedmq_pal_mutex_lock(embedmq_pal_mutex_t *mutex);
void embedmq_pal_mutex_unlock(embedmq_pal_mutex_t *mutex);

/* Thread (no-op in EMBEDMQ_PAL_NONE) */
int  embedmq_pal_thread_create(embedmq_pal_thread_t *thread,
                                void (*fn)(void *), void *arg,
                                int priority);
void embedmq_pal_thread_join(embedmq_pal_thread_t *thread);

#endif /* EMBEDMQ_PAL_H */
