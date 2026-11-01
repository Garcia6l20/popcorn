#include "stm32l1xx_hal.h"

#include <stdint.h>

#define BLOCK_WORDS 8u

void SysTick_Handler(void);

CRC_HandleTypeDef hcrc;

static uint32_t first_word[]       = {0x12345678u};
static uint32_t second_word[]      = {0x9ABCDEF0u};
static uint32_t block[BLOCK_WORDS] = {
    0x00000000u, 0xFFFFFFFFu, 0x01234567u, 0x89ABCDEFu,
    0xDEADBEEFu, 0x0BADF00Du, 0x80000001u, 0x7FFFFFFEu,
};

volatile uint32_t crc_first;
volatile uint32_t crc_accumulated;
volatile uint32_t crc_block;
volatile uint8_t  tag;

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void HAL_CRC_MspInit(CRC_HandleTypeDef* handle)
{
  (void)handle;
  __HAL_RCC_CRC_CLK_ENABLE();
}

__attribute__((noipa)) void computed(void)
{}

static void halt(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

int main(void)
{
  HAL_Init();
  hcrc.Instance = CRC;
  if(HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    halt();
  }
  crc_first       = HAL_CRC_Calculate(&hcrc, first_word, 1);
  crc_accumulated = HAL_CRC_Accumulate(&hcrc, second_word, 1);
  crc_block       = HAL_CRC_Calculate(&hcrc, block, BLOCK_WORDS);
  CRC->IDR        = 0xA5u;
  tag             = (uint8_t)CRC->IDR;
  computed();
  for(;;)
  {
    __WFI();
  }
}
