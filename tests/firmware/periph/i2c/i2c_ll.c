#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_i2c.h"

#define EEPROM_ADDRESS    (0x50u << 1)
#define TABLE_ADDRESS     (0x38u << 1)
#define ABSENT_ADDRESS    (0x2Au << 1)
#define TIMING_400K_AT_4M 0x00100305u

static uint8_t           buffer[300];
static volatile uint32_t nack_entries;
static volatile uint32_t stop_entries;
static volatile uint32_t event_entries;

void I2C1_EV_IRQHandler(void);
void idle(void);
void eeprom_rw(void);
void nack(void);
void table(void);
void reload(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void I2C1_EV_IRQHandler(void)
{
  ++event_entries;
  if(LL_I2C_IsActiveFlag_NACK(I2C1))
  {
    LL_I2C_ClearFlag_NACK(I2C1);
    ++nack_entries;
  }
  if(LL_I2C_IsActiveFlag_STOP(I2C1))
  {
    LL_I2C_ClearFlag_STOP(I2C1);
    ++stop_entries;
  }
}

static void configure(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_I2C1);
  LL_I2C_InitTypeDef config = {
      .PeripheralMode  = LL_I2C_MODE_I2C,
      .Timing          = TIMING_400K_AT_4M,
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

static void send(uint32_t address, uint8_t const* bytes, uint32_t count, uint32_t end)
{
  LL_I2C_HandleTransfer(
      I2C1, address, LL_I2C_ADDRSLAVE_7BIT, count, end, LL_I2C_GENERATE_START_WRITE);
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

static void receive(uint32_t address, uint8_t* bytes, uint32_t count)
{
  LL_I2C_HandleTransfer(
      I2C1, address, LL_I2C_ADDRSLAVE_7BIT, count, LL_I2C_MODE_AUTOEND, LL_I2C_GENERATE_START_READ);
  for(uint32_t index = 0; index < count; ++index)
  {
    while(!LL_I2C_IsActiveFlag_RXNE(I2C1))
    {
    }
    bytes[index] = LL_I2C_ReceiveData8(I2C1);
  }
  wait_stop();
}

static uint32_t acknowledge_polls(void)
{
  uint32_t polls = 0;
  for(;;)
  {
    LL_I2C_HandleTransfer(
        I2C1, EEPROM_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, 0, LL_I2C_MODE_AUTOEND,
        LL_I2C_GENERATE_START_WRITE);
    wait_stop();
    if(!LL_I2C_IsActiveFlag_NACK(I2C1))
    {
      return polls;
    }
    LL_I2C_ClearFlag_NACK(I2C1);
    ++polls;
  }
}

static uint32_t pattern(uint32_t index)
{
  return (7u * index + 3u) & 0xFFu;
}

void eeprom_rw(void)
{
  configure();
  static uint8_t first[18]  = {0x00, 0x10};
  static uint8_t second[6]  = {0x00, 0x20};
  static uint8_t address[2] = {0x00, 0x10};
  for(uint32_t index = 0; index < 16; ++index)
  {
    first[2 + index] = (uint8_t)pattern(index);
  }
  for(uint32_t index = 0; index < 4; ++index)
  {
    second[2 + index] = (uint8_t)pattern(16 + index);
  }
  send(EEPROM_ADDRESS, first, sizeof first, LL_I2C_MODE_AUTOEND);
  uint32_t const first_polls = acknowledge_polls();
  send(EEPROM_ADDRESS, second, sizeof second, LL_I2C_MODE_AUTOEND);
  uint32_t const second_polls = acknowledge_polls();
  send(EEPROM_ADDRESS, address, sizeof address, LL_I2C_MODE_SOFTEND);
  receive(EEPROM_ADDRESS, buffer, 20);
  uint32_t equal = 0;
  for(uint32_t index = 0; index < 20; ++index)
  {
    equal += buffer[index] == pattern(index) ? 1u : 0u;
  }
  LOG(equal);
  LOG(first_polls);
  LOG(second_polls);
  DONE();
}

void nack(void)
{
  configure();
  LL_I2C_EnableIT_NACK(I2C1);
  LL_I2C_EnableIT_STOP(I2C1);
  NVIC_EnableIRQ(I2C1_EV_IRQn);
  LL_I2C_HandleTransfer(
      I2C1, ABSENT_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, 1, LL_I2C_MODE_AUTOEND,
      LL_I2C_GENERATE_START_WRITE);
  while(stop_entries == 0)
  {
  }
  LOG(nack_entries);
  LOG(stop_entries);
  LOG(event_entries);
  LOG(LL_I2C_IsActiveFlag_BUSY(I2C1));
  DONE();
}

void table(void)
{
  configure();
  static uint8_t const first[3]  = {0x40, 0x00, 0x01};
  static uint8_t const second[4] = {0x40, 0x01, 0x02, 0x03};
  static uint8_t const third[2]  = {0x40, 0x02};
  send(TABLE_ADDRESS, first, sizeof first, LL_I2C_MODE_AUTOEND);
  send(TABLE_ADDRESS, second, sizeof second, LL_I2C_MODE_AUTOEND);
  send(TABLE_ADDRESS, third, sizeof third, LL_I2C_MODE_SOFTEND);
  receive(TABLE_ADDRESS, buffer, 2);
  LOG(buffer[0]);
  LOG(buffer[1]);
  DONE();
}

void reload(void)
{
  configure();
  static uint8_t const address[2] = {0x00, 0x00};
  send(EEPROM_ADDRESS, address, sizeof address, LL_I2C_MODE_SOFTEND);
  LL_I2C_HandleTransfer(
      I2C1, EEPROM_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, 100, LL_I2C_MODE_RELOAD,
      LL_I2C_GENERATE_START_READ);
  uint32_t reloads = 0;
  for(uint32_t index = 0; index < sizeof buffer; ++index)
  {
    if(index != 0 && index % 100 == 0)
    {
      while(!LL_I2C_IsActiveFlag_TCR(I2C1))
      {
      }
      ++reloads;
      uint32_t const mode = index == 200 ? LL_I2C_MODE_AUTOEND : LL_I2C_MODE_RELOAD;
      LL_I2C_HandleTransfer(
          I2C1, EEPROM_ADDRESS, LL_I2C_ADDRSLAVE_7BIT, 100, mode, LL_I2C_GENERATE_NOSTARTSTOP);
    }
    while(!LL_I2C_IsActiveFlag_RXNE(I2C1))
    {
    }
    buffer[index] = LL_I2C_ReceiveData8(I2C1);
  }
  wait_stop();
  uint32_t equal = 0;
  for(uint32_t index = 0; index < sizeof buffer; ++index)
  {
    equal += buffer[index] == (uint8_t)index ? 1u : 0u;
  }
  LOG(equal);
  LOG(reloads);
  DONE();
}

int main(void)
{
  idle();
}
