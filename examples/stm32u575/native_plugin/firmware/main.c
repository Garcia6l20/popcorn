#include "main.h"

RNG_HandleTypeDef hrng;
SPI_HandleTypeDef hspi1;
struct rng_result rng_result;
struct adc_result adc_result;

static volatile bool ready;

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void rng_done(void)
{}

__attribute__((noipa)) void adc_done(void)
{}

void HAL_RNG_ReadyDataCallback(RNG_HandleTypeDef* handle, uint32_t random32bit)
{
  (void)handle;
  rng_result.words[rng_result.count++] = random32bit;
  ready                                = true;
}

static void MX_HSI48_Init(void)
{
  RCC_OscInitTypeDef oscillator = {
      .OscillatorType = RCC_OSCILLATORTYPE_HSI48,
      .HSI48State     = RCC_HSI48_ON,
      .PLL            = {.PLLState = RCC_PLL_NONE},
  };
  if(HAL_RCC_OscConfig(&oscillator) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_RNG_Init(void)
{
  hrng.Instance                 = RNG;
  hrng.Init.ClockErrorDetection = RNG_CED_ENABLE;
  if(HAL_RNG_Init(&hrng) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_GPIO_Init(void)
{
  __HAL_RCC_GPIOB_CLK_ENABLE();
  HAL_GPIO_WritePin(ADC_CS_PORT, ADC_CS_PIN, GPIO_PIN_SET);
  GPIO_InitTypeDef cs = {
      .Pin   = ADC_CS_PIN,
      .Mode  = GPIO_MODE_OUTPUT_PP,
      .Pull  = GPIO_NOPULL,
      .Speed = GPIO_SPEED_FREQ_LOW,
  };
  HAL_GPIO_Init(ADC_CS_PORT, &cs);
}

static void MX_SPI1_Init(void)
{
  hspi1.Instance                     = SPI1;
  hspi1.Init.Mode                    = SPI_MODE_MASTER;
  hspi1.Init.Direction               = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize                = SPI_DATASIZE_16BIT;
  hspi1.Init.CLKPolarity             = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase                = SPI_PHASE_1EDGE;
  hspi1.Init.NSS                     = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler       = SPI_BAUDRATEPRESCALER_4;
  hspi1.Init.FirstBit                = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode                  = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation          = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial           = 0x7;
  hspi1.Init.NSSPMode                = SPI_NSS_PULSE_DISABLE;
  hspi1.Init.NSSPolarity             = SPI_NSS_POLARITY_LOW;
  hspi1.Init.FifoThreshold           = SPI_FIFO_THRESHOLD_01DATA;
  hspi1.Init.MasterSSIdleness        = SPI_MASTER_SS_IDLENESS_00CYCLE;
  hspi1.Init.MasterInterDataIdleness = SPI_MASTER_INTERDATA_IDLENESS_00CYCLE;
  hspi1.Init.MasterReceiverAutoSusp  = SPI_MASTER_RX_AUTOSUSP_DISABLE;
  hspi1.Init.MasterKeepIOState       = SPI_MASTER_KEEP_IO_STATE_DISABLE;
  hspi1.Init.IOSwap                  = SPI_IO_SWAP_DISABLE;
  hspi1.Init.ReadyMasterManagement   = SPI_RDY_MASTER_MANAGEMENT_INTERNALLY;
  hspi1.Init.ReadyPolarity           = SPI_RDY_POLARITY_HIGH;
  if(HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
}

static uint16_t convert(uint32_t channel)
{
  uint16_t const command[2] = {(uint16_t)(ADC_START | channel << ADC_CHANNEL_BIT), 0};
  uint16_t       reply[2]   = {0};
  HAL_GPIO_WritePin(ADC_CS_PORT, ADC_CS_PIN, GPIO_PIN_RESET);
  if(HAL_SPI_TransmitReceive(&hspi1, (uint8_t const*)command, (uint8_t*)reply, 2, HAL_MAX_DELAY)
     != HAL_OK)
  {
    Error_Handler();
  }
  HAL_GPIO_WritePin(ADC_CS_PORT, ADC_CS_PIN, GPIO_PIN_SET);
  return reply[1] & 0x0FFFu;
}

static void read_random_words(void)
{
  while(rng_result.count < RANDOM_WORDS)
  {
    ready = false;
    if(HAL_RNG_GenerateRandomNumber_IT(&hrng) != HAL_OK)
    {
      Error_Handler();
    }
    __disable_irq();
    while(!ready)
    {
      __WFI();
      __enable_irq();
      __disable_irq();
    }
    __enable_irq();
  }
  rng_done();
}

static void convert_both_channels(void)
{
  for(uint32_t index = 0; index < ADC_CONVERSIONS; ++index)
  {
    for(uint32_t channel = 0; channel < ADC_CHANNELS; ++channel)
    {
      adc_result.samples[channel][index] = convert(channel);
    }
    ++adc_result.conversions;
    HAL_Delay(1);
  }
  adc_done();
}

int main(void)
{
  HAL_Init();
  MX_HSI48_Init();
  MX_RNG_Init();
  MX_GPIO_Init();
  MX_SPI1_Init();
  read_random_words();
  convert_both_channels();
  for(;;)
  {
    __WFI();
  }
}
