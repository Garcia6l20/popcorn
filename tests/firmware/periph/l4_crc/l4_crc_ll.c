#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_crc.h"

void idle(void);

void poly7(void);
void reversed(void);

static char const check[] = "123456789";

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void feed_check(void)
{
  for(char const* next = check; *next != 0; ++next)
  {
    LL_CRC_FeedData8(CRC, (uint8_t)*next);
  }
}

int main(void)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_CRC);
  LL_CRC_FeedData32(CRC, 0x12345678u);
  LL_CRC_FeedData32(CRC, 0x9ABCDEF0u);
  LOG(LL_CRC_ReadData32(CRC));
  LL_CRC_Write_IDR(CRC, 0xA5u);
  LL_CRC_ResetCRCCalculationUnit(CRC);
  LOG(LL_CRC_ReadData32(CRC));
  LOG(LL_CRC_Read_IDR(CRC));
  DONE();
}

void poly7(void)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_CRC);
  LL_CRC_SetPolynomialSize(CRC, LL_CRC_POLYLENGTH_7B);
  LL_CRC_SetPolynomialCoef(CRC, 0x09u);
  LL_CRC_SetInitialData(CRC, 0x00u);
  feed_check();
  LOG(LL_CRC_ReadData7(CRC));
  DONE();
}

void reversed(void)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_CRC);
  LL_CRC_SetInputDataReverseMode(CRC, LL_CRC_INDATA_REVERSE_BYTE);
  LL_CRC_SetOutputDataReverseMode(CRC, LL_CRC_OUTDATA_REVERSE_BIT);
  LL_CRC_ResetCRCCalculationUnit(CRC);
  feed_check();
  LOG(LL_CRC_ReadData32(CRC));
  DONE();
}
