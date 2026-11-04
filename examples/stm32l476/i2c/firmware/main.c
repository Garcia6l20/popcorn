#include "main.h"

I2C_HandleTypeDef    hi2c1;
struct sensor_result sensor_result;

void SysTick_Handler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void sample_taken(void)
{}

static void MX_GPIO_Init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
  GPIO_InitTypeDef led = {
      .Pin   = GPIO_PIN_5,
      .Mode  = GPIO_MODE_OUTPUT_PP,
      .Pull  = GPIO_NOPULL,
      .Speed = GPIO_SPEED_FREQ_LOW,
  };
  HAL_GPIO_Init(GPIOA, &led);
}

static void MX_I2C1_Init(void)
{
  hi2c1.Instance              = I2C1;
  hi2c1.Init.Timing           = I2C_TIMING_SM_AT_4MHZ;
  hi2c1.Init.OwnAddress1      = 0;
  hi2c1.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2      = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
  if(HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
}

static void write_config(void)
{
  uint8_t const value[2] = {SENSOR_CONFIG_VALUE >> 8, SENSOR_CONFIG_VALUE & 0xFFu};
  if(HAL_I2C_Mem_Write(
         &hi2c1, SENSOR_ADDRESS, SENSOR_CONFIG, I2C_MEMADD_SIZE_8BIT, (uint8_t*)value, sizeof value,
         SENSOR_TIMEOUT_MS)
     != HAL_OK)
  {
    ++sensor_result.errors;
  }
}

static void sample(void)
{
  uint8_t raw[2] = {0};
  if(HAL_I2C_Mem_Read(
         &hi2c1, SENSOR_ADDRESS, SENSOR_TEMPERATURE, I2C_MEMADD_SIZE_8BIT, raw, sizeof raw,
         SENSOR_TIMEOUT_MS)
     != HAL_OK)
  {
    ++sensor_result.errors;
    sensor_result.last_error = HAL_I2C_GetError(&hi2c1);
    return;
  }
  int32_t const counts = (int16_t)(raw[0] << 8 | raw[1]) >> 4;
  sensor_result.sum += counts;
  ++sensor_result.samples;
  sensor_result.mean = sensor_result.sum / (int32_t)sensor_result.samples;
  HAL_GPIO_WritePin(
      GPIOA, GPIO_PIN_5,
      sensor_result.mean > OVER_TEMPERATURE_COUNTS ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

int main(void)
{
  HAL_Init();
  MX_GPIO_Init();
  MX_I2C1_Init();
  write_config();
  uint32_t next = HAL_GetTick();
  for(;;)
  {
    next += SAMPLE_MS;
    while((int32_t)(HAL_GetTick() - next) < 0)
    {
      __WFI();
    }
    sample();
    sample_taken();
  }
}
