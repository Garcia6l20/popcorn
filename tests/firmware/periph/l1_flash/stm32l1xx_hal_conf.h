#pragma once

#define HAL_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED

#define HSE_VALUE           8000000U
#define HSE_STARTUP_TIMEOUT 100U
#define MSI_VALUE           2097000U
#define HSI_VALUE           16000000U
#define LSI_VALUE           37000U
#define LSE_VALUE           32768U
#define LSE_STARTUP_TIMEOUT 5000U
#define VDD_VALUE           3300U
#define TICK_INT_PRIORITY   0x0FU
#define USE_RTOS            0U
#define PREFETCH_ENABLE     0U

#define assert_param(expr) ((void)0U)

#include "stm32l1xx_hal_flash.h"
