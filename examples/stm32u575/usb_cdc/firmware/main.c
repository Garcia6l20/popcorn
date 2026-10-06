#include "stm32u5xx_hal.h"
#include "tusb.h"
#include "usb_descriptors.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BUTTON_PORT        GPIOC
#define BUTTON_PIN         GPIO_PIN_13
#define CONFIGURED_SETTLED 2u

volatile uint32_t usb_mounted;
volatile uint32_t cdc_connected;
volatile uint32_t hid_reports;
volatile uint32_t hid_led;
volatile uint32_t hid_led_writes;
volatile uint32_t hid_echoes;
uint8_t           hid_feature[HID_IO_FEATURE_SIZE] = {0xf0, 0xf1, 0xf2, 0xf3};
static uint8_t    hid_echo[HID_IO_REPORT_SIZE];
static uint16_t   hid_echo_length;
static bool       hid_echo_pending;
volatile uint32_t usb_suspends;
static uint32_t   mounted_at;
static bool       announced;

void     SysTick_Handler(void);
void     OTG_FS_IRQHandler(void);
uint32_t tusb_time_millis_api(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void OTG_FS_IRQHandler(void)
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
      .PeriphClockSelection = RCC_PERIPHCLK_ICLK,
      .IclkClockSelection   = RCC_CLK48CLKSOURCE_HSI48,
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
  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWREx_EnableVddUSB();
  __HAL_RCC_USB_OTG_FS_CLK_ENABLE();
}

static void button_init(void)
{
  __HAL_RCC_GPIOC_CLK_ENABLE();
  GPIO_InitTypeDef button = {
      .Pin  = BUTTON_PIN,
      .Mode = GPIO_MODE_INPUT,
      .Pull = GPIO_PULLDOWN,
  };
  HAL_GPIO_Init(BUTTON_PORT, &button);
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

static void hid_task(bool* pressed)
{
  bool const level = HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN) == GPIO_PIN_SET;
  if(level && !*pressed && tud_hid_n_ready(HID_BUTTON))
  {
    uint8_t const report = (uint8_t)(hid_reports + 1);
    if(tud_hid_n_report(HID_BUTTON, REPORT_ID_BUTTON, &report, sizeof report))
    {
      ++hid_reports;
      *pressed = true;
    }
  }
  else if(!level)
  {
    *pressed = false;
  }
  if(hid_echo_pending
     && tud_hid_n_ready(HID_IO)
     && tud_hid_n_report(HID_IO, 0, hid_echo, hid_echo_length))
  {
    hid_echo_pending = false;
    ++hid_echoes;
  }
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

uint16_t tud_hid_get_report_cb(
    uint8_t           instance,
    uint8_t           report_id,
    hid_report_type_t report_type,
    uint8_t*          buffer,
    uint16_t          reqlen)
{
  if(instance == HID_IO && report_type == HID_REPORT_TYPE_FEATURE)
  {
    uint16_t const count = reqlen < sizeof hid_feature ? reqlen : (uint16_t)sizeof hid_feature;
    memcpy(buffer, hid_feature, count);
    return count;
  }
  if(instance != HID_BUTTON
     || report_type != HID_REPORT_TYPE_INPUT
     || report_id != REPORT_ID_BUTTON
     || reqlen < 1)
  {
    return 0;
  }
  buffer[0] = (uint8_t)hid_reports;
  return 1;
}

void tud_hid_set_report_cb(
    uint8_t           instance,
    uint8_t           report_id,
    hid_report_type_t report_type,
    uint8_t const*    buffer,
    uint16_t          bufsize)
{
  if(instance == HID_BUTTON
     && report_id == REPORT_ID_LED
     && report_type == HID_REPORT_TYPE_OUTPUT
     && bufsize >= 1)
  {
    hid_led = buffer[0];
    ++hid_led_writes;
  }
  else if(instance == HID_IO && report_type == HID_REPORT_TYPE_FEATURE)
  {
    memcpy(hid_feature, buffer, bufsize < sizeof hid_feature ? bufsize : sizeof hid_feature);
  }
  else if(instance == HID_IO && report_type == HID_REPORT_TYPE_OUTPUT && !hid_echo_pending)
  {
    hid_echo_length = bufsize < sizeof hid_echo ? bufsize : (uint16_t)sizeof hid_echo;
    for(uint16_t i = 0; i < hid_echo_length; ++i)
    {
      hid_echo[i] = (uint8_t)~buffer[i];
    }
    hid_echo_pending = true;
  }
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  button_init();
  usb_hardware_init();
  tusb_rhport_init_t const device = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
  if(!tusb_init(0, &device))
  {
    Error_Handler();
  }
  bool pressed = false;
  for(;;)
  {
    tud_task();
    cdc_connected = tud_cdc_connected() ? 1 : 0;
    cdc_task();
    hid_task(&pressed);
    configured_task();
    __disable_irq();
    if(!tud_task_event_ready())
    {
      __WFI();
    }
    __enable_irq();
  }
}
