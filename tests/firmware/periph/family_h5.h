#pragma once

#include "stm32h5xx_ll_rcc.h"

#define LOG_RESET_CLOCK_SOURCE() LOG(LL_RCC_HSI_GetDivider())
