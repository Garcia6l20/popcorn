#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_exti.h"
#include "stm32l1xx_ll_gpio.h"
#include "stm32l1xx_ll_system.h"

void idle(void);
void EXTI0_IRQHandler(void);
void EXTI15_10_IRQHandler(void);

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

void EXTI15_10_IRQHandler(void)
{
  LOG(0xe130u | LL_GPIO_IsInputPinSet(GPIOC, LL_GPIO_PIN_13));
  LL_EXTI_ClearFlag_0_31(LL_EXTI_LINE_13);
}

int main(void)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA | LL_AHB1_GRP1_PERIPH_GPIOC);
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SYSCFG);

  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_0, LL_GPIO_MODE_INPUT);
  LL_GPIO_SetPinPull(GPIOA, LL_GPIO_PIN_0, LL_GPIO_PULL_DOWN);
  LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_13, LL_GPIO_MODE_INPUT);
  LL_GPIO_SetPinPull(GPIOC, LL_GPIO_PIN_13, LL_GPIO_PULL_UP);

  LL_SYSCFG_SetEXTISource(LL_SYSCFG_EXTI_PORTA, LL_SYSCFG_EXTI_LINE0);
  LL_EXTI_EnableRisingTrig_0_31(LL_EXTI_LINE_0);
  LL_EXTI_EnableFallingTrig_0_31(LL_EXTI_LINE_0);
  LL_EXTI_EnableIT_0_31(LL_EXTI_LINE_0);
  NVIC_EnableIRQ(EXTI0_IRQn);

  LL_SYSCFG_SetEXTISource(LL_SYSCFG_EXTI_PORTC, LL_SYSCFG_EXTI_LINE13);
  LL_EXTI_EnableFallingTrig_0_31(LL_EXTI_LINE_13);
  LL_EXTI_EnableIT_0_31(LL_EXTI_LINE_13);
  NVIC_EnableIRQ(EXTI15_10_IRQn);
  LOG(SYSCFG->EXTICR[3]);
  __asm__ volatile("bkpt #3" ::: "memory");
  LOG(0xb3u);

  LL_EXTI_GenerateSWI_0_31(LL_EXTI_LINE_0);
  __DSB();
  __ISB();
  LOG(0x5e1u);
  LOG(EXTI->SWIER);
  LOG(EXTI->PR);
  DONE();
}
