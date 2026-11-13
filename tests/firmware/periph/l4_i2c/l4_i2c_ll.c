#include "i2c_target.h"
#include "periph_test.h"
#include "stm32l4xx.h"
#include "stm32l4xx_ll_bus.h"
#include "stm32l4xx_ll_i2c.h"
#include "stm32l4xx_ll_rcc.h"

#include <stdbool.h>

#define EEPROM_ADDRESS     (0x50u << 1)
#define TIMING_400K_AT_4M  0x00100305u
#define TIMING_100K_AT_16M 0x00303D5Bu
#define MAX_POLLS          1000u
#define MAX_ATTEMPTS       4u

static uint8_t buffer[16];

void idle(void);
void hsi16_100k(void);
void I2C1_EV_IRQHandler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void I2C1_EV_IRQHandler(void)
{
  i2c_target_event();
}

static void configure(uint32_t timing)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_I2C1);
  LL_I2C_InitTypeDef config = {
      .PeripheralMode  = LL_I2C_MODE_I2C,
      .Timing          = timing,
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

static bool send(uint8_t const* bytes, uint32_t count, uint32_t end)
{
  LL_I2C_HandleTransfer(
      I2C1, EEPROM_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, count, end, LL_I2C_GENERATE_START_WRITE);
  for(uint32_t index = 0; index < count; ++index)
  {
    while(!LL_I2C_IsActiveFlag_TXIS(I2C1))
    {
      if(LL_I2C_IsActiveFlag_NACK(I2C1))
      {
        wait_stop();
        LL_I2C_ClearFlag_NACK(I2C1);
        return false;
      }
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
  if(LL_I2C_IsActiveFlag_NACK(I2C1))
  {
    LL_I2C_ClearFlag_NACK(I2C1);
    return false;
  }
  return true;
}

static uint32_t polls(void)
{
  uint32_t count = 0;
  for(;;)
  {
    if(count >= MAX_POLLS)
    {
      FAIL(1);
    }
    LL_I2C_HandleTransfer(
        I2C1, EEPROM_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, 0, LL_I2C_MODE_AUTOEND,
        LL_I2C_GENERATE_START_WRITE);
    wait_stop();
    if(!LL_I2C_IsActiveFlag_NACK(I2C1))
    {
      return count;
    }
    LL_I2C_ClearFlag_NACK(I2C1);
    ++count;
  }
}

static void send_page(uint8_t const* bytes, uint32_t count)
{
  for(uint32_t attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
  {
    if(send(bytes, count, LL_I2C_MODE_AUTOEND))
    {
      return;
    }
    if(attempt + 1 < MAX_ATTEMPTS)
    {
      polls();
    }
  }
  FAIL(2);
}

static void eeprom_rw(uint32_t timing)
{
  configure(timing);
  static uint8_t page[10]   = {0x00, 0x20};
  static uint8_t address[2] = {0x00, 0x20};
  for(uint32_t index = 0; index < 8u; ++index)
  {
    page[2 + index] = (uint8_t)(0x30u + index);
  }
  send_page(page, sizeof page);
  uint32_t const waited = polls();
  send(address, sizeof address, LL_I2C_MODE_SOFTEND);
  LL_I2C_HandleTransfer(
      I2C1, EEPROM_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, 8u, LL_I2C_MODE_AUTOEND,
      LL_I2C_GENERATE_START_READ);
  for(uint32_t index = 0; index < 8u; ++index)
  {
    while(!LL_I2C_IsActiveFlag_RXNE(I2C1))
    {
    }
    buffer[index] = LL_I2C_ReceiveData8(I2C1);
  }
  wait_stop();
  uint32_t equal = 0;
  for(uint32_t index = 0; index < 8u; ++index)
  {
    equal += buffer[index] == page[2 + index] ? 1u : 0u;
  }
  LOG(equal);
  LOG(waited != 0u);
  LOG(I2C1->TIMINGR);
}

int main(void)
{
  eeprom_rw(TIMING_400K_AT_4M);
  DONE();
}

void hsi16_100k(void)
{
  LL_RCC_HSI_Enable();
  while(!LL_RCC_HSI_IsReady())
  {
  }
  LL_RCC_SetI2CClockSource(LL_RCC_I2C1_CLKSOURCE_HSI);
  eeprom_rw(TIMING_100K_AT_16M);
  DONE();
}
