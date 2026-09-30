#include "main.h"

#include "stm32u5xx.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_gpio.h"
#include "stm32u5xx_ll_i2c.h"
#include "stm32u5xx_ll_tim.h"

#define I2C_TIMING_FM_AT_4MHZ 0x00100305u
#define TIMER_CLOCK_HZ        10000u
#define SAMPLE_PERIOD_MS      10u

struct sensor_result     sensor_result;
static volatile uint32_t ticks;

void TIM6_IRQHandler(void);

__attribute__((noipa)) void sample_taken(void)
{}

void TIM6_IRQHandler(void)
{
  if(LL_TIM_IsActiveFlag_UPDATE(TIM6))
  {
    LL_TIM_ClearFlag_UPDATE(TIM6);
    ++ticks;
  }
}

static void MX_GPIO_Init(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
  LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_5);
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_5, LL_GPIO_MODE_OUTPUT);
}

static void MX_I2C1_Init(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOB);
  LL_GPIO_InitTypeDef pins = {
      .Pin        = LL_GPIO_PIN_6 | LL_GPIO_PIN_7,
      .Mode       = LL_GPIO_MODE_ALTERNATE,
      .Speed      = LL_GPIO_SPEED_FREQ_HIGH,
      .OutputType = LL_GPIO_OUTPUT_OPENDRAIN,
      .Pull       = LL_GPIO_PULL_UP,
      .Alternate  = LL_GPIO_AF_4,
  };
  LL_GPIO_Init(GPIOB, &pins);

  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_I2C1);
  LL_I2C_InitTypeDef config = {
      .PeripheralMode  = LL_I2C_MODE_I2C,
      .Timing          = I2C_TIMING_FM_AT_4MHZ,
      .AnalogFilter    = LL_I2C_ANALOGFILTER_ENABLE,
      .DigitalFilter   = 0,
      .OwnAddress1     = 0,
      .TypeAcknowledge = LL_I2C_ACK,
      .OwnAddrSize     = LL_I2C_OWNADDRESS1_7BIT,
  };
  LL_I2C_Init(I2C1, &config);
}

static void MX_TIM6_Init(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
  LL_TIM_SetPrescaler(TIM6, __LL_TIM_CALC_PSC(SystemCoreClock, TIMER_CLOCK_HZ));
  LL_TIM_SetAutoReload(TIM6, TIMER_CLOCK_HZ / 1000u * SAMPLE_PERIOD_MS - 1u);
  LL_TIM_GenerateEvent_UPDATE(TIM6);
  LL_TIM_ClearFlag_UPDATE(TIM6);
  LL_TIM_EnableIT_UPDATE(TIM6);
  NVIC_SetPriority(TIM6_IRQn, 2);
  NVIC_EnableIRQ(TIM6_IRQn);
  LL_TIM_EnableCounter(TIM6);
}

static void wait_stop(void)
{
  while(!LL_I2C_IsActiveFlag_STOP(I2C1))
  {
  }
  LL_I2C_ClearFlag_STOP(I2C1);
}

static bool nacked(void)
{
  if(!LL_I2C_IsActiveFlag_NACK(I2C1))
  {
    return false;
  }
  wait_stop();
  LL_I2C_ClearFlag_NACK(I2C1);
  return true;
}

static bool write_bytes(uint8_t const* bytes, uint32_t count, uint32_t end)
{
  LL_I2C_HandleTransfer(
      I2C1, SENSOR_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, count, end, LL_I2C_GENERATE_START_WRITE);
  for(uint32_t index = 0; index < count; ++index)
  {
    while(!LL_I2C_IsActiveFlag_TXIS(I2C1))
    {
      if(nacked())
      {
        return false;
      }
    }
    LL_I2C_TransmitData8(I2C1, bytes[index]);
  }
  if(end == LL_I2C_MODE_AUTOEND)
  {
    wait_stop();
    return !nacked();
  }
  while(!LL_I2C_IsActiveFlag_TC(I2C1))
  {
    if(nacked())
    {
      return false;
    }
  }
  return true;
}

static bool read_bytes(uint8_t* bytes, uint32_t count)
{
  LL_I2C_HandleTransfer(
      I2C1, SENSOR_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, count, LL_I2C_MODE_AUTOEND,
      LL_I2C_GENERATE_START_READ);
  for(uint32_t index = 0; index < count; ++index)
  {
    while(!LL_I2C_IsActiveFlag_RXNE(I2C1))
    {
      if(nacked())
      {
        return false;
      }
    }
    bytes[index] = LL_I2C_ReceiveData8(I2C1);
  }
  wait_stop();
  return true;
}

static void configure_sensor(void)
{
  static uint8_t const config[] = {
      SENSOR_CONFIG, SENSOR_CONFIG_VALUE >> 8, SENSOR_CONFIG_VALUE & 0xFFu};
  if(!write_bytes(config, sizeof config, LL_I2C_MODE_AUTOEND))
  {
    ++sensor_result.errors;
  }
}

static void take_sample(void)
{
  static uint8_t const pointer = SENSOR_TEMPERATURE;
  uint8_t              raw[2];
  if(!write_bytes(&pointer, 1, LL_I2C_MODE_SOFTEND) || !read_bytes(raw, sizeof raw))
  {
    ++sensor_result.errors;
    sample_taken();
    return;
  }
  int32_t const counts = (int16_t)(raw[0] << 8 | raw[1]) >> 4;
  sensor_result.sum += counts;
  ++sensor_result.samples;
  sensor_result.mean = sensor_result.sum / (int32_t)sensor_result.samples;
  if(sensor_result.mean > OVER_TEMPERATURE_COUNTS)
  {
    LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_5);
  }
  else
  {
    LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_5);
  }
  sample_taken();
}

int main(void)
{
  MX_GPIO_Init();
  MX_I2C1_Init();
  configure_sensor();
  MX_TIM6_Init();
  uint32_t seen = 0;
  for(;;)
  {
    __disable_irq();
    if(ticks == seen)
    {
      __WFI();
    }
    __enable_irq();
    if(ticks != seen)
    {
      seen = ticks;
      take_sample();
    }
  }
}
