#include "stm32u5xx_hal.h"
#include "tusb.h"
#include "usb_descriptors.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define FEATURE_UNITS   2
#define VOLUME_MIN      (-50 * 256)
#define VOLUME_MAX      0
#define VOLUME_RES      256
#define LOOPBACK_BUFFER (4 * AUDIO_EP_SIZE)

volatile uint32_t     usb_mounted;
volatile uint32_t     speaker_alt;
volatile uint32_t     mic_alt;
volatile uint32_t     speaker_bytes;
volatile uint32_t     mic_bytes;
volatile uint32_t     clock_set_requests;
static uint32_t const sample_rate = AUDIO_SAMPLE_RATE;
uint8_t               feature_mute[FEATURE_UNITS][AUDIO_CHANNELS + 1];
int16_t               feature_volume[FEATURE_UNITS][AUDIO_CHANNELS + 1];
static uint8_t        loopback[LOOPBACK_BUFFER];

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

static void loopback_task(void)
{
  if(speaker_alt == 0)
  {
    return;
  }
  uint16_t const count = tud_audio_read(loopback, sizeof loopback);
  if(count == 0)
  {
    return;
  }
  speaker_bytes += count;
  if(mic_alt != 0)
  {
    mic_bytes += tud_audio_write(loopback, count);
  }
}

static int feature_unit(uint8_t entity)
{
  if(entity == ENTITY_SPEAKER_FEATURE)
  {
    return 0;
  }
  if(entity == ENTITY_MIC_FEATURE)
  {
    return 1;
  }
  return -1;
}

static bool clock_get_request(uint8_t rhport, tusb_control_request_t const* request)
{
  uint8_t const selector = TU_U16_HIGH(request->wValue);
  if(selector == AUDIO20_CS_CTRL_SAM_FREQ && request->bRequest == AUDIO20_CS_REQ_CUR)
  {
    audio20_control_cur_4_t current = {.bCur = (int32_t)tu_htole32(sample_rate)};
    return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &current, sizeof current);
  }
  if(selector == AUDIO20_CS_CTRL_SAM_FREQ && request->bRequest == AUDIO20_CS_REQ_RANGE)
  {
    audio20_control_range_4_n_t(1) range = {
        .wNumSubRanges = tu_htole16(1),
        .subrange[0]   = {.bMin = (int32_t)sample_rate, .bMax = (int32_t)sample_rate, .bRes = 0},
    };
    return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &range, sizeof range);
  }
  if(selector == AUDIO20_CS_CTRL_CLK_VALID && request->bRequest == AUDIO20_CS_REQ_CUR)
  {
    audio20_control_cur_1_t valid = {.bCur = 1};
    return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &valid, sizeof valid);
  }
  return false;
}

static bool feature_get_request(uint8_t rhport, tusb_control_request_t const* request, int unit)
{
  uint8_t const selector = TU_U16_HIGH(request->wValue);
  uint8_t const channel  = TU_U16_LOW(request->wValue);
  if(channel > AUDIO_CHANNELS)
  {
    return false;
  }
  if(selector == AUDIO20_FU_CTRL_MUTE && request->bRequest == AUDIO20_CS_REQ_CUR)
  {
    audio20_control_cur_1_t mute = {.bCur = feature_mute[unit][channel]};
    return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &mute, sizeof mute);
  }
  if(selector == AUDIO20_FU_CTRL_VOLUME && request->bRequest == AUDIO20_CS_REQ_CUR)
  {
    audio20_control_cur_2_t volume = {.bCur = (int16_t)tu_htole16(feature_volume[unit][channel])};
    return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &volume, sizeof volume);
  }
  if(selector == AUDIO20_FU_CTRL_VOLUME && request->bRequest == AUDIO20_CS_REQ_RANGE)
  {
    audio20_control_range_2_n_t(1) range = {
        .wNumSubRanges = tu_htole16(1),
        .subrange[0]   = {.bMin = VOLUME_MIN, .bMax = VOLUME_MAX, .bRes = VOLUME_RES},
    };
    return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &range, sizeof range);
  }
  return false;
}

static bool
    feature_set_request(tusb_control_request_t const* request, uint8_t const* data, int unit)
{
  uint8_t const selector = TU_U16_HIGH(request->wValue);
  uint8_t const channel  = TU_U16_LOW(request->wValue);
  if(request->bRequest != AUDIO20_CS_REQ_CUR || channel > AUDIO_CHANNELS)
  {
    return false;
  }
  if(selector == AUDIO20_FU_CTRL_MUTE && request->wLength == sizeof(audio20_control_cur_1_t))
  {
    feature_mute[unit][channel] = ((audio20_control_cur_1_t const*)data)->bCur;
    return true;
  }
  if(selector == AUDIO20_FU_CTRL_VOLUME && request->wLength == sizeof(audio20_control_cur_2_t))
  {
    feature_volume[unit][channel] = ((audio20_control_cur_2_t const*)data)->bCur;
    return true;
  }
  return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const* request)
{
  uint8_t const entity = TU_U16_HIGH(request->wIndex);
  if(entity == ENTITY_CLOCK)
  {
    return clock_get_request(rhport, request);
  }
  int const unit = feature_unit(entity);
  return unit >= 0 && feature_get_request(rhport, request, unit);
}

bool tud_audio_set_req_entity_cb(
    uint8_t                       rhport,
    tusb_control_request_t const* request,
    uint8_t*                      data)
{
  (void)rhport;
  uint8_t const entity = TU_U16_HIGH(request->wIndex);
  if(entity == ENTITY_CLOCK)
  {
    ++clock_set_requests;
    return false;
  }
  int const unit = feature_unit(entity);
  return unit >= 0 && feature_set_request(request, data, unit);
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* request)
{
  (void)rhport;
  uint8_t const interface = TU_U16_LOW(request->wIndex);
  uint8_t const alternate = TU_U16_LOW(request->wValue);
  if(interface == ITF_NUM_AUDIO_SPEAKER)
  {
    speaker_alt = alternate;
  }
  else if(interface == ITF_NUM_AUDIO_MIC)
  {
    mic_alt = alternate;
  }
  return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const* request)
{
  (void)rhport;
  uint8_t const interface = TU_U16_LOW(request->wIndex);
  if(interface == ITF_NUM_AUDIO_SPEAKER)
  {
    speaker_alt = 0;
  }
  else if(interface == ITF_NUM_AUDIO_MIC)
  {
    mic_alt = 0;
  }
  return true;
}

void tud_mount_cb(void)
{
  usb_mounted = 1;
}

void tud_umount_cb(void)
{
  usb_mounted = 0;
  speaker_alt = 0;
  mic_alt     = 0;
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  usb_hardware_init();
  for(int unit = 0; unit < FEATURE_UNITS; ++unit)
  {
    memset(feature_mute[unit], 0, sizeof feature_mute[unit]);
    for(int channel = 0; channel <= AUDIO_CHANNELS; ++channel)
    {
      feature_volume[unit][channel] = VOLUME_MAX;
    }
  }
  tusb_rhport_init_t const device = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
  if(!tusb_init(0, &device))
  {
    Error_Handler();
  }
  for(;;)
  {
    tud_task();
    loopback_task();
    __disable_irq();
    if(!tud_task_event_ready())
    {
      __WFI();
    }
    __enable_irq();
  }
}
