#include "periph_test.h"
#include "stm32l4xx_hal.h"

#define EEPROM_ADDRESS (0x50u << 1)

static I2C_HandleTypeDef i2c;

void idle(void);
void SysTick_Handler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

int main(void)
{
  HAL_Init();
  __HAL_RCC_I2C1_CLK_ENABLE();
  i2c.Instance              = I2C1;
  i2c.Init.Timing           = 0x00100305u;
  i2c.Init.OwnAddress1      = 0;
  i2c.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
  i2c.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
  i2c.Init.OwnAddress2      = 0;
  i2c.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  i2c.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
  i2c.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
  if(HAL_I2C_Init(&i2c) != HAL_OK)
  {
    FAIL(1);
  }
  LOG(HAL_I2C_IsDeviceReady(&i2c, EEPROM_ADDRESS, 1u, 10u));
  LOG(HAL_I2C_IsDeviceReady(&i2c, 0x2Au << 1, 1u, 10u));
  static uint8_t written[4] = {0xC0u, 0xC1u, 0xC2u, 0xC3u};
  static uint8_t read[4];
  if(HAL_I2C_Mem_Write(&i2c, EEPROM_ADDRESS, 0x0040u, I2C_MEMADD_SIZE_16BIT, written, 4u, 100u)
     != HAL_OK)
  {
    FAIL(2);
  }
  if(HAL_I2C_IsDeviceReady(&i2c, EEPROM_ADDRESS, 1000u, 100u) != HAL_OK)
  {
    FAIL(3);
  }
  if(HAL_I2C_Mem_Read(&i2c, EEPROM_ADDRESS, 0x0040u, I2C_MEMADD_SIZE_16BIT, read, 4u, 100u)
     != HAL_OK)
  {
    FAIL(4);
  }
  LOG(read[0]);
  LOG(read[3]);
  DONE();
}
