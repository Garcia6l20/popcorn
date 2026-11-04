#pragma once

#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

#define HSE_VALUE                       8000000U
#define HSE_STARTUP_TIMEOUT             100U
#define MSI_VALUE                       4000000U
#define HSI_VALUE                       16000000U
#define LSI_VALUE                       32000U
#define LSE_VALUE                       32768U
#define LSE_STARTUP_TIMEOUT             5000U
#define HSI48_VALUE                     48000000U
#define EXTERNAL_SAI1_CLOCK_VALUE       2097000U
#define EXTERNAL_SAI2_CLOCK_VALUE       2097000U
#define VDD_VALUE                       3300U
#define TICK_INT_PRIORITY               0x0FU
#define USE_RTOS                        0U
#define PREFETCH_ENABLE                 0U
#define INSTRUCTION_CACHE_ENABLE        1U
#define DATA_CACHE_ENABLE               1U
#define USE_HAL_UART_REGISTER_CALLBACKS 0U

#define assert_param(expr) ((void)0U)

#include "stm32l4xx_hal_cortex.h"
#include "stm32l4xx_hal_dma.h"
#include "stm32l4xx_hal_flash.h"
#include "stm32l4xx_hal_gpio.h"
#include "stm32l4xx_hal_pwr.h"
#include "stm32l4xx_hal_rcc.h"
#include "stm32l4xx_hal_uart.h"
