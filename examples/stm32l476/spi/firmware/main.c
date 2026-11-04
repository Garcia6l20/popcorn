#include "main.h"

SPI_HandleTypeDef   hspi1;
struct accel_result accel_result;

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

__attribute__((noipa)) void accel_done(void)
{}

static void MX_GPIO_Init(void)
{
  __HAL_RCC_GPIOB_CLK_ENABLE();
  HAL_GPIO_WritePin(ACCEL_CS_PORT, ACCEL_CS_PIN, GPIO_PIN_SET);
  GPIO_InitTypeDef cs = {
      .Pin   = ACCEL_CS_PIN,
      .Mode  = GPIO_MODE_OUTPUT_PP,
      .Pull  = GPIO_NOPULL,
      .Speed = GPIO_SPEED_FREQ_LOW,
  };
  HAL_GPIO_Init(ACCEL_CS_PORT, &cs);
}

static void MX_SPI1_Init(void)
{
  hspi1.Instance               = SPI1;
  hspi1.Init.Mode              = SPI_MODE_MASTER;
  hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity       = SPI_POLARITY_HIGH;
  hspi1.Init.CLKPhase          = SPI_PHASE_2EDGE;
  hspi1.Init.NSS               = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial     = 7;
  hspi1.Init.CRCLength         = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode          = SPI_NSS_PULSE_DISABLE;
  if(HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
}

static void transfer(uint8_t const* out, uint8_t* in, uint16_t size)
{
  HAL_GPIO_WritePin(ACCEL_CS_PORT, ACCEL_CS_PIN, GPIO_PIN_RESET);
  if(HAL_SPI_TransmitReceive(&hspi1, (uint8_t*)out, in, size, HAL_MAX_DELAY) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_GPIO_WritePin(ACCEL_CS_PORT, ACCEL_CS_PIN, GPIO_PIN_SET);
}

static uint8_t read_register(uint8_t address)
{
  uint8_t const out[2] = {ACCEL_READ | address, 0};
  uint8_t       in[2]  = {0};
  transfer(out, in, sizeof out);
  return in[1];
}

static void write_register(uint8_t address, uint8_t value)
{
  uint8_t const out[2] = {address, value};
  uint8_t       in[2]  = {0};
  transfer(out, in, sizeof out);
}

static void read_axes(int16_t* axes)
{
  uint8_t out[1 + 2 * ACCEL_AXES] = {ACCEL_READ | ACCEL_INCREMENT | ACCEL_OUT_X_L};
  uint8_t in[1 + 2 * ACCEL_AXES]  = {0};
  transfer(out, in, sizeof out);
  for(uint32_t axis = 0; axis < ACCEL_AXES; ++axis)
  {
    axes[axis] = (int16_t)(in[1 + 2 * axis] | in[2 + 2 * axis] << 8);
  }
}

int main(void)
{
  HAL_Init();
  MX_GPIO_Init();
  MX_SPI1_Init();
  accel_result.who_am_i = read_register(ACCEL_WHO_AM_I);
  write_register(ACCEL_CTRL_REG1, ACCEL_CTRL_VALUE);
  for(uint32_t index = 0; index < ACCEL_SAMPLES; ++index)
  {
    read_axes(accel_result.samples[index]);
    ++accel_result.count;
    HAL_Delay(1);
  }
  accel_done();
  for(;;)
  {
    __WFI();
  }
}
