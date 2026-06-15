#include "../embedmq_pal.h"

#if defined(EMBEDMQ_PAL_FREERTOS)

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* ---------------------------------------------------------
 * Tunables (override with -D... at compile time)
 *
 *   EMBEDMQ_FREERTOS_TASK_STACK     consumer task stack depth (words)
 *   EMBEDMQ_FREERTOS_SEM_MAX_COUNT  counting-semaphore max value; must be
 *                                   >= the number of messages that can be
 *                                   in flight. A FreeRTOS counting semaphore
 *                                   only allocates a control block, not
 *                                   max_count items, so a large value is cheap.
 * --------------------------------------------------------- */
#ifndef EMBEDMQ_FREERTOS_TASK_STACK
#define EMBEDMQ_FREERTOS_TASK_STACK   (configMINIMAL_STACK_SIZE * 4)
#endif

#ifndef EMBEDMQ_FREERTOS_SEM_MAX_COUNT
#define EMBEDMQ_FREERTOS_SEM_MAX_COUNT  ((UBaseType_t)0x10000)
#endif

/* ---------------------------------------------------------
 * Semaphore — counting, initial value 0
 *
 * Matches the POSIX sem_t semantics used by the Linux PAL:
 * each give() increments, each take() blocks until > 0.
 * --------------------------------------------------------- */

int embedmq_pal_sem_create(embedmq_pal_sem_t *sem)
{
    *sem = xSemaphoreCreateCounting(EMBEDMQ_FREERTOS_SEM_MAX_COUNT, 0);
    return (*sem != NULL) ? 0 : -1;
}

void embedmq_pal_sem_destroy(embedmq_pal_sem_t *sem)
{
    if (*sem) {
        vSemaphoreDelete(*sem);
        *sem = NULL;
    }
}

void embedmq_pal_sem_give(embedmq_pal_sem_t *sem)
{
    xSemaphoreGive(*sem);
}

void embedmq_pal_sem_take(embedmq_pal_sem_t *sem)
{
    xSemaphoreTake(*sem, portMAX_DELAY); /* block until available */
}

/* ---------------------------------------------------------
 * Mutex (FreeRTOS mutex semaphore)
 * --------------------------------------------------------- */

int embedmq_pal_mutex_create(embedmq_pal_mutex_t *mutex)
{
    *mutex = xSemaphoreCreateMutex();
    return (*mutex != NULL) ? 0 : -1;
}

void embedmq_pal_mutex_destroy(embedmq_pal_mutex_t *mutex)
{
    if (*mutex) {
        vSemaphoreDelete(*mutex);
        *mutex = NULL;
    }
}

void embedmq_pal_mutex_lock(embedmq_pal_mutex_t *mutex)
{
    xSemaphoreTake(*mutex, portMAX_DELAY);
}

void embedmq_pal_mutex_unlock(embedmq_pal_mutex_t *mutex)
{
    xSemaphoreGive(*mutex);
}

/* ---------------------------------------------------------
 * Thread (FreeRTOS task)
 *
 * The PAL thread function has signature void (*)(void *), which already
 * matches FreeRTOS TaskFunction_t. But the embedmq consumer function
 * returns when the queue is being torn down, whereas a FreeRTOS task
 * must never return. The trampoline therefore:
 *   1. runs the user function,
 *   2. gives the "done" semaphore so join() can unblock,
 *   3. calls vTaskDelete(NULL) so the task exits cleanly.
 * --------------------------------------------------------- */

static void task_trampoline(void *param)
{
    embedmq_pal_thread_t *t = (embedmq_pal_thread_t *)param;

    t->fn(t->arg);

    xSemaphoreGive(t->done);  /* signal join() — last use of *t */
    vTaskDelete(NULL);        /* never returns */
}

int embedmq_pal_thread_create(embedmq_pal_thread_t *thread,
                               void (*fn)(void *), void *arg,
                               int priority)
{
    thread->fn   = fn;
    thread->arg  = arg;
    thread->done = xSemaphoreCreateBinary();
    if (!thread->done)
        return -1;

    /* cfg priority 0 means "default"; idle priority would starve the
     * consumer, so bump it one above idle. */
    UBaseType_t prio = (priority > 0)
                     ? (UBaseType_t)priority
                     : (tskIDLE_PRIORITY + 1);

    BaseType_t ok = xTaskCreate(task_trampoline, "embedmq",
                                EMBEDMQ_FREERTOS_TASK_STACK,
                                thread, prio, &thread->handle);
    if (ok != pdPASS) {
        vSemaphoreDelete(thread->done);
        thread->done = NULL;
        return -1;
    }
    return 0;
}

void embedmq_pal_thread_join(embedmq_pal_thread_t *thread)
{
    if (!thread->done)
        return;

    /* Block until the task has run to completion and signalled done.
     * The task self-deletes; the idle task reclaims its TCB and stack. */
    xSemaphoreTake(thread->done, portMAX_DELAY);
    vSemaphoreDelete(thread->done);
    thread->done = NULL;
}

#endif /* EMBEDMQ_PAL_FREERTOS */
