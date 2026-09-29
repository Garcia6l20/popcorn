#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_gpio.h"
#include "stm32u5xx_ll_spi.h"

static uint8_t const     command[4] = {0x9F, 0, 0, 0};
static uint8_t           answer[4];
static volatile uint32_t received;
static volatile uint32_t finished;

void SPI1_IRQHandler(void);
void idle(void);
void poll(void);
void irq_eot(void);
void no_cs(void);
void hw_ss(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void chip_select(uint32_t low)
{
  if(low)
  {
    LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_4);
  }
  else
  {
    LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_4);
  }
}

static void configure(uint32_t nss)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
  LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_4);
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_4, LL_GPIO_MODE_OUTPUT);
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SPI1);
  LL_SPI_InitTypeDef config = {
      .TransferDirection = LL_SPI_FULL_DUPLEX,
      .Mode              = LL_SPI_MODE_MASTER,
      .DataWidth         = LL_SPI_DATAWIDTH_8BIT,
      .ClockPolarity     = LL_SPI_POLARITY_LOW,
      .ClockPhase        = LL_SPI_PHASE_1EDGE,
      .NSS               = nss,
      .BaudRate          = LL_SPI_BAUDRATEPRESCALER_DIV2,
      .BitOrder          = LL_SPI_MSB_FIRST,
      .CRCCalculation    = LL_SPI_CRCCALCULATION_DISABLE,
      .CRCPoly           = 7,
  };
  LL_SPI_Init(SPI1, &config);
  LL_SPI_SetInternalSSLevel(SPI1, LL_SPI_SS_LEVEL_HIGH);
  LL_SPI_SetFIFOThreshold(SPI1, LL_SPI_FIFO_TH_01DATA);
  LL_SPI_SetTransferSize(SPI1, sizeof command);
}

static void finish(void)
{
  while(!LL_SPI_IsActiveFlag_EOT(SPI1))
  {
  }
  LL_SPI_ClearFlag_EOT(SPI1);
  LL_SPI_ClearFlag_TXTF(SPI1);
  LL_SPI_Disable(SPI1);
}

static void exchange_polled(void)
{
  LL_SPI_Enable(SPI1);
  LL_SPI_StartMasterTransfer(SPI1);
  for(uint32_t index = 0; index < sizeof command; ++index)
  {
    while(!LL_SPI_IsActiveFlag_TXP(SPI1))
    {
    }
    LL_SPI_TransmitData8(SPI1, command[index]);
    while(!LL_SPI_IsActiveFlag_RXP(SPI1))
    {
    }
    answer[index] = LL_SPI_ReceiveData8(SPI1);
  }
  finish();
}

static void log_answer(void)
{
  for(uint32_t index = 0; index < sizeof answer; ++index)
  {
    LOG(answer[index]);
  }
}

void poll(void)
{
  configure(LL_SPI_NSS_SOFT);
  chip_select(1);
  exchange_polled();
  chip_select(0);
  log_answer();
  DONE();
}

void no_cs(void)
{
  configure(LL_SPI_NSS_SOFT);
  exchange_polled();
  log_answer();
  DONE();
}

void hw_ss(void)
{
  configure(LL_SPI_NSS_HARD_OUTPUT);
  exchange_polled();
  log_answer();
  DONE();
}

void SPI1_IRQHandler(void)
{
  while(LL_SPI_IsActiveFlag_RXP(SPI1) && received < sizeof answer)
  {
    answer[received++] = LL_SPI_ReceiveData8(SPI1);
  }
  if(LL_SPI_IsActiveFlag_EOT(SPI1))
  {
    LL_SPI_ClearFlag_EOT(SPI1);
    LL_SPI_ClearFlag_TXTF(SPI1);
    LL_SPI_DisableIT_EOT(SPI1);
    LL_SPI_DisableIT_RXP(SPI1);
    LL_SPI_Disable(SPI1);
    finished = 1;
  }
}

void irq_eot(void)
{
  configure(LL_SPI_NSS_SOFT);
  chip_select(1);
  LL_SPI_Enable(SPI1);
  for(uint32_t index = 0; index < sizeof command; ++index)
  {
    LL_SPI_TransmitData8(SPI1, command[index]);
  }
  LL_SPI_EnableIT_RXP(SPI1);
  LL_SPI_EnableIT_EOT(SPI1);
  NVIC_EnableIRQ(SPI1_IRQn);
  LL_SPI_StartMasterTransfer(SPI1);
  while(finished == 0)
  {
    __WFI();
  }
  chip_select(0);
  log_answer();
  LOG(received);
  DONE();
}

int main(void)
{
  idle();
}
