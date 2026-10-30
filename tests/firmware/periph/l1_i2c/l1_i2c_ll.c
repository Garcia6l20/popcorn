#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_i2c.h"

#define PCLK1_HZ 2097152u
#define EEPROM   0x50u

void idle(void);
void write_eeprom(void);
void nack(void);
void read_one(void);
void read_two(void);
void read_four(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void start(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_I2C1);
  LL_I2C_ConfigSpeed(I2C1, PCLK1_HZ, 100000u, LL_I2C_DUTYCYCLE_2);
  LL_I2C_Enable(I2C1);
  LL_I2C_GenerateStartCondition(I2C1);
  while(!LL_I2C_IsActiveFlag_SB(I2C1))
  {
  }
}

static void send(uint8_t byte)
{
  while(!LL_I2C_IsActiveFlag_TXE(I2C1))
  {
    if(LL_I2C_IsActiveFlag_AF(I2C1))
    {
      FAIL(1);
    }
  }
  LL_I2C_TransmitData8(I2C1, byte);
}

static void stop(void)
{
  LL_I2C_GenerateStopCondition(I2C1);
  while(LL_I2C_IsActiveFlag_BUSY(I2C1))
  {
  }
  LOG(I2C1->SR1);
  LOG(I2C1->SR2);
}

void write_eeprom(void)
{
  static uint8_t const out[] = {0x00, 0x10, 0xA5, 0x5A, 0x3C};
  start();
  LL_I2C_TransmitData8(I2C1, EEPROM << 1);
  while(!LL_I2C_IsActiveFlag_ADDR(I2C1))
  {
    if(LL_I2C_IsActiveFlag_AF(I2C1))
    {
      FAIL(2);
    }
  }
  LL_I2C_ClearFlag_ADDR(I2C1);
  LOG(I2C1->SR2);
  for(uint32_t index = 0; index < sizeof out; ++index)
  {
    send(out[index]);
  }
  while(!LL_I2C_IsActiveFlag_BTF(I2C1))
  {
  }
  LOG(I2C1->SR1);
  stop();
  DONE();
}

void nack(void)
{
  start();
  LL_I2C_TransmitData8(I2C1, (EEPROM + 1u) << 1);
  while(!LL_I2C_IsActiveFlag_AF(I2C1))
  {
    if(LL_I2C_IsActiveFlag_ADDR(I2C1))
    {
      FAIL(3);
    }
  }
  LOG(I2C1->SR1);
  LOG(I2C1->SR2);
  stop();
  LL_I2C_ClearFlag_AF(I2C1);
  LOG(I2C1->SR1);
  DONE();
}

static void address_read(void)
{
  start();
  LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_ACK);
  LL_I2C_TransmitData8(I2C1, EEPROM << 1 | 1u);
  while(!LL_I2C_IsActiveFlag_ADDR(I2C1))
  {
    if(LL_I2C_IsActiveFlag_AF(I2C1))
    {
      FAIL(4);
    }
  }
}

static void wait_btf(void)
{
  while(!LL_I2C_IsActiveFlag_BTF(I2C1))
  {
  }
}

void read_one(void)
{
  address_read();
  LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_NACK);
  LL_I2C_ClearFlag_ADDR(I2C1);
  LL_I2C_GenerateStopCondition(I2C1);
  while(!LL_I2C_IsActiveFlag_RXNE(I2C1))
  {
  }
  LOG(LL_I2C_ReceiveData8(I2C1));
  stop();
  DONE();
}

void read_two(void)
{
  address_read();
  LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_NACK);
  LL_I2C_EnableBitPOS(I2C1);
  LL_I2C_ClearFlag_ADDR(I2C1);
  wait_btf();
  LL_I2C_GenerateStopCondition(I2C1);
  LOG(LL_I2C_ReceiveData8(I2C1));
  LOG(LL_I2C_ReceiveData8(I2C1));
  stop();
  LL_I2C_DisableBitPOS(I2C1);
  DONE();
}

void read_four(void)
{
  address_read();
  LL_I2C_ClearFlag_ADDR(I2C1);
  while(!LL_I2C_IsActiveFlag_RXNE(I2C1))
  {
  }
  LOG(LL_I2C_ReceiveData8(I2C1));
  wait_btf();
  LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_NACK);
  LOG(LL_I2C_ReceiveData8(I2C1));
  wait_btf();
  LL_I2C_GenerateStopCondition(I2C1);
  LOG(LL_I2C_ReceiveData8(I2C1));
  LOG(LL_I2C_ReceiveData8(I2C1));
  stop();
  DONE();
}

int main(void)
{
  write_eeprom();
  return 0;
}
