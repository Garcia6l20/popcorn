#pragma once

#if defined(STM32U575xx)
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_i2c.h"
#elif defined(STM32H573xx)
#include "stm32h5xx_ll_bus.h"
#include "stm32h5xx_ll_i2c.h"
#elif defined(STM32L476xx)
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_i2c.h"
#else
#error "no I2C family header for this chipset define"
#endif
