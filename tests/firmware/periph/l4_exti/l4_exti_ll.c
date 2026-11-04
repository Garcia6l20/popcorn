#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_exti.h"
#include "stm32l4xx_ll_gpio.h"
#include "stm32l4xx_ll_system.h"

void idle(void);
void EXTI0_IRQHandler(void);
void PVD_PVM_IRQHandler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void EXTI0_IRQHandler(void)
{
  LOG(0xe000u | LL_GPIO_IsInputPinSet(GPIOA, LL_GPIO_PIN_0));
  LL_EXTI_ClearFlag_0_31(LL_EXTI_LINE_0);
}

void PVD_PVM_IRQHandler(void)
{
  LOG(0x3500u | EXTI->PR2);
  LL_EXTI_ClearFlag_32_63(LL_EXTI_LINE_35);
}

int main(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SYSCFG);

  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_0, LL_GPIO_MODE_INPUT);
  LL_GPIO_SetPinPull(GPIOA, LL_GPIO_PIN_0, LL_GPIO_PULL_DOWN);

  LL_SYSCFG_SetEXTISource(LL_SYSCFG_EXTI_PORTA, LL_SYSCFG_EXTI_LINE0);
  LL_EXTI_EnableRisingTrig_0_31(LL_EXTI_LINE_0);
  LL_EXTI_EnableIT_0_31(LL_EXTI_LINE_0);
  NVIC_EnableIRQ(EXTI0_IRQn);

  LL_EXTI_EnableIT_32_63(LL_EXTI_LINE_35);
  NVIC_EnableIRQ(PVD_PVM_IRQn);
  LOG(SYSCFG->EXTICR[0]);
  LOG(EXTI->IMR1);
  LOG(EXTI->IMR2);
  __asm__ volatile("bkpt #3" ::: "memory");
  LOG(0xb3u);

  LL_EXTI_GenerateSWI_32_63(LL_EXTI_LINE_35);
  __DSB();
  __ISB();
  LOG(EXTI->SWIER2);
  LOG(EXTI->PR2);
  DONE();
}
