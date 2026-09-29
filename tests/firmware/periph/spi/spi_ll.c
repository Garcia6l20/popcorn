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
void write_read(void);
void erase(void);

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

static void transfer(uint8_t const* out, uint8_t* in, uint32_t count)
{
  LL_SPI_SetTransferSize(SPI1, count);
  LL_SPI_Enable(SPI1);
  LL_SPI_StartMasterTransfer(SPI1);
  for(uint32_t index = 0; index < count; ++index)
  {
    while(!LL_SPI_IsActiveFlag_TXP(SPI1))
    {
    }
    LL_SPI_TransmitData8(SPI1, out != 0 ? out[index] : 0);
    while(!LL_SPI_IsActiveFlag_RXP(SPI1))
    {
    }
    uint8_t const value = LL_SPI_ReceiveData8(SPI1);
    if(in != 0)
    {
      in[index] = value;
    }
  }
  finish();
}

static void flash_command(uint8_t const* out, uint8_t* in, uint32_t count)
{
  chip_select(1);
  transfer(out, in, count);
  chip_select(0);
}

static uint32_t wait_ready(void)
{
  static uint8_t const read_status[2] = {0x05, 0};
  uint8_t              status[2];
  uint32_t             polls = 0;
  do
  {
    flash_command(read_status, status, sizeof status);
    ++polls;
  } while((status[1] & 1u) != 0);
  return polls;
}

static void write_enable(void)
{
  static uint8_t const enable[1] = {0x06};
  flash_command(enable, 0, sizeof enable);
}

static uint32_t read_back(uint8_t expected_base, uint32_t erased)
{
  static uint8_t const out[20] = {0x03, 0x00, 0x10, 0x00};
  static uint8_t       in[20];
  flash_command(out, in, sizeof in);
  uint32_t equal = 0;
  for(uint32_t index = 0; index < 16; ++index)
  {
    uint8_t const want = erased ? 0xFF : (uint8_t)(expected_base + index);
    equal += in[4 + index] == want ? 1u : 0u;
  }
  return equal;
}

void write_read(void)
{
  configure(LL_SPI_NSS_SOFT);
  static uint8_t out[20] = {0x02, 0x00, 0x10, 0x00};
  for(uint32_t index = 0; index < 16; ++index)
  {
    out[4 + index] = (uint8_t)(0xA0 + index);
  }
  write_enable();
  flash_command(out, 0, sizeof out);
  LOG(wait_ready());
  LOG(read_back(0xA0, 0));
  DONE();
}

void erase(void)
{
  static uint8_t const sector[4] = {0x20, 0x00, 0x10, 0x00};
  configure(LL_SPI_NSS_SOFT);
  write_enable();
  flash_command(sector, 0, sizeof sector);
  LOG(wait_ready());
  LOG(read_back(0, 1));
  DONE();
}

int main(void)
{
  idle();
}
