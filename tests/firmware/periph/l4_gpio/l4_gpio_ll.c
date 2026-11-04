#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_gpio.h"

#define PA5_LEVEL() ((LL_GPIO_ReadInputPort(GPIOA) >> 5) & 1u)

void idle(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
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
  WRITE_REG(GPIOA->BSRR, GPIO_BSRR_BS5 | GPIO_BSRR_BR5);
  LOG(PA5_LEVEL());
  WRITE_REG(GPIOA->BRR, GPIO_BRR_BR5);
  LOG(PA5_LEVEL());

  LL_GPIO_InitTypeDef usart = {
      .Pin        = LL_GPIO_PIN_2 | LL_GPIO_PIN_10,
      .Mode       = LL_GPIO_MODE_ALTERNATE,
      .Speed      = LL_GPIO_SPEED_FREQ_HIGH,
      .OutputType = LL_GPIO_OUTPUT_PUSHPULL,
      .Pull       = LL_GPIO_PULL_UP,
      .Alternate  = LL_GPIO_AF_7,
  };
  LL_GPIO_Init(GPIOA, &usart);
  LOG(LL_GPIO_GetAFPin_0_7(GPIOA, LL_GPIO_PIN_2));
  LOG(LL_GPIO_GetAFPin_8_15(GPIOA, LL_GPIO_PIN_10));
  LOG(GPIOA->AFR[0]);
  LOG(GPIOA->AFR[1]);

  LL_GPIO_InitTypeDef button = {
      .Pin        = LL_GPIO_PIN_13,
      .Mode       = LL_GPIO_MODE_INPUT,
      .Speed      = LL_GPIO_SPEED_FREQ_LOW,
      .OutputType = LL_GPIO_OUTPUT_PUSHPULL,
      .Pull       = LL_GPIO_PULL_UP,
      .Alternate  = LL_GPIO_AF_0,
  };
  LL_GPIO_Init(GPIOC, &button);
  LOG(LL_GPIO_IsInputPinSet(GPIOC, LL_GPIO_PIN_13));

  LL_GPIO_InitTypeDef analog = {
      .Pin        = LL_GPIO_PIN_0,
      .Mode       = LL_GPIO_MODE_ANALOG,
      .Speed      = LL_GPIO_SPEED_FREQ_LOW,
      .OutputType = LL_GPIO_OUTPUT_PUSHPULL,
      .Pull       = LL_GPIO_PULL_NO,
      .Alternate  = LL_GPIO_AF_0,
  };
  LL_GPIO_Init(GPIOC, &analog);
  LL_GPIO_EnablePinAnalogControl(GPIOC, LL_GPIO_PIN_0);
  LOG(GPIOC->ASCR);

  LL_GPIO_LockPin(GPIOA, LL_GPIO_PIN_5);
  LOG(LL_GPIO_IsAnyPinLocked(GPIOA));
  LOG(LL_GPIO_IsPinLocked(GPIOA, LL_GPIO_PIN_5));
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_5, LL_GPIO_MODE_INPUT);
  LOG(LL_GPIO_GetPinMode(GPIOA, LL_GPIO_PIN_5) == LL_GPIO_MODE_OUTPUT);
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_6, LL_GPIO_MODE_OUTPUT);
  LOG(LL_GPIO_GetPinMode(GPIOA, LL_GPIO_PIN_6) == LL_GPIO_MODE_OUTPUT);
  DONE();
}
