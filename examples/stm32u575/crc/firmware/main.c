#include "stm32u5xx_hal.h"

#include <stdint.h>

/** @brief CRCs the HAL computed, in the order main computes them. */
struct results
{
  uint32_t one_word;
  uint32_t two_words;
  uint32_t iso_hdlc;
  uint32_t ibm_3740;
  uint32_t errors;
};

volatile struct results results;

static uint32_t words[] = {0x12345678u, 0x9ABCDEF0u};
static uint8_t  check_input[]
    __attribute__((aligned(4))) = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};

void SysTick_Handler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void HAL_CRC_MspInit(CRC_HandleTypeDef* hcrc)
{
  (void)hcrc;
  __HAL_RCC_CRC_CLK_ENABLE();
}

__attribute__((noipa)) void computed(void)
{}

static void init(CRC_HandleTypeDef* hcrc)
{
  hcrc->Instance = CRC;
  if(HAL_CRC_Init(hcrc) != HAL_OK)
  {
    ++results.errors;
  }
}

static void reset_config_words(void)
{
  CRC_HandleTypeDef hcrc            = {0};
  hcrc.Init.DefaultPolynomialUse    = DEFAULT_POLYNOMIAL_ENABLE;
  hcrc.Init.DefaultInitValueUse     = DEFAULT_INIT_VALUE_ENABLE;
  hcrc.Init.InputDataInversionMode  = CRC_INPUTDATA_INVERSION_NONE;
  hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
  hcrc.InputDataFormat              = CRC_INPUTDATA_FORMAT_WORDS;
  init(&hcrc);
  results.one_word  = HAL_CRC_Calculate(&hcrc, &words[0], 1);
  results.two_words = HAL_CRC_Accumulate(&hcrc, &words[1], 1);
}

static void iso_hdlc_bytes(void)
{
  CRC_HandleTypeDef hcrc            = {0};
  hcrc.Init.DefaultPolynomialUse    = DEFAULT_POLYNOMIAL_ENABLE;
  hcrc.Init.DefaultInitValueUse     = DEFAULT_INIT_VALUE_ENABLE;
  hcrc.Init.InputDataInversionMode  = CRC_INPUTDATA_INVERSION_BYTE;
  hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_ENABLE;
  hcrc.InputDataFormat              = CRC_INPUTDATA_FORMAT_BYTES;
  init(&hcrc);
  results.iso_hdlc = ~HAL_CRC_Calculate(&hcrc, (uint32_t*)(void*)check_input, sizeof check_input);
}

static void ibm_3740_bytes(void)
{
  CRC_HandleTypeDef hcrc            = {0};
  hcrc.Init.DefaultPolynomialUse    = DEFAULT_POLYNOMIAL_DISABLE;
  hcrc.Init.GeneratingPolynomial    = 0x1021u;
  hcrc.Init.CRCLength               = CRC_POLYLENGTH_16B;
  hcrc.Init.DefaultInitValueUse     = DEFAULT_INIT_VALUE_DISABLE;
  hcrc.Init.InitValue               = 0xFFFFu;
  hcrc.Init.InputDataInversionMode  = CRC_INPUTDATA_INVERSION_NONE;
  hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
  hcrc.InputDataFormat              = CRC_INPUTDATA_FORMAT_BYTES;
  init(&hcrc);
  results.ibm_3740 = HAL_CRC_Calculate(&hcrc, (uint32_t*)(void*)check_input, sizeof check_input);
}

int main(void)
{
  HAL_Init();
  reset_config_words();
  iso_hdlc_bytes();
  ibm_3740_bytes();
  computed();
  for(;;)
  {
  }
}
