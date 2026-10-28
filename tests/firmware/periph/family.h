#pragma once

#if defined(STM32U575xx)
#include "family_u5.h"
#elif defined(STM32H573xx)
#include "family_h5.h"
#else
#error "no family header for this chipset define"
#endif
