#pragma once

#include "stm32u5xx_ll_rcc.h"

#define LOG_RESET_CLOCK_SOURCE() LOG(LL_RCC_MSIS_GetRange())
