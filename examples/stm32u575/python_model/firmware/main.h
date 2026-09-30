#ifndef MAIN_H
#define MAIN_H

#include "stm32u5xx_hal.h"

#define RANDOM_WORDS 8u

/** @brief Random words the RNG gave, one per interrupt. */
struct rng_result
{
  uint32_t words[RANDOM_WORDS];
  uint32_t count;
};

extern RNG_HandleTypeDef hrng;
extern struct rng_result rng_result;

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

/** @brief Called once rng_result is filled. */
void rng_done(void);

#endif
