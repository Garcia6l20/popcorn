#include "i2c_target.h"

#include "i2c_family.h"
#include "periph_test.h"

void I2C1_EV_IRQHandler(void);
void idle(void);

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

int main(void)
{
  idle();
}
