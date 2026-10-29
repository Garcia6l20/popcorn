#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_dma.h"
#include "stm32u5xx_ll_gpio.h"
#include "stm32u5xx_ll_spi.h"

#define LINK_MAGIC    0x5B1A11u
#define LINK_MASTER   1u
#define LINK_SLAVE    2u
#define LINK_END      0u
#define LINK_CS_LOW   1u
#define LINK_CS_HIGH  2u
#define LINK_SEND     3u
#define LINK_WAIT_US  4u
#define LINK_RESET    5u
#define LINK_HARD_NSS 6u
#define LINK_RCC      7u
#define LINK_STAMP    8u
#define LINK_REPEAT   9u
#define LINK_KICK     10u
#define LINK_FORMAT   11u
#define LINK_ABORT    12u
#define LINK_BOOT     0xB0000000u

typedef struct
{
  uint32_t magic;
  uint32_t entry;
  uint32_t pc;
  uint32_t log_len;
  uint32_t count;
  uint32_t prefill;
  uint32_t script[64];
  uint32_t log[256];
} link_area_t;

#define LINK_AREA ((volatile link_area_t*)0x20080000u)

static uint8_t const     command[4] = {0x9F, 0, 0, 0};
static uint8_t           answer[4];
static volatile uint32_t received;
static volatile uint32_t finished;
static volatile uint32_t echoing;

void SPI1_IRQHandler(void);
void idle(void);
void poll(void);
void irq_eot(void);
void no_cs(void);
void hw_ss(void);
void write_read(void);
void erase(void);
void dma(void);
void target_echo(void);
void link_run(void);
void GPDMA1_Channel2_IRQHandler(void);
void GPDMA1_Channel3_IRQHandler(void);

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

static void configure_spi(uint32_t nss);

static void configure(uint32_t nss)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
  LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_4);
  LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_4, LL_GPIO_MODE_OUTPUT);
  configure_spi(nss);
}

static void configure_spi(uint32_t nss)
{
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

static void echo_frames(void)
{
  while(LL_SPI_IsActiveFlag_RXP(SPI1) && received < sizeof answer)
  {
    uint8_t const value = LL_SPI_ReceiveData8(SPI1);
    answer[received++]  = value;
    LL_SPI_TransmitData8(SPI1, (uint8_t)(value + 0x10u));
  }
  if(received == sizeof answer)
  {
    LL_SPI_DisableIT_RXP(SPI1);
    finished = 1;
  }
}

static void link_slave_echo(void);

void SPI1_IRQHandler(void)
{
  if(LINK_AREA->magic == LINK_MAGIC && LINK_AREA->entry == LINK_SLAVE)
  {
    link_slave_echo();
    return;
  }
  if(echoing)
  {
    echo_frames();
    return;
  }
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

static uint8_t           dma_out[68] = {0x03, 0x00, 0x10, 0x00};
static uint8_t           dma_in[68];
static volatile uint32_t tx_done;
static volatile uint32_t rx_done;

void GPDMA1_Channel2_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_2))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_2);
    ++tx_done;
  }
}

void GPDMA1_Channel3_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_3))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_3);
    ++rx_done;
  }
}

static void dma_channel(
    uint32_t channel,
    uint32_t request,
    uint32_t direction,
    uint32_t increment,
    uint32_t source,
    uint32_t destination)
{
  LL_DMA_ConfigTransfer(
      GPDMA1, channel, increment | LL_DMA_SRC_DATAWIDTH_BYTE | LL_DMA_DEST_DATAWIDTH_BYTE);
  LL_DMA_SetDataTransferDirection(GPDMA1, channel, direction);
  LL_DMA_SetPeriphRequest(GPDMA1, channel, request);
  LL_DMA_SetBlkDataLength(GPDMA1, channel, sizeof dma_out);
  LL_DMA_SetSrcAddress(GPDMA1, channel, source);
  LL_DMA_SetDestAddress(GPDMA1, channel, destination);
  LL_DMA_EnableIT_TC(GPDMA1, channel);
}

void dma(void)
{
  configure(LL_SPI_NSS_SOFT);
  static uint8_t program[68] = {0x02, 0x00, 0x10, 0x00};
  for(uint32_t index = 0; index < 64; ++index)
  {
    program[4 + index] = (uint8_t)(3 * index + 1);
  }
  write_enable();
  flash_command(program, 0, sizeof program);
  wait_ready();

  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  dma_channel(
      LL_DMA_CHANNEL_2, LL_GPDMA1_REQUEST_SPI1_TX, LL_DMA_DIRECTION_MEMORY_TO_PERIPH,
      LL_DMA_SRC_INCREMENT | LL_DMA_DEST_FIXED, (uint32_t)dma_out, (uint32_t)&SPI1->TXDR);
  dma_channel(
      LL_DMA_CHANNEL_3, LL_GPDMA1_REQUEST_SPI1_RX, LL_DMA_DIRECTION_PERIPH_TO_MEMORY,
      LL_DMA_SRC_FIXED | LL_DMA_DEST_INCREMENT, (uint32_t)&SPI1->RXDR, (uint32_t)dma_in);
  NVIC_EnableIRQ(GPDMA1_Channel2_IRQn);
  NVIC_EnableIRQ(GPDMA1_Channel3_IRQn);
  LL_SPI_SetTransferSize(SPI1, sizeof dma_out);
  LL_SPI_EnableDMAReq_RX(SPI1);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_3);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_2);
  LL_SPI_EnableDMAReq_TX(SPI1);
  chip_select(1);
  LL_SPI_Enable(SPI1);
  LL_SPI_StartMasterTransfer(SPI1);
  while(tx_done == 0 || rx_done == 0)
  {
  }
  finish();
  chip_select(0);
  LL_SPI_DisableDMAReq_TX(SPI1);
  LL_SPI_DisableDMAReq_RX(SPI1);
  uint32_t equal = 0;
  for(uint32_t index = 0; index < 64; ++index)
  {
    equal += dma_in[4 + index] == (uint8_t)(3 * index + 1) ? 1u : 0u;
  }
  LOG(equal);
  LOG(tx_done);
  LOG(rx_done);
  DONE();
}

void target_echo(void)
{
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SPI1);
  LL_SPI_InitTypeDef config = {
      .TransferDirection = LL_SPI_FULL_DUPLEX,
      .Mode              = LL_SPI_MODE_SLAVE,
      .DataWidth         = LL_SPI_DATAWIDTH_8BIT,
      .ClockPolarity     = LL_SPI_POLARITY_LOW,
      .ClockPhase        = LL_SPI_PHASE_1EDGE,
      .NSS               = LL_SPI_NSS_HARD_INPUT,
      .BaudRate          = LL_SPI_BAUDRATEPRESCALER_DIV2,
      .BitOrder          = LL_SPI_MSB_FIRST,
      .CRCCalculation    = LL_SPI_CRCCALCULATION_DISABLE,
      .CRCPoly           = 7,
  };
  LL_SPI_Init(SPI1, &config);
  LL_SPI_SetFIFOThreshold(SPI1, LL_SPI_FIFO_TH_01DATA);
  LL_SPI_SetTransferSize(SPI1, 0);
  LL_SPI_SetUDRConfiguration(SPI1, LL_SPI_UDR_CONFIG_REGISTER_PATTERN);
  SPI1->UDRDR = 0xEEu;
  echoing     = 1;
  LL_SPI_Enable(SPI1);
  LL_SPI_TransmitData8(SPI1, 0xA0u);
  LL_SPI_TransmitData8(SPI1, 0xA1u);
  LL_SPI_EnableIT_RXP(SPI1);
  NVIC_EnableIRQ(SPI1_IRQn);
  while(finished == 0)
  {
    __WFI();
  }
  log_answer();
  LOG(LL_SPI_IsActiveFlag_UDR(SPI1));
  LL_SPI_Disable(SPI1);
  DONE();
}

static uint32_t link_nss;
static uint32_t link_bits;

static void link_log(uint32_t value)
{
  LINK_AREA->log[LINK_AREA->log_len++ & 0xffu] = value;
}

static void link_wait_us(uint32_t us)
{
  uint32_t const from = DWT->CYCCNT;
  while(DWT->CYCCNT - from < us * 4u)
  {
  }
}

static void link_master_configure(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
  configure_spi(link_nss ? LL_SPI_NSS_HARD_OUTPUT : LL_SPI_NSS_SOFT);
  link_bits = 8u;
}

static void link_format(uint32_t bits, uint32_t prescaler)
{
  MODIFY_REG(
      SPI1->CFG1, SPI_CFG1_DSIZE | SPI_CFG1_MBR,
      ((bits - 1u) << SPI_CFG1_DSIZE_Pos) | (prescaler << SPI_CFG1_MBR_Pos));
  link_bits = bits;
}

static void link_drive(uint32_t pin, uint32_t high)
{
  if(high)
  {
    LL_GPIO_SetOutputPin(GPIOA, 1u << pin);
  }
  else
  {
    LL_GPIO_ResetOutputPin(GPIOA, 1u << pin);
  }
  LL_GPIO_SetPinMode(GPIOA, 1u << pin, LL_GPIO_MODE_OUTPUT);
}

static void link_kick(uint32_t first)
{
  LL_SPI_SetTransferSize(SPI1, 1);
  LL_SPI_Enable(SPI1);
  LL_SPI_StartMasterTransfer(SPI1);
  if(link_bits > 16u)
  {
    LL_SPI_TransmitData32(SPI1, first);
  }
  else if(link_bits > 8u)
  {
    LL_SPI_TransmitData16(SPI1, (uint16_t)first);
  }
  else
  {
    LL_SPI_TransmitData8(SPI1, (uint8_t)first);
  }
}

static void link_slave_configure(void)
{
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SPI1);
  LL_SPI_InitTypeDef config = {
      .TransferDirection = LL_SPI_FULL_DUPLEX,
      .Mode              = LL_SPI_MODE_SLAVE,
      .DataWidth         = LL_SPI_DATAWIDTH_8BIT,
      .ClockPolarity     = LL_SPI_POLARITY_LOW,
      .ClockPhase        = LL_SPI_PHASE_1EDGE,
      .NSS               = LL_SPI_NSS_HARD_INPUT,
      .BaudRate          = LL_SPI_BAUDRATEPRESCALER_DIV2,
      .BitOrder          = LL_SPI_MSB_FIRST,
      .CRCCalculation    = LL_SPI_CRCCALCULATION_DISABLE,
      .CRCPoly           = 7,
  };
  LL_SPI_Init(SPI1, &config);
  LL_SPI_SetFIFOThreshold(SPI1, LL_SPI_FIFO_TH_01DATA);
  LL_SPI_SetTransferSize(SPI1, 0);
  LL_SPI_SetUDRConfiguration(SPI1, LL_SPI_UDR_CONFIG_REGISTER_PATTERN);
  SPI1->UDRDR = 0xEEu;
  LL_SPI_Enable(SPI1);
  LL_SPI_TransmitData8(SPI1, LINK_AREA->prefill != 0 ? (uint8_t)LINK_AREA->prefill : 0xA0u);
  LL_SPI_EnableIT_RXP(SPI1);
  NVIC_EnableIRQ(SPI1_IRQn);
}

static void link_send(uint32_t count, uint32_t first)
{
  LL_SPI_SetTransferSize(SPI1, count);
  LL_SPI_Enable(SPI1);
  LL_SPI_StartMasterTransfer(SPI1);
  for(uint32_t index = 0; index < count; ++index)
  {
    while(!LL_SPI_IsActiveFlag_TXP(SPI1))
    {
    }
    LL_SPI_TransmitData8(SPI1, (uint8_t)(first + index));
    while(!LL_SPI_IsActiveFlag_RXP(SPI1))
    {
    }
    link_log(LL_SPI_ReceiveData8(SPI1));
  }
  finish();
}

static void link_rcc(void)
{
  LL_APB2_GRP1_ForceReset(LL_APB2_GRP1_PERIPH_SPI1);
  LL_APB2_GRP1_ReleaseReset(LL_APB2_GRP1_PERIPH_SPI1);
  if(LINK_AREA->entry == LINK_MASTER)
  {
    link_master_configure();
  }
  else
  {
    link_slave_configure();
  }
}

void link_run(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  link_log(LINK_BOOT | LINK_AREA->pc);
  link_nss = 0;
  for(uint32_t at = 0; at < LINK_AREA->pc; ++at)
  {
    if(LINK_AREA->script[at] >> 24 == LINK_HARD_NSS)
    {
      link_nss = LINK_AREA->script[at] & 1u;
    }
  }
  if(LINK_AREA->entry == LINK_MASTER)
  {
    link_master_configure();
  }
  else
  {
    link_slave_configure();
  }
  for(;;)
  {
    uint32_t const step = LINK_AREA->script[LINK_AREA->pc++ & 0x3fu];
    uint32_t const arg  = step & 0xffffffu;
    switch(step >> 24)
    {
      case LINK_CS_LOW:
        link_drive(arg, 0);
        break;
      case LINK_CS_HIGH:
        link_drive(arg, 1);
        break;
      case LINK_STAMP:
        link_log(0x5A000000u | (DWT->CYCCNT & 0xffffffu));
        break;
      case LINK_REPEAT:
        if(++LINK_AREA->count < (arg >> 8))
        {
          LINK_AREA->pc = arg & 0xffu;
        }
        break;
      case LINK_KICK:
        link_kick(arg);
        break;
      case LINK_SEND:
        link_send(arg >> 8, arg & 0xffu);
        break;
      case LINK_WAIT_US:
        link_wait_us(arg);
        break;
      case LINK_RESET:
        NVIC_SystemReset();
        break;
      case LINK_HARD_NSS:
        link_nss = arg & 1u;
        link_master_configure();
        break;
      case LINK_RCC:
        link_rcc();
        break;
      case LINK_FORMAT:
        link_format(arg >> 8, arg & 0x7u);
        break;
      case LINK_ABORT:
        LL_SPI_Disable(SPI1);
        LL_SPI_Enable(SPI1);
        break;
      default:
        LINK_AREA->pc--;
        idle();
    }
  }
}

static void link_slave_echo(void)
{
  while(LL_SPI_IsActiveFlag_RXP(SPI1))
  {
    uint8_t const value = LL_SPI_ReceiveData8(SPI1);
    link_log(0x5100u | value);
    LL_SPI_TransmitData8(SPI1, (uint8_t)(value + 0x10u));
  }
}

int main(void)
{
  if(LINK_AREA->magic == LINK_MAGIC)
  {
    link_run();
  }
  idle();
}
