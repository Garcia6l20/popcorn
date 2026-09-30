#include "main.h"

#include "stm32u5xx.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_gpio.h"
#include "stm32u5xx_ll_i2c.h"

#define I2C_TIMING_FM_AT_4MHZ 0x00100305u

struct eeprom_result eeprom_result;

__attribute__((noipa)) void eeprom_done(void)
{}

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

static void wait_stop(void)
{
  while(!LL_I2C_IsActiveFlag_STOP(I2C1))
  {
  }
  LL_I2C_ClearFlag_STOP(I2C1);
}

static void write_bytes(uint8_t const* bytes, uint32_t count, uint32_t end)
{
  LL_I2C_HandleTransfer(
      I2C1, EEPROM_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, count, end, LL_I2C_GENERATE_START_WRITE);
  for(uint32_t index = 0; index < count; ++index)
  {
    while(!LL_I2C_IsActiveFlag_TXIS(I2C1))
    {
    }
    LL_I2C_TransmitData8(I2C1, bytes[index]);
  }
  if(end == LL_I2C_MODE_AUTOEND)
  {
    wait_stop();
  }
  else
  {
    while(!LL_I2C_IsActiveFlag_TC(I2C1))
    {
    }
  }
}

static void read_bytes(uint8_t* bytes, uint32_t count)
{
  LL_I2C_HandleTransfer(
      I2C1, EEPROM_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, count, LL_I2C_MODE_AUTOEND,
      LL_I2C_GENERATE_START_READ);
  for(uint32_t index = 0; index < count; ++index)
  {
    while(!LL_I2C_IsActiveFlag_RXNE(I2C1))
    {
    }
    bytes[index] = LL_I2C_ReceiveData8(I2C1);
  }
  wait_stop();
}

static uint32_t wait_write_cycle(void)
{
  uint32_t nacked = 0;
  for(;;)
  {
    LL_I2C_HandleTransfer(
        I2C1, EEPROM_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, 0, LL_I2C_MODE_AUTOEND,
        LL_I2C_GENERATE_START_WRITE);
    wait_stop();
    if(!LL_I2C_IsActiveFlag_NACK(I2C1))
    {
      return nacked;
    }
    LL_I2C_ClearFlag_NACK(I2C1);
    ++nacked;
  }
}

int main(void)
{
  MX_I2C1_Init();

  static uint8_t page[2 + EEPROM_PAGE_BYTES] = {EEPROM_PAGE >> 8, EEPROM_PAGE & 0xFF};
  for(uint32_t index = 0; index < EEPROM_PAGE_BYTES; ++index)
  {
    eeprom_result.written[index] = (uint8_t)(0x40 + 3 * index);
    page[2 + index]              = eeprom_result.written[index];
  }
  write_bytes(page, sizeof page, LL_I2C_MODE_AUTOEND);
  eeprom_result.nacked_polls = wait_write_cycle();

  write_bytes(page, 2, LL_I2C_MODE_SOFTEND);
  read_bytes(eeprom_result.readback, EEPROM_PAGE_BYTES);

  eeprom_result.done = 1;
  eeprom_done();
  for(;;)
  {
    __WFI();
  }
}
