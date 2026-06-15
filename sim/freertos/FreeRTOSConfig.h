/*
 * Minimal FreeRTOSConfig.h for the embedmq POSIX (GCC_POSIX) simulator test.
 *
 * Trimmed from the official FreeRTOS Posix_GCC demo: all application hooks
 * (idle / tick / daemon / malloc-failed / stack-overflow) and static
 * allocation are disabled, so the test harness needs no callback functions.
 * Dynamic allocation (heap_4) is used throughout.
 *
 * This config exists only to run embedmq's FreeRTOS PAL on a host machine
 * without hardware. It is NOT a recommended production config.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <pthread.h>   /* PTHREAD_STACK_MIN for the POSIX port */
#include <assert.h>

#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0
#define configCHECK_FOR_STACK_OVERFLOW          0
#define configGENERATE_RUN_TIME_STATS           0

#define configTICK_RATE_HZ                      ( 1000 )

/*
 * In the GCC_POSIX port the FreeRTOS stack buffer (depth * sizeof(StackType_t),
 * 8 bytes here) doubles as the pthread stack, clamped up to PTHREAD_STACK_MIN.
 * So express the minimal depth as PTHREAD_STACK_MIN / sizeof(StackType_t):
 * the heap cost stays ~PTHREAD_STACK_MIN bytes instead of 8x that.
 */
#define configMINIMAL_STACK_SIZE                ( ( unsigned short ) ( PTHREAD_STACK_MIN / sizeof( unsigned long ) ) )
#define configTOTAL_HEAP_SIZE                   ( ( size_t ) ( 2 * 1024 * 1024 ) )
#define configMAX_TASK_NAME_LEN                 ( 16 )
#define configMAX_PRIORITIES                    ( 7 )
#define configSTACK_DEPTH_TYPE                  uint32_t

#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_TASK_NOTIFICATIONS            1
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_QUEUE_SETS                    0
#define configUSE_TRACE_FACILITY                0

/* Memory allocation: dynamic only (heap_4 selected via CMake FREERTOS_HEAP) */
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1

/* Software timers off — embedmq does not use them */
#define configUSE_TIMERS                        0
#define configUSE_CO_ROUTINES                   0

/* Optional API the harness / PAL relies on */
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_eTaskGetState                   1

#define configASSERT( x )    assert( x )

#endif /* FREERTOS_CONFIG_H */
