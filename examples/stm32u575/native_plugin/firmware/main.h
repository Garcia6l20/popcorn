#ifndef MAIN_H
#define MAIN_H

#include "stm32u5xx_hal.h"

#define RANDOM_WORDS    8u
#define ADC_CHANNELS    2u
#define ADC_CONVERSIONS 16u
#define ADC_START       0x8000u
#define ADC_CHANNEL_BIT 12u
#define ADC_CS_PORT     GPIOB
#define ADC_CS_PIN      GPIO_PIN_0

/** @brief Random words the RNG gave, one per interrupt. */
struct rng_result
{
  uint32_t words[RANDOM_WORDS];
  uint32_t count;
};

/** @brief Conversions of both channels, one per millisecond of SysTick time. */
struct adc_result
{
  uint16_t samples[ADC_CHANNELS][ADC_CONVERSIONS];
  uint32_t conversions;
};

extern RNG_HandleTypeDef hrng;
extern SPI_HandleTypeDef hspi1;
extern struct rng_result rng_result;
extern struct adc_result adc_result;

/** @brief Stops the firmware on an unrecoverable HAL error. */
void Error_Handler(void);

/** @brief Called once rng_result is filled. */
void rng_done(void);

/** @brief Called once adc_result is filled. */
void adc_done(void);

#endif
