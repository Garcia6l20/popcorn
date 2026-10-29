#include "i2c_target.h"

#include "i2c_family.h"
#include "periph_test.h"

volatile uint32_t i2c_target_active;

void i2c_target_event(void)
{
  if(LL_I2C_IsActiveFlag_ADDR(I2C1))
  {
    LOG(TARGET_ADDRESS
        | LL_I2C_GetAddressMatchCode(I2C1)
        | (LL_I2C_GetTransferDirection(I2C1) >> I2C_ISR_DIR_Pos));
    LL_I2C_ClearFlag_ADDR(I2C1);
  }
  if(LL_I2C_IsActiveFlag_STOP(I2C1))
  {
    LOG(TARGET_STOP);
    LL_I2C_ClearFlag_STOP(I2C1);
  }
}

void target(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_I2C1);
  LL_I2C_Disable(I2C1);
  LL_I2C_DisableOwnAddress1(I2C1);
  LL_I2C_DisableOwnAddress2(I2C1);
  LL_I2C_SetOwnAddress1(I2C1, TARGET_OWN1 << 1, LL_I2C_OWNADDRESS1_7BIT);
  LL_I2C_EnableOwnAddress1(I2C1);
  LL_I2C_SetOwnAddress2(I2C1, TARGET_OWN2 << 1, LL_I2C_OWNADDRESS2_MASK02);
  LL_I2C_EnableOwnAddress2(I2C1);
  LL_I2C_EnableGeneralCall(I2C1);
  LL_I2C_EnableIT_ADDR(I2C1);
  LL_I2C_EnableIT_STOP(I2C1);
  i2c_target_active = 1;
  NVIC_EnableIRQ(I2C1_EV_IRQn);
  LL_I2C_Enable(I2C1);
  for(;;)
  {
    __WFI();
  }
}
