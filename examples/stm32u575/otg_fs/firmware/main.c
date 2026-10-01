#include "stm32u5xx_hal.h"

#include <stdint.h>

#define NOT_RUN     0xffffffffu
#define RX_FIFO     0x80u
#define EP0_TX_FIFO 0x40u
#define EP1_TX_FIFO 0x80u

PCD_HandleTypeDef hpcd_USB_OTG_FS;
volatile uint32_t init_status  = NOT_RUN;
volatile uint32_t start_status = NOT_RUN;

void SysTick_Handler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

__attribute__((noinline)) static void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void usb_core_ready(void)
{}

static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef oscillators = {
      .OscillatorType = RCC_OSCILLATORTYPE_HSI48,
      .HSI48State     = RCC_HSI48_ON,
      .PLL.PLLState   = RCC_PLL_NONE,
  };
  if(HAL_RCC_OscConfig(&oscillators) != HAL_OK)
  {
    Error_Handler();
  }
  RCC_PeriphCLKInitTypeDef peripherals = {
      .PeriphClockSelection = RCC_PERIPHCLK_ICLK,
      .IclkClockSelection   = RCC_CLK48CLKSOURCE_HSI48,
  };
  if(HAL_RCCEx_PeriphCLKConfig(&peripherals) != HAL_OK)
  {
    Error_Handler();
  }
}

void HAL_PCD_MspInit(PCD_HandleTypeDef* hpcd)
{
  if(hpcd->Instance != USB_OTG_FS)
  {
    return;
  }
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef pins = {
      .Pin       = GPIO_PIN_11 | GPIO_PIN_12,
      .Mode      = GPIO_MODE_AF_PP,
      .Pull      = GPIO_NOPULL,
      .Speed     = GPIO_SPEED_FREQ_HIGH,
      .Alternate = GPIO_AF10_USB,
  };
  HAL_GPIO_Init(GPIOA, &pins);
  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWREx_EnableVddUSB();
  __HAL_RCC_USB_OTG_FS_CLK_ENABLE();
}

static void MX_USB_OTG_FS_PCD_Init(void)
{
  hpcd_USB_OTG_FS.Instance                     = USB_OTG_FS;
  hpcd_USB_OTG_FS.Init.dev_endpoints           = 6;
  hpcd_USB_OTG_FS.Init.speed                   = PCD_SPEED_FULL;
  hpcd_USB_OTG_FS.Init.phy_itface              = PCD_PHY_EMBEDDED;
  hpcd_USB_OTG_FS.Init.Sof_enable              = DISABLE;
  hpcd_USB_OTG_FS.Init.low_power_enable        = DISABLE;
  hpcd_USB_OTG_FS.Init.lpm_enable              = DISABLE;
  hpcd_USB_OTG_FS.Init.battery_charging_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.use_dedicated_ep1       = DISABLE;
  hpcd_USB_OTG_FS.Init.vbus_sensing_enable     = DISABLE;
  init_status                                  = HAL_PCD_Init(&hpcd_USB_OTG_FS);
  if(init_status != HAL_OK)
  {
    Error_Handler();
  }
  HAL_PCDEx_SetRxFiFo(&hpcd_USB_OTG_FS, RX_FIFO);
  HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_FS, 0, EP0_TX_FIFO);
  HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_FS, 1, EP1_TX_FIFO);
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_USB_OTG_FS_PCD_Init();
  start_status = HAL_PCD_Start(&hpcd_USB_OTG_FS);
  usb_core_ready();
  for(;;)
  {
    __WFI();
  }
}
