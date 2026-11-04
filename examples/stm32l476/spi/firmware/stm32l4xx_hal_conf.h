#ifndef STM32L4XX_HAL_CONF_H
#define STM32L4XX_HAL_CONF_H

#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_PWR_EX_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_SPI_MODULE_ENABLED

#define HSE_VALUE                 8000000UL
#define HSE_STARTUP_TIMEOUT       100UL
#define MSI_VALUE                 4000000UL
#define HSI_VALUE                 16000000UL
#define LSI_VALUE                 32000UL
#define LSE_VALUE                 32768UL
#define LSE_STARTUP_TIMEOUT       5000UL
#define EXTERNAL_SAI1_CLOCK_VALUE 48000UL
#define EXTERNAL_SAI2_CLOCK_VALUE 48000UL

#define VDD_VALUE                3300UL
#define TICK_INT_PRIORITY        ((1UL << __NVIC_PRIO_BITS) - 1UL)
#define USE_RTOS                 0U
#define PREFETCH_ENABLE          0U
#define INSTRUCTION_CACHE_ENABLE 1U
#define DATA_CACHE_ENABLE        1U

#define USE_HAL_SPI_REGISTER_CALLBACKS 0U
#define USE_SPI_CRC                    0U

#include "stm32l4xx_hal_cortex.h"
#include "stm32l4xx_hal_dma.h"
#include "stm32l4xx_hal_flash.h"
#include "stm32l4xx_hal_gpio.h"
#include "stm32l4xx_hal_pwr.h"
#include "stm32l4xx_hal_rcc.h"
#include "stm32l4xx_hal_spi.h"

#define assert_param(expr) ((void)sizeof(expr))

#endif
