#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern uint32_t SystemCoreClock;
void            vAssertCalled(char const* file, int line);

#define configENABLE_TRUSTZONE         0
#define configRUN_FREERTOS_SECURE_ONLY 0
#define configENABLE_FPU               1
#define configENABLE_MPU               0
#define configENABLE_MVE               0

#define configUSE_PREEMPTION             1
#define configCPU_CLOCK_HZ               SystemCoreClock
#define configTICK_RATE_HZ               1000
#define configMAX_PRIORITIES             5
#define configMINIMAL_STACK_SIZE         256
#define configTOTAL_HEAP_SIZE            (16 * 1024)
#define configMAX_TASK_NAME_LEN          16
#define configTICK_TYPE_WIDTH_IN_BITS    TICK_TYPE_WIDTH_32_BITS
#define configUSE_TASK_NOTIFICATIONS     1
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configSUPPORT_STATIC_ALLOCATION  0
#define configUSE_IDLE_HOOK              0
#define configUSE_TICK_HOOK              0
#define configUSE_MALLOC_FAILED_HOOK     0
#define configCHECK_FOR_STACK_OVERFLOW   0
#define configUSE_TIMERS                 0
#define configUSE_MUTEXES                0
#define configUSE_COUNTING_SEMAPHORES    0

#define configPRIO_BITS                              4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY \
  (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
  (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#define INCLUDE_vTaskDelay                1
#define INCLUDE_vTaskDelete               1
#define INCLUDE_vTaskSuspend              1
#define INCLUDE_xTaskGetCurrentTaskHandle 1

#define configASSERT(x)                  \
  do                                     \
  {                                      \
    if(!(x))                             \
    {                                    \
      vAssertCalled(__FILE__, __LINE__); \
    }                                    \
  } while(0)

#endif
