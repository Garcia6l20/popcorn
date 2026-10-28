#include "stm32h5xx_hal.h"
#include "tusb.h"
#include "usb_descriptors.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#define CONFIGURED_SETTLED 2u

volatile uint32_t usb_mounted;
volatile uint32_t cdc_connected;
volatile uint32_t usb_suspends;
static uint32_t   mounted_at;
static bool       announced;

void     SysTick_Handler(void);
void     USB_DRD_FS_IRQHandler(void);
uint32_t tusb_time_millis_api(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void USB_DRD_FS_IRQHandler(void)
{
  tud_int_handler(0);
}

uint32_t tusb_time_millis_api(void)
{
  return HAL_GetTick();
}

__attribute__((noipa)) void usb_configured(void)
{}

__attribute__((noinline)) static void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

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
      .PeriphClockSelection = RCC_PERIPHCLK_USB,
      .UsbClockSelection    = RCC_USBCLKSOURCE_HSI48,
  };
  if(HAL_RCCEx_PeriphCLKConfig(&peripherals) != HAL_OK)
  {
    Error_Handler();
  }
}

static void usb_hardware_init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef usb_pins = {
      .Pin       = GPIO_PIN_11 | GPIO_PIN_12,
      .Mode      = GPIO_MODE_AF_PP,
      .Pull      = GPIO_NOPULL,
      .Speed     = GPIO_SPEED_FREQ_HIGH,
      .Alternate = GPIO_AF10_USB,
  };
  HAL_GPIO_Init(GPIOA, &usb_pins);
  HAL_PWREx_EnableVddUSB();
  __HAL_RCC_USB_CLK_ENABLE();
}

static void cdc_task(void)
{
  uint8_t buffer[64];
  if(!tud_cdc_connected())
  {
    return;
  }
  while(tud_cdc_available() > 0 && tud_cdc_write_available() > 0)
  {
    uint32_t room  = tud_cdc_write_available();
    uint32_t count = tud_cdc_read(buffer, room < sizeof buffer ? room : sizeof buffer);
    for(uint32_t i = 0; i < count; ++i)
    {
      buffer[i] = (uint8_t)toupper(buffer[i]);
    }
    tud_cdc_write(buffer, count);
  }
  tud_cdc_write_flush();
}

static void configured_task(void)
{
  if(usb_mounted && !announced && HAL_GetTick() - mounted_at >= CONFIGURED_SETTLED)
  {
    announced = true;
    usb_configured();
  }
}

void tud_mount_cb(void)
{
  usb_mounted = 1;
  mounted_at  = HAL_GetTick();
}

void tud_suspend_cb(bool remote_wakeup_en)
{
  (void)remote_wakeup_en;
  ++usb_suspends;
}

void tud_umount_cb(void)
{
  usb_mounted = 0;
  announced   = false;
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  usb_hardware_init();
  tusb_rhport_init_t const device = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
  if(!tusb_init(0, &device))
  {
    Error_Handler();
  }
  for(;;)
  {
    tud_task();
    cdc_connected = tud_cdc_connected() ? 1 : 0;
    cdc_task();
    configured_task();
    __disable_irq();
    if(!tud_task_event_ready())
    {
      __WFI();
    }
    __enable_irq();
  }
}
