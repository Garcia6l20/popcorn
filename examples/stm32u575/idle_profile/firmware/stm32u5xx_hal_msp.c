#include "main.h"

void HAL_MspInit(void)
{
  __HAL_RCC_PWR_CLK_ENABLE();
}
