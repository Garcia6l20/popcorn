#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_crc.h"
#include "stm32l1xx_ll_dma.h"

void idle(void);
void words(void);
void dma_bytes(void);

static uint8_t const bytes[4] = {0x11, 0x22, 0x33, 0x44};

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void words(void)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_CRC);
  LOG(LL_CRC_ReadData32(CRC));
  LL_CRC_FeedData32(CRC, 0x12345678u);
  LOG(LL_CRC_ReadData32(CRC));
  LL_CRC_FeedData32(CRC, 0x9ABCDEF0u);
  LOG(LL_CRC_ReadData32(CRC));
  LL_CRC_Write_IDR(CRC, 0xA5u);
  LL_CRC_ResetCRCCalculationUnit(CRC);
  LOG(LL_CRC_ReadData32(CRC));
  LOG(LL_CRC_Read_IDR(CRC));
  LL_CRC_Write_IDR(CRC, 0x80u);
  LOG(LL_CRC_Read_IDR(CRC));
  DONE();
}

void dma_bytes(void)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_CRC | LL_AHB1_GRP1_PERIPH_DMA1);
  LL_DMA_ConfigTransfer(
      DMA1, LL_DMA_CHANNEL_1,
      LL_DMA_DIRECTION_MEMORY_TO_MEMORY
          | LL_DMA_MODE_NORMAL
          | LL_DMA_PERIPH_INCREMENT
          | LL_DMA_MEMORY_NOINCREMENT
          | LL_DMA_PDATAALIGN_BYTE
          | LL_DMA_MDATAALIGN_BYTE
          | LL_DMA_PRIORITY_LOW);
  LL_DMA_ConfigAddresses(
      DMA1, LL_DMA_CHANNEL_1, (uint32_t)bytes, (uint32_t)&CRC->DR,
      LL_DMA_DIRECTION_MEMORY_TO_MEMORY);
  LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_1, 4u);
  LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);
  while(LL_DMA_IsActiveFlag_TC1(DMA1) == 0u)
  {
  }
  LOG(LL_CRC_ReadData32(CRC));
  DONE();
}

int main(void)
{
  idle();
}
