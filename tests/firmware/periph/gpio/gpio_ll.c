#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_exti.h"
#include "stm32u5xx_ll_gpio.h"

#define PA5_LEVEL() ((LL_GPIO_ReadInputPort(GPIOA) >> 5) & 1u)

void EXTI0_IRQHandler(void);
void EXTI13_IRQHandler(void);

void EXTI0_IRQHandler(void)
{
  LOG(0xe000u);
  LL_EXTI_ClearRisingFlag_0_31(LL_EXTI_LINE_0);
}

void EXTI13_IRQHandler(void)
{
  LOG(0xe013u);
  LL_EXTI_ClearFallingFlag_0_31(LL_EXTI_LINE_13);
}

int main(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA | LL_AHB2_GRP1_PERIPH_GPIOC);

  LL_GPIO_InitTypeDef output = {
      .Pin        = LL_GPIO_PIN_5,
      .Mode       = LL_GPIO_MODE_OUTPUT,
      .Speed      = LL_GPIO_SPEED_FREQ_LOW,
      .OutputType = LL_GPIO_OUTPUT_PUSHPULL,
      .Pull       = LL_GPIO_PULL_NO,
      .Alternate  = LL_GPIO_AF_0,
  };
  LL_GPIO_Init(GPIOA, &output);
  LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_5);
  LOG(PA5_LEVEL());
  LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_5);
  LOG(PA5_LEVEL());
  LL_GPIO_TogglePin(GPIOA, LL_GPIO_PIN_5);
  LOG(PA5_LEVEL());
  WRITE_REG(GPIOA->BSRR, GPIO_BSRR_BR5);
  WRITE_REG(GPIOA->BSRR, GPIO_BSRR_BS5 | GPIO_BSRR_BR5);
  LOG(PA5_LEVEL());

  LL_GPIO_InitTypeDef button = {
      .Pin        = LL_GPIO_PIN_13,
      .Mode       = LL_GPIO_MODE_INPUT,
      .Speed      = LL_GPIO_SPEED_FREQ_LOW,
      .OutputType = LL_GPIO_OUTPUT_PUSHPULL,
      .Pull       = LL_GPIO_PULL_UP,
      .Alternate  = LL_GPIO_AF_0,
  };
  LL_GPIO_Init(GPIOC, &button);
  LOG((LL_GPIO_ReadInputPort(GPIOC) >> 13) & 1u);

  LL_EXTI_SetEXTISource(LL_EXTI_EXTI_PORTC, LL_EXTI_EXTI_LINE13);
  LL_EXTI_EnableFallingTrig_0_31(LL_EXTI_LINE_13);
  LL_EXTI_EnableIT_0_31(LL_EXTI_LINE_13);
  NVIC_EnableIRQ(EXTI13_IRQn);
  __asm__ volatile("bkpt #3" ::: "memory");
  LOG(0xb3u);

  LL_GPIO_LockPin(GPIOA, LL_GPIO_PIN_5);
  LOG(LL_GPIO_IsAnyPinLocked(GPIOA));
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_5, LL_GPIO_MODE_INPUT);
  LOG(LL_GPIO_GetPinMode(GPIOA, LL_GPIO_PIN_5) == LL_GPIO_MODE_OUTPUT);

  LL_EXTI_EnableIT_0_31(LL_EXTI_LINE_0);
  NVIC_EnableIRQ(EXTI0_IRQn);
  LL_EXTI_GenerateSWI_0_31(LL_EXTI_LINE_0);
  __DSB();
  __ISB();
  LOG(0x5e1u);
  DONE();
}
