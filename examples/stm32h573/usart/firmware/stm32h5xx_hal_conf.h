#ifndef STM32H5XX_HAL_CONF_H
#define STM32H5XX_HAL_CONF_H

#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

#define HSE_VALUE           25000000UL
#define HSE_STARTUP_TIMEOUT 100UL
#define CSI_VALUE           4000000UL
#define HSI_VALUE           64000000UL
#define HSI48_VALUE         48000000UL
#define LSI_VALUE           32000UL
#define LSI_STARTUP_TIME    130UL
#define LSE_VALUE           32768UL
#define LSE_STARTUP_TIMEOUT 5000UL

#define EXTERNAL_CLOCK_VALUE 12288000UL

#define VDD_VALUE         3300UL
#define TICK_INT_PRIORITY ((1UL << __NVIC_PRIO_BITS) - 1UL)
#define USE_RTOS          0U
#define PREFETCH_ENABLE   0U

#define USE_HAL_UART_REGISTER_CALLBACKS 0U

#include "stm32h5xx_hal_cortex.h"
#include "stm32h5xx_hal_dma.h"
#include "stm32h5xx_hal_flash.h"
#include "stm32h5xx_hal_gpio.h"
#include "stm32h5xx_hal_pwr.h"
#include "stm32h5xx_hal_rcc.h"
#include "stm32h5xx_hal_uart.h"

#define assert_param(expr) ((void)(expr))

#endif
