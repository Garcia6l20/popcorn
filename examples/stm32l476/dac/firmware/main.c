#include "stm32l4xx_hal.h"

#include <stdint.h>

#define SAMPLES   16u
#define BASE_CODE 0x400u
#define TIMER_HZ  1000000u
#define UPDATE_HZ 1000u
#define LOAD_SPIN 4u

ADC_HandleTypeDef hadc;
DAC_HandleTypeDef hdac;
TIM_HandleTypeDef htim6;

volatile uint32_t samples[SAMPLES];
volatile uint32_t sampled_count;

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

__attribute__((noipa)) void wave_started(void)
{}

__attribute__((noipa)) void sampled(void)
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
}

void HAL_DAC_MspInit(DAC_HandleTypeDef* handle)
{
  (void)handle;
  __HAL_RCC_DAC1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef pin = {
      .Pin  = GPIO_PIN_4,
      .Mode = GPIO_MODE_ANALOG,
      .Pull = GPIO_NOPULL,
  };
  HAL_GPIO_Init(GPIOA, &pin);
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef* handle)
{
  (void)handle;
  __HAL_RCC_TIM6_CLK_ENABLE();
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

static void MX_DAC_Init(void)
{
  hdac.Instance = DAC1;
  if(HAL_DAC_Init(&hdac) != HAL_OK)
  {
    Error_Handler();
  }
  DAC_ChannelConfTypeDef config = {
      .DAC_SampleAndHold           = DAC_SAMPLEANDHOLD_DISABLE,
      .DAC_Trigger                 = DAC_TRIGGER_T6_TRGO,
      .DAC_OutputBuffer            = DAC_OUTPUTBUFFER_ENABLE,
      .DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_ENABLE,
      .DAC_UserTrimming            = DAC_TRIMMING_FACTORY,
  };
  if(HAL_DAC_ConfigChannel(&hdac, &config, DAC_CHANNEL_1) != HAL_OK
     || HAL_DACEx_TriangleWaveGenerate(&hdac, DAC_CHANNEL_1, DAC_TRIANGLEAMPLITUDE_7) != HAL_OK
     || HAL_DAC_SetValue(&hdac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, BASE_CODE) != HAL_OK
     || HAL_DAC_Start(&hdac, DAC_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_ADC_Init(void)
{
  hadc.Instance                   = ADC2;
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
  ADC_ChannelConfTypeDef channel  = {
      .Channel      = ADC_CHANNEL_DAC1CH1_ADC2,
      .Rank         = ADC_REGULAR_RANK_1,
      .SamplingTime = ADC_SAMPLETIME_47CYCLES_5,
      .SingleDiff   = ADC_SINGLE_ENDED,
      .OffsetNumber = ADC_OFFSET_NONE,
      .Offset       = 0,
  };
  if(HAL_ADC_Init(&hadc) != HAL_OK
     || HAL_ADCEx_Calibration_Start(&hadc, ADC_SINGLE_ENDED) != HAL_OK
     || HAL_ADC_ConfigChannel(&hadc, &channel) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_TIM6_Init(void)
{
  htim6.Instance               = TIM6;
  htim6.Init.Prescaler         = SystemCoreClock / TIMER_HZ - 1u;
  htim6.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim6.Init.Period            = TIMER_HZ / UPDATE_HZ - 1u;
  htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if(HAL_TIM_Base_Init(&htim6) != HAL_OK)
  {
    Error_Handler();
  }
  MODIFY_REG(TIM6->CR2, TIM_CR2_MMS, TIM_TRGO_UPDATE);
}

static uint32_t convert(void)
{
  if(HAL_ADC_Start(&hadc) != HAL_OK || HAL_ADC_PollForConversion(&hadc, 10) != HAL_OK)
  {
    Error_Handler();
  }
  return HAL_ADC_GetValue(&hadc);
}

static void spin(uint32_t count)
{
  for(volatile uint32_t i = 0; i < count; ++i)
  {
  }
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_DAC_Init();
  MX_ADC_Init();
  MX_TIM6_Init();
  __HAL_TIM_CLEAR_FLAG(&htim6, TIM_FLAG_UPDATE);
  if(HAL_TIM_Base_Start(&htim6) != HAL_OK)
  {
    Error_Handler();
  }
  wave_started();
  for(;;)
  {
    while(__HAL_TIM_GET_FLAG(&htim6, TIM_FLAG_UPDATE) == RESET)
    {
    }
    __HAL_TIM_CLEAR_FLAG(&htim6, TIM_FLAG_UPDATE);
    spin(LOAD_SPIN);
    samples[sampled_count % SAMPLES] = convert();
    ++sampled_count;
    if(sampled_count == SAMPLES)
    {
      sampled();
    }
  }
}
