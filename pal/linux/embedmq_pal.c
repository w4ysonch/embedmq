#include "../embedmq_pal.h"

#if defined(EMBEDMQ_PAL_LINUX)

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------
 * Semaphore (POSIX unnamed sem, counting, initial value 0)
 * --------------------------------------------------------- */

int embedmq_pal_sem_create(embedmq_pal_sem_t *sem)
{
    if (sem_init(sem, 0, 0) != 0) {
        perror("embedmq: sem_init");
        return -1;
    }
    return 0;
}

void embedmq_pal_sem_destroy(embedmq_pal_sem_t *sem)
{
    sem_destroy(sem);
}

void embedmq_pal_sem_give(embedmq_pal_sem_t *sem)
{
    sem_post(sem);
}

void embedmq_pal_sem_take(embedmq_pal_sem_t *sem)
{
    int r;
    do {
        r = sem_wait(sem);
    } while (r != 0 && errno == EINTR); /* retry on signal interrupt */
}

/* ---------------------------------------------------------
 * Mutex (pthread_mutex, default attributes)
 * --------------------------------------------------------- */

int embedmq_pal_mutex_create(embedmq_pal_mutex_t *mutex)
{
    if (pthread_mutex_init(mutex, NULL) != 0) {
        perror("embedmq: pthread_mutex_init");
        return -1;
    }
    return 0;
}

void embedmq_pal_mutex_destroy(embedmq_pal_mutex_t *mutex)
{
    pthread_mutex_destroy(mutex);
}

void embedmq_pal_mutex_lock(embedmq_pal_mutex_t *mutex)
{
    pthread_mutex_lock(mutex);
}

void embedmq_pal_mutex_unlock(embedmq_pal_mutex_t *mutex)
{
    pthread_mutex_unlock(mutex);
}

/* ---------------------------------------------------------
 * Thread (pthread)
 *
 * The PAL thread function signature is void (*)(void *),
 * but pthread expects void *(*)(void *), so we wrap it.
 * --------------------------------------------------------- */

typedef struct {
    void (*fn)(void *);
    void *arg;
} thread_trampoline_t;

static void *thread_trampoline(void *arg)
{
    thread_trampoline_t *t = (thread_trampoline_t *)arg;
    void (*fn)(void *) = t->fn;
    void *user_arg = t->arg;
    free(t);
    fn(user_arg);
    return NULL;
}

int embedmq_pal_thread_create(embedmq_pal_thread_t *thread,
                               void (*fn)(void *), void *arg,
                               int priority)
{
    thread_trampoline_t *t = malloc(sizeof(*t));
    if (!t) return -1;
    t->fn  = fn;
    t->arg = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);

    if (priority != 0) {
        struct sched_param sp = { .sched_priority = priority };
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &sp);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    }

    int r = pthread_create(thread, &attr, thread_trampoline, t);
    pthread_attr_destroy(&attr);
    if (r != 0) {
        free(t);
        return -1;
    }
    return 0;
}

void embedmq_pal_thread_join(embedmq_pal_thread_t *thread)
{
    pthread_join(*thread, NULL);
}

#endif /* EMBEDMQ_PAL_LINUX */
