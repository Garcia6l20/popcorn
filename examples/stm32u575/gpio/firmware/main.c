#include "main.h"

volatile uint32_t presses;

static void MX_GPIO_Init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
  GPIO_InitTypeDef led = {
      .Pin   = LED_PIN,
      .Mode  = GPIO_MODE_OUTPUT_PP,
      .Pull  = GPIO_NOPULL,
      .Speed = GPIO_SPEED_FREQ_LOW,
  };
  HAL_GPIO_Init(LED_PORT, &led);

  GPIO_InitTypeDef button = {
      .Pin  = BUTTON_PIN,
      .Mode = GPIO_MODE_IT_FALLING,
      .Pull = GPIO_PULLUP,
  };
  HAL_GPIO_Init(BUTTON_PORT, &button);

  HAL_NVIC_SetPriority(EXTI13_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI13_IRQn);
}

void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin)
{
  if(GPIO_Pin == BUTTON_PIN)
  {
    ++presses;
    HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
  }
}

int main(void)
{
  HAL_Init();
  MX_GPIO_Init();
  for(;;)
  {
    __WFI();
  }
}
