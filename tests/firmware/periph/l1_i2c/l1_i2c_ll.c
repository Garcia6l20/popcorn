#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_hal.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_i2c.h"

#define PCLK1_HZ 2097152u
#define EEPROM   0x50u
#define MEMORY   0x0040u

static I2C_HandleTypeDef i2c;
static uint8_t const     pattern[] = {0xA1u, 0xB2u, 0xC3u};

void idle(void);
void write_eeprom(void);
void nack(void);
void read_one(void);
void read_two(void);
void read_four(void);
void hal_read_one(void);
void hal_read_two(void);
void hal_read_three(void);
void hal_read_none(void);
void hal_ready(void);
void SysTick_Handler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

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

static void hal_setup(void)
{
  HAL_Init();
  __HAL_RCC_I2C1_CLK_ENABLE();
  i2c.Instance             = I2C1;
  i2c.Init.ClockSpeed      = 100000u;
  i2c.Init.DutyCycle       = I2C_DUTYCYCLE_2;
  i2c.Init.OwnAddress1     = 0u;
  i2c.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
  i2c.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  i2c.Init.OwnAddress2     = 0u;
  i2c.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  i2c.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
  if(HAL_I2C_Init(&i2c) != HAL_OK)
  {
    FAIL(10);
  }
}

static void hal_mem_read(uint16_t size)
{
  uint8_t got[sizeof pattern] = {0};
  hal_setup();
  if(HAL_I2C_Mem_Write(
         &i2c, EEPROM << 1, MEMORY, I2C_MEMADD_SIZE_16BIT, (uint8_t*)pattern, size, 10u)
     != HAL_OK)
  {
    FAIL(11);
  }
  uint32_t const written = HAL_GetTick();
  if(HAL_I2C_IsDeviceReady(&i2c, EEPROM << 1, 100u, 10u) != HAL_OK)
  {
    FAIL(12);
  }
  LOG(HAL_GetTick() - written);
  if(HAL_I2C_Mem_Read(&i2c, EEPROM << 1, MEMORY, I2C_MEMADD_SIZE_16BIT, got, size, 10u) != HAL_OK)
  {
    FAIL(13);
  }
  for(uint32_t index = 0; index < size; ++index)
  {
    LOG(got[index]);
  }
  LOG(i2c.ErrorCode);
  DONE();
}

void hal_read_one(void)
{
  hal_mem_read(1u);
}

void hal_read_two(void)
{
  hal_mem_read(2u);
}

void hal_read_three(void)
{
  hal_mem_read(3u);
}

void hal_read_none(void)
{
  uint8_t got[2] = {0};
  hal_setup();
  if(HAL_I2C_Mem_Read(&i2c, EEPROM << 1, MEMORY, I2C_MEMADD_SIZE_16BIT, got, 0u, 10u) != HAL_OK)
  {
    FAIL(14);
  }
  while(I2C1->SR2 & I2C_SR2_BUSY)
  {
  }
  LOG(I2C1->SR1);
  if(HAL_I2C_Mem_Write(&i2c, EEPROM << 1, MEMORY, I2C_MEMADD_SIZE_16BIT, (uint8_t*)pattern, 2u, 10u)
     != HAL_OK)
  {
    FAIL(15);
  }
  if(HAL_I2C_IsDeviceReady(&i2c, EEPROM << 1, 100u, 10u) != HAL_OK)
  {
    FAIL(16);
  }
  if(HAL_I2C_Mem_Read(&i2c, EEPROM << 1, MEMORY, I2C_MEMADD_SIZE_16BIT, got, 2u, 10u) != HAL_OK)
  {
    FAIL(17);
  }
  LOG(got[0]);
  LOG(got[1]);
  LOG(i2c.ErrorCode);
  DONE();
}

void hal_ready(void)
{
  hal_setup();
  LOG(HAL_I2C_IsDeviceReady(&i2c, EEPROM << 1, 1u, 10u));
  uint32_t const before = HAL_GetTick();
  LOG(HAL_I2C_IsDeviceReady(&i2c, (EEPROM + 1u) << 1, 3u, 10u));
  LOG(HAL_GetTick() - before);
  LOG(I2C1->SR1);
  LOG(HAL_I2C_IsDeviceReady(&i2c, EEPROM << 1, 1u, 10u));
  LOG(I2C1->SR1);
  LOG(I2C1->SR2);
  DONE();
}

int main(void)
{
  write_eeprom();
  return 0;
}
