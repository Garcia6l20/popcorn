#include "stm32l4xx_hal.h"

#include <stdint.h>

ADC_HandleTypeDef hadc;

volatile uint32_t vrefint_data;
volatile uint32_t vdda_mv;
volatile uint32_t sensor_data;
volatile int32_t  temperature_c;
volatile uint32_t pa0_data;
volatile uint32_t pa0_mv;
volatile uint32_t pa0_conversions;

#define FULL_SCALE 4095u

void SysTick_Handler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

__attribute__((noinline)) static void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void measured(void)
{}

__attribute__((noipa)) void pa0_converted(void)
{}

void HAL_ADC_MspInit(ADC_HandleTypeDef* handle)
{
  (void)handle;
  RCC_PeriphCLKInitTypeDef clock = {
      .PeriphClockSelection = RCC_PERIPHCLK_ADC,
      .AdcClockSelection    = RCC_ADCCLKSOURCE_SYSCLK,
  };
  if(HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_RCC_ADC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef pin = {
      .Pin  = GPIO_PIN_0,
      .Mode = GPIO_MODE_ANALOG_ADC_CONTROL,
      .Pull = GPIO_NOPULL,
  };
  HAL_GPIO_Init(GPIOA, &pin);
}

static void SystemClock_Config(void)
{
  __HAL_RCC_PWR_CLK_ENABLE();
  if(HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }
  RCC_OscInitTypeDef oscillators = {
      .OscillatorType      = RCC_OSCILLATORTYPE_HSI,
      .HSIState            = RCC_HSI_ON,
      .HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT,
      .PLL                 = {.PLLState = RCC_PLL_NONE},
  };
  if(HAL_RCC_OscConfig(&oscillators) != HAL_OK)
  {
    Error_Handler();
  }
  RCC_ClkInitTypeDef clocks = {
      .ClockType =
          RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2,
      .SYSCLKSource   = RCC_SYSCLKSOURCE_HSI,
      .AHBCLKDivider  = RCC_SYSCLK_DIV1,
      .APB1CLKDivider = RCC_HCLK_DIV1,
      .APB2CLKDivider = RCC_HCLK_DIV1,
  };
  if(HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_ADC_Init(void)
{
  hadc.Instance                   = ADC1;
  hadc.Init.ClockPrescaler        = ADC_CLOCK_ASYNC_DIV1;
  hadc.Init.Resolution            = ADC_RESOLUTION_12B;
  hadc.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
  hadc.Init.ScanConvMode          = ADC_SCAN_DISABLE;
  hadc.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
  hadc.Init.LowPowerAutoWait      = DISABLE;
  hadc.Init.ContinuousConvMode    = DISABLE;
  hadc.Init.NbrOfConversion       = 1;
  hadc.Init.DiscontinuousConvMode = DISABLE;
  hadc.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
  hadc.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc.Init.DMAContinuousRequests = DISABLE;
  hadc.Init.Overrun               = ADC_OVR_DATA_OVERWRITTEN;
  hadc.Init.OversamplingMode      = DISABLE;
  if(HAL_ADC_Init(&hadc) != HAL_OK
     || HAL_ADCEx_Calibration_Start(&hadc, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
}

static uint32_t convert(uint32_t channel)
{
  ADC_ChannelConfTypeDef config = {
      .Channel      = channel,
      .Rank         = ADC_REGULAR_RANK_1,
      .SamplingTime = ADC_SAMPLETIME_640CYCLES_5,
      .SingleDiff   = ADC_SINGLE_ENDED,
      .OffsetNumber = ADC_OFFSET_NONE,
      .Offset       = 0,
  };
  if(HAL_ADC_ConfigChannel(&hadc, &config) != HAL_OK
     || HAL_ADC_Start(&hadc) != HAL_OK
     || HAL_ADC_PollForConversion(&hadc, 10) != HAL_OK)
  {
    Error_Handler();
  }
  return HAL_ADC_GetValue(&hadc);
}

static uint32_t vrefint_mv(uint32_t data)
{
  return VREFINT_CAL_VREF * (uint32_t)*VREFINT_CAL_ADDR / data;
}

static int32_t celsius(uint32_t data, uint32_t vdda)
{
  int32_t const cal1   = *TEMPSENSOR_CAL1_ADDR;
  int32_t const cal2   = *TEMPSENSOR_CAL2_ADDR;
  int32_t const scaled = (int32_t)(data * vdda / TEMPSENSOR_CAL_VREFANALOG);
  return (scaled - cal1) * (TEMPSENSOR_CAL2_TEMP - TEMPSENSOR_CAL1_TEMP) / (cal2 - cal1)
       + TEMPSENSOR_CAL1_TEMP;
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_ADC_Init();
  vrefint_data  = convert(ADC_CHANNEL_VREFINT);
  vdda_mv       = vrefint_mv(vrefint_data);
  sensor_data   = convert(ADC_CHANNEL_TEMPSENSOR);
  temperature_c = celsius(sensor_data, vdda_mv);
  measured();
  for(;;)
  {
    pa0_data = convert(ADC_CHANNEL_5);
    pa0_mv   = pa0_data * vdda_mv / FULL_SCALE;
    ++pa0_conversions;
    pa0_converted();
    HAL_Delay(1);
  }
}
