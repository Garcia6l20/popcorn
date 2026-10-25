#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_dma.h"
#include "stm32u5xx_ll_iwdg.h"
#include "stm32u5xx_ll_lpuart.h"
#include "stm32u5xx_ll_pwr.h"
#include "stm32u5xx_ll_rcc.h"
#include "stm32u5xx_ll_usart.h"

static char const hello[] = "hello\r\n";

uint8_t                  received[16];
static volatile uint32_t received_count;
static volatile uint32_t tx_next;
static volatile uint32_t tx_refills;

void USART1_IRQHandler(void);
void tx_poll(void);
void tx_irq(void);
void rx_irq(void);
void loop_fifo(void);
void overrun(void);
void uart4_poll(void);
void lpuart_poll(void);
void idle(void);
void brr_while_enabled(void);
void tx_fifo_irq(void);
void tx_dma(void);
void GPDMA1_Channel0_IRQHandler(void);
void rx_dma_list(void);
void GPDMA1_Channel3_IRQHandler(void);
void link_tx(void);
void link_rx(void);
void link_abort(void);
void link_rx_slow(void);
void link_rx_2stop(void);
void link_rx_at(void);
void link_tx_pattern(void);
void link_tx_switch(void);
void link_tx_55(void);
void link_rx_over8(void);
void link_rx_onebit(void);
void break_tx(void);
void break_fifo(void);
void break_gap(void);
void link_tx_long(void);
void link_rx_quiet(void);
void link_tx_bursts(void);
void dma_ring(void);
void link_ping(void);
void link_echo(void);
void link_tx_count(void);
void link_abort_gap(void);
void link_tx_reset(void);
void link_abort_b2b(void);
void link_tx_mid_switch(void);
void link_tx_fifo_stop(void);
void link_rx_dma(void);
void link_iwdg(void);
void link_audio(void);
void link_ue_toggle(void);
void link_audio_peer(void);
void link_audio_tim6(void);
void link_self(void);
void USART3_IRQHandler(void);
void USART2_IRQHandler(void);
void SysTick_Handler(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void init(USART_TypeDef* usart, uint32_t baud)
{
  LL_USART_InitTypeDef config = {
      .PrescalerValue      = LL_USART_PRESCALER_DIV1,
      .BaudRate            = baud,
      .DataWidth           = LL_USART_DATAWIDTH_8B,
      .StopBits            = LL_USART_STOPBITS_1,
      .Parity              = LL_USART_PARITY_NONE,
      .TransferDirection   = LL_USART_DIRECTION_TX_RX,
      .HardwareFlowControl = LL_USART_HWCONTROL_NONE,
      .OverSampling        = LL_USART_OVERSAMPLING_16,
  };
  LL_USART_Init(usart, &config);
  LL_USART_ConfigAsyncMode(usart);
}

static void start_usart1(void)
{
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
  init(USART1, 115200);
}

static void enable(USART_TypeDef* usart)
{
  LL_USART_Enable(usart);
  while(!LL_USART_IsActiveFlag_TEACK(usart) || !LL_USART_IsActiveFlag_REACK(usart))
  {
  }
}

static void send_poll(USART_TypeDef* usart)
{
  for(char const* next = hello; *next != 0; ++next)
  {
    while(!LL_USART_IsActiveFlag_TXE_TXFNF(usart))
    {
    }
    LL_USART_TransmitData8(usart, (uint8_t)*next);
  }
  while(!LL_USART_IsActiveFlag_TC(usart))
  {
  }
}

static void link_irq(void);
static void quiet_irq(void);
static void echo_irq(void);
static void audio_irq(uint32_t port);

static volatile uint32_t audio_on;

void USART1_IRQHandler(void)
{
  if(audio_on != 0u)
  {
    audio_irq(0);
    return;
  }
  if(received_count == 0xffffffffu)
  {
    link_irq();
    return;
  }
  if(received_count == 0xfffffffeu)
  {
    quiet_irq();
    return;
  }
  if(received_count == 0xfffffffdu)
  {
    echo_irq();
    return;
  }
  if(LL_USART_IsActiveFlag_RXNE_RXFNE(USART1))
  {
    uint8_t const data = LL_USART_ReceiveData8(USART1);
    if(received_count < sizeof received)
    {
      received[received_count++] = data;
    }
  }
  if(LL_USART_IsEnabledIT_TXFT(USART1) && LL_USART_IsActiveFlag_TXFT(USART1))
  {
    ++tx_refills;
    while(tx_next < 16 && LL_USART_IsActiveFlag_TXE_TXFNF(USART1))
    {
      LL_USART_TransmitData8(USART1, (uint8_t)(0x30u + tx_next++));
    }
    if(tx_next == 16)
    {
      LL_USART_DisableIT_TXFT(USART1);
    }
  }
  if(LL_USART_IsEnabledIT_TXE_TXFNF(USART1) && LL_USART_IsActiveFlag_TXE_TXFNF(USART1))
  {
    ++tx_refills;
    if(hello[tx_next] != 0)
    {
      LL_USART_TransmitData8(USART1, (uint8_t)hello[tx_next++]);
    }
    else
    {
      LL_USART_DisableIT_TXE_TXFNF(USART1);
    }
  }
}

void tx_poll(void)
{
  start_usart1();
  enable(USART1);
  send_poll(USART1);
  LOG(LL_USART_IsActiveFlag_TC(USART1));
  DONE();
}

void tx_irq(void)
{
  start_usart1();
  enable(USART1);
  NVIC_EnableIRQ(USART1_IRQn);
  LL_USART_EnableIT_TXE_TXFNF(USART1);
  while(LL_USART_IsEnabledIT_TXE_TXFNF(USART1))
  {
  }
  while(!LL_USART_IsActiveFlag_TC(USART1))
  {
  }
  LOG(tx_refills);
  DONE();
}

void rx_irq(void)
{
  start_usart1();
  enable(USART1);
  NVIC_EnableIRQ(USART1_IRQn);
  LL_USART_EnableIT_RXNE_RXFNE(USART1);
  __asm__ volatile("bkpt #3" ::: "memory");
  while(received_count < 5)
  {
  }
  for(uint32_t index = 0; index < 5; ++index)
  {
    LOG(received[index]);
  }
  DONE();
}

void loop_fifo(void)
{
  start_usart1();
  LL_USART_EnableFIFO(USART1);
  enable(USART1);
  uint32_t sent  = 0;
  uint32_t count = 0;
  while(count < 16)
  {
    if(sent < 16 && LL_USART_IsActiveFlag_TXE_TXFNF(USART1))
    {
      LL_USART_TransmitData8(USART1, (uint8_t)(0x40u + sent++));
    }
    if(LL_USART_IsActiveFlag_RXNE_RXFNE(USART1))
    {
      received[count++] = LL_USART_ReceiveData8(USART1);
    }
  }
  uint32_t same = 1;
  for(uint32_t index = 0; index < 16; ++index)
  {
    same = same && received[index] == 0x40u + index;
  }
  LOG(count);
  LOG(same);
  DONE();
}

void overrun(void)
{
  start_usart1();
  enable(USART1);
  __asm__ volatile("bkpt #3" ::: "memory");
  while(!LL_USART_IsActiveFlag_ORE(USART1))
  {
  }
  LOG(LL_USART_IsActiveFlag_ORE(USART1));
  LL_USART_ClearFlag_ORE(USART1);
  LOG(LL_USART_IsActiveFlag_ORE(USART1));
  DONE();
}

void uart4_poll(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_UART4);
  LL_RCC_SetUARTClockSource(LL_RCC_UART4_CLKSOURCE_PCLK1);
  init(UART4, 9600);
  enable(UART4);
  send_poll(UART4);
  DONE();
}

void lpuart_poll(void)
{
  LL_AHB3_GRP1_EnableClock(LL_AHB3_GRP1_PERIPH_PWR);
  LL_PWR_EnableBkUpAccess();
  LL_RCC_LSE_Enable();
  while(!LL_RCC_LSE_IsReady())
  {
  }
  LL_APB3_GRP1_EnableClock(LL_APB3_GRP1_PERIPH_LPUART1);
  LL_RCC_SetLPUARTClockSource(LL_RCC_LPUART1_CLKSOURCE_LSE);
  LL_LPUART_InitTypeDef config = {
      .PrescalerValue      = LL_LPUART_PRESCALER_DIV1,
      .BaudRate            = 9600,
      .DataWidth           = LL_LPUART_DATAWIDTH_8B,
      .StopBits            = LL_LPUART_STOPBITS_1,
      .Parity              = LL_LPUART_PARITY_NONE,
      .TransferDirection   = LL_LPUART_DIRECTION_TX_RX,
      .HardwareFlowControl = LL_LPUART_HWCONTROL_NONE,
  };
  LL_LPUART_Init(LPUART1, &config);
  LL_LPUART_Enable(LPUART1);
  for(char const* next = hello; *next != 0; ++next)
  {
    while(!LL_LPUART_IsActiveFlag_TXE_TXFNF(LPUART1))
    {
    }
    LL_LPUART_TransmitData8(LPUART1, (uint8_t)*next);
  }
  while(!LL_LPUART_IsActiveFlag_TC(LPUART1))
  {
  }
  DONE();
}

void tx_fifo_irq(void)
{
  start_usart1();
  LL_USART_EnableFIFO(USART1);
  LL_USART_SetTXFIFOThreshold(USART1, LL_USART_FIFOTHRESHOLD_1_8);
  enable(USART1);
  NVIC_EnableIRQ(USART1_IRQn);
  LL_USART_EnableIT_TXFT(USART1);
  while(LL_USART_IsEnabledIT_TXFT(USART1))
  {
  }
  while(!LL_USART_IsActiveFlag_TC(USART1))
  {
  }
  LOG(tx_refills);
  DONE();
}

static char const banner[65] = "popcorn usart dma banner, sixty-four bytes long, sent by gpdma!\n";
static volatile uint32_t dma_done;

void GPDMA1_Channel0_IRQHandler(void)
{
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_0))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_0);
    ++dma_done;
  }
}

void tx_dma(void)
{
  start_usart1();
  enable(USART1);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  LL_DMA_ConfigTransfer(
      GPDMA1, LL_DMA_CHANNEL_0,
      LL_DMA_SRC_INCREMENT
          | LL_DMA_DEST_FIXED
          | LL_DMA_SRC_DATAWIDTH_BYTE
          | LL_DMA_DEST_DATAWIDTH_BYTE);
  LL_DMA_SetDataTransferDirection(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
  LL_DMA_SetPeriphRequest(GPDMA1, LL_DMA_CHANNEL_0, LL_GPDMA1_REQUEST_USART1_TX);
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_0, 64);
  LL_DMA_SetSrcAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)banner);
  LL_DMA_SetDestAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)&USART1->TDR);
  LL_DMA_EnableIT_TC(GPDMA1, LL_DMA_CHANNEL_0);
  NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
  LL_USART_ClearFlag_TC(USART1);
  LL_USART_EnableDMAReq_TX(USART1);
  while(dma_done == 0)
  {
  }
  while(!LL_USART_IsActiveFlag_TC(USART1))
  {
  }
  LOG(dma_done);
  DONE();
}

#define LIST_UPDATE \
  (LL_DMA_UPDATE_CBR1 | LL_DMA_UPDATE_CSAR | LL_DMA_UPDATE_CDAR | LL_DMA_UPDATE_CLLR)
#define RING_BYTES 16u

uint8_t                  ring[RING_BYTES];
static uint32_t          rx_node0[6] __attribute__((aligned(4)));
static uint32_t          rx_node1[4] __attribute__((aligned(4)));
static volatile uint32_t rx_events;
static volatile uint32_t ring_size = RING_BYTES;
uint8_t                  big_ring[64];

void GPDMA1_Channel3_IRQHandler(void)
{
  uint32_t const moved = ring_size - LL_DMA_GetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_3);
  if(LL_DMA_IsActiveFlag_HT(GPDMA1, LL_DMA_CHANNEL_3))
  {
    LL_DMA_ClearFlag_HT(GPDMA1, LL_DMA_CHANNEL_3);
    LOG(0x48000000u | moved);
    ++rx_events;
  }
  if(LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_3))
  {
    LL_DMA_ClearFlag_TC(GPDMA1, LL_DMA_CHANNEL_3);
    LOG(0x54000000u | moved);
    ++rx_events;
  }
}

static void start_ring(uint8_t* target, uint32_t bytes)
{
  ring_size = bytes;
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART2_CLKSOURCE_PCLK1);
  init(USART2, 115200);
  enable(USART2);
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  uint32_t const source = (uint32_t)&USART2->RDR;
  uint32_t const next   = LIST_UPDATE | ((uint32_t)rx_node1 & 0xfffcu);
  rx_node0[0]           = LL_DMA_SRC_FIXED | LL_DMA_DEST_INCREMENT;
  rx_node0[1]           = LL_DMA_TCEM_EACH_LLITEM_TRANSFER
                        | LL_DMA_DIRECTION_PERIPH_TO_MEMORY
                        | LL_GPDMA1_REQUEST_USART2_RX;
  rx_node0[2]           = bytes;
  rx_node0[3]           = source;
  rx_node0[4]           = (uint32_t)target;
  rx_node0[5]           = next;
  rx_node1[0]           = bytes;
  rx_node1[1]           = source;
  rx_node1[2]           = (uint32_t)target;
  rx_node1[3]           = next;
  LL_DMA_SetTransferEventMode(GPDMA1, LL_DMA_CHANNEL_3, LL_DMA_TCEM_RPT_BLK_TRANSFER);
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_3, 0);
  LL_DMA_SetLinkedListBaseAddr(GPDMA1, LL_DMA_CHANNEL_3, (uint32_t)rx_node0 & 0xffff0000u);
  LL_DMA_ConfigLinkUpdate(
      GPDMA1, LL_DMA_CHANNEL_3, LL_DMA_UPDATE_CTR1 | LL_DMA_UPDATE_CTR2 | LIST_UPDATE,
      (uint32_t)rx_node0);
  LL_DMA_EnableIT_HT(GPDMA1, LL_DMA_CHANNEL_3);
  LL_DMA_EnableIT_TC(GPDMA1, LL_DMA_CHANNEL_3);
  NVIC_EnableIRQ(GPDMA1_Channel3_IRQn);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_3);
  LL_USART_EnableDMAReq_RX(USART2);
}

void rx_dma_list(void)
{
  start_ring(ring, RING_BYTES);
  __asm__ volatile("bkpt #3" ::: "memory");
  while(rx_events < 5)
  {
  }
  DONE();
}

static void send_text(USART_TypeDef* usart, char const* text)
{
  for(char const* next = text; *next != 0; ++next)
  {
    while(!LL_USART_IsActiveFlag_TXE_TXFNF(usart))
    {
    }
    LL_USART_TransmitData8(usart, (uint8_t)*next);
  }
  while(!LL_USART_IsActiveFlag_TC(usart))
  {
  }
}

void brr_while_enabled(void)
{
  start_usart1();
  enable(USART1);
  send_text(USART1, "A");
  USART1->BRR = 4000000u / 9600u;
  send_text(USART1, "BC");
  LOG(USART1->BRR);
  DONE();
}

int main(void)
{
  tx_poll();
}

#define LINK_BAUD    460800u
#define LINK_BYTE    1u
#define LINK_IDLE    2u
#define LINK_OVERRUN 3u
#define LINK_ERROR   4u

volatile uint32_t link_baud = 460800u;

static void clock_160mhz(void)
{
  LL_AHB3_GRP1_EnableClock(LL_AHB3_GRP1_PERIPH_PWR);
  LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);
  while(!LL_PWR_IsActiveFlag_VOS())
  {
  }
  LL_PWR_EnableEPODBooster();
  while(!LL_PWR_IsActiveFlag_BOOST())
  {
  }
  FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | 4u;
  LL_RCC_MSIS_Enable();
  while(!LL_RCC_MSIS_IsReady())
  {
  }
  LL_RCC_PLL1_ConfigDomain_SYS(LL_RCC_PLL1SOURCE_MSIS, 1, 80, 2);
  LL_RCC_PLL1_SetVCOInputRange(LL_RCC_PLLINPUTRANGE_4_8);
  LL_RCC_PLL1_EnableDomain_SYS();
  LL_RCC_PLL1_Enable();
  while(!LL_RCC_PLL1_IsReady())
  {
  }
  LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
  LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
  LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_1);
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL1);
  while(LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL1)
  {
  }
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void start_link_at(uint32_t baud)
{
  clock_160mhz();
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
  init(USART1, baud);
  enable(USART1);
}

static void start_link(void)
{
  start_link_at(LINK_BAUD);
}

uint32_t wait_ticks(uint32_t us);

__attribute__((noipa)) uint32_t wait_ticks(uint32_t us)
{
  return us * 160u;
}

static void wait_us(uint32_t us)
{
  uint32_t const from  = DWT->CYCCNT;
  uint32_t const ticks = wait_ticks(us);
  while(DWT->CYCCNT - from < ticks)
  {
  }
}

static void send_byte(uint8_t data)
{
  while(!LL_USART_IsActiveFlag_TXE_TXFNF(USART1))
  {
  }
  LL_USART_TransmitData8(USART1, data);
}

static void wait_complete(void)
{
  while(!LL_USART_IsActiveFlag_TC(USART1))
  {
  }
}

void link_tx(void)
{
  static uint8_t const  runs[] = {20, 10, 10, 24};
  static uint16_t const gaps[] = {40, 10, 200, 0};
  start_link();
  uint8_t next = 0x20;
  for(unsigned run = 0; run < sizeof runs; ++run)
  {
    for(unsigned index = 0; index < runs[run]; ++index)
    {
      send_byte(next++);
    }
    wait_complete();
    wait_us(gaps[run]);
  }
  DONE();
}

void link_abort(void)
{
  start_link();
  send_byte('a');
  wait_complete();
  wait_us(60);
  send_byte('b');
  wait_us(10);
  LL_USART_Disable(USART1);
  enable(USART1);
  send_byte('c');
  wait_complete();
  DONE();
}

static void link_irq(void)
{
  uint32_t const errors =
      (LL_USART_IsActiveFlag_FE(USART1) ? 1u : 0u) | (LL_USART_IsActiveFlag_NE(USART1) ? 2u : 0u);
  if(errors != 0u)
  {
    LL_USART_ClearFlag_FE(USART1);
    LL_USART_ClearFlag_NE(USART1);
    LOG((LINK_ERROR << 16) | errors);
    LOG(DWT->CYCCNT);
  }
  if(LL_USART_IsActiveFlag_ORE(USART1))
  {
    LL_USART_ClearFlag_ORE(USART1);
    LOG(LINK_OVERRUN << 16);
    LOG(DWT->CYCCNT);
  }
  if(LL_USART_IsActiveFlag_RXNE_RXFNE(USART1))
  {
    LOG((LINK_BYTE << 16) | LL_USART_ReceiveData8(USART1));
    LOG(DWT->CYCCNT);
  }
  if(LL_USART_IsActiveFlag_IDLE(USART1))
  {
    LL_USART_ClearFlag_IDLE(USART1);
    LOG(LINK_IDLE << 16);
    LOG(DWT->CYCCNT);
  }
}

static void receive_link(void)
{
  received_count = 0xffffffffu;
  LL_USART_EnableIT_RXNE_RXFNE(USART1);
  LL_USART_EnableIT_IDLE(USART1);
  LL_USART_EnableIT_ERROR(USART1);
  NVIC_EnableIRQ(USART1_IRQn);
  idle();
}

void link_rx(void)
{
  start_link();
  receive_link();
}

void link_rx_slow(void)
{
  start_link_at(115200u);
  receive_link();
}

void link_rx_2stop(void)
{
  start_link();
  LL_USART_Disable(USART1);
  LL_USART_SetStopBitsLength(USART1, LL_USART_STOPBITS_2);
  enable(USART1);
  receive_link();
}

void link_rx_at(void)
{
  start_link_at(link_baud);
  receive_link();
}

void link_tx_pattern(void)
{
  start_link();
  for(unsigned index = 0; index < 8; ++index)
  {
    send_byte(0x55);
    send_byte(0xf0);
  }
  wait_complete();
  wait_us(200);
  send_byte(0x55);
  wait_complete();
  DONE();
}

void link_tx_switch(void)
{
  start_link();
  for(unsigned index = 0; index < 4; ++index)
  {
    send_byte(0x55);
  }
  wait_complete();
  wait_us(100);
  LL_USART_Disable(USART1);
  init(USART1, 115200u);
  enable(USART1);
  for(unsigned index = 0; index < 4; ++index)
  {
    send_byte(0x55);
  }
  wait_complete();
  DONE();
}

void break_tx(void)
{
  start_link_at(115200u);
  send_byte('a');
  LL_USART_RequestBreakSending(USART1);
  send_byte('b');
  wait_complete();
  DONE();
}

void break_gap(void)
{
  start_link_at(115200u);
  send_byte('a');
  wait_complete();
  wait_us(40);
  LL_USART_RequestBreakSending(USART1);
  while(LL_USART_IsActiveFlag_SBK(USART1))
  {
  }
  wait_us(60);
  send_byte('b');
  wait_complete();
  DONE();
}

void break_fifo(void)
{
  start_link_at(115200u);
  LL_USART_Disable(USART1);
  LL_USART_EnableFIFO(USART1);
  enable(USART1);
  for(uint8_t next = '0'; next < '8'; ++next)
  {
    send_byte(next);
  }
  LL_USART_RequestBreakSending(USART1);
  wait_complete();
  DONE();
}

void link_tx_long(void)
{
  start_link();
  for(unsigned index = 0; index < 512; ++index)
  {
    send_byte((uint8_t)index);
  }
  wait_complete();
  DONE();
}

static volatile uint32_t quiet_bytes;

static void quiet_irq(void)
{
  if(LL_USART_IsActiveFlag_RXNE_RXFNE(USART1))
  {
    (void)LL_USART_ReceiveData8(USART1);
    ++quiet_bytes;
  }
  if(LL_USART_IsActiveFlag_IDLE(USART1))
  {
    LL_USART_ClearFlag_IDLE(USART1);
    LOG((LINK_IDLE << 16) | (quiet_bytes & 0xffffu));
    LOG(DWT->CYCCNT);
  }
}

void link_rx_quiet(void)
{
  start_link();
  received_count = 0xfffffffeu;
  LL_USART_EnableIT_RXNE_RXFNE(USART1);
  LL_USART_EnableIT_IDLE(USART1);
  NVIC_EnableIRQ(USART1_IRQn);
  idle();
}

void link_tx_bursts(void)
{
  start_usart1();
  enable(USART1);
  uint8_t  next = 0;
  unsigned sent = 0;
  while(sent < 200)
  {
    for(unsigned index = 0; index < 37 && sent < 200; ++index, ++sent)
    {
      send_byte(next++);
    }
    wait_complete();
    for(volatile uint32_t pause = 0; pause < 200; ++pause)
    {
    }
  }
  DONE();
}

void USART2_IRQHandler(void)
{
  if(audio_on != 0u)
  {
    audio_irq(1);
    return;
  }
  if(LL_USART_IsActiveFlag_IDLE(USART2))
  {
    LL_USART_ClearFlag_IDLE(USART2);
    LOG(0x49000000u | (ring_size - LL_DMA_GetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_3)));
  }
}

void dma_ring(void)
{
  start_ring(big_ring, sizeof big_ring);
  LL_USART_EnableIT_IDLE(USART2);
  NVIC_EnableIRQ(USART2_IRQn);
  __asm__ volatile("bkpt #3" ::: "memory");
  idle();
}

void link_tx_55(void)
{
  start_link();
  for(unsigned index = 0; index < 48; ++index)
  {
    send_byte(0x55);
  }
  wait_complete();
  DONE();
}

void link_rx_over8(void)
{
  start_link_at(link_baud);
  LL_USART_Disable(USART1);
  LL_USART_SetOverSampling(USART1, LL_USART_OVERSAMPLING_8);
  LL_USART_SetBaudRate(
      USART1, 160000000u, LL_USART_PRESCALER_DIV1, LL_USART_OVERSAMPLING_8, link_baud);
  enable(USART1);
  receive_link();
}

void link_rx_onebit(void)
{
  start_link_at(link_baud);
  LL_USART_Disable(USART1);
  LL_USART_EnableOneBitSamp(USART1);
  enable(USART1);
  receive_link();
}

void link_ping(void)
{
  start_link();
  received_count = 0xffffffffu;
  LL_USART_EnableIT_RXNE_RXFNE(USART1);
  LL_USART_EnableIT_ERROR(USART1);
  NVIC_EnableIRQ(USART1_IRQn);
  uint32_t volatile const* const logged = &log_len;
  for(unsigned index = 0; index < 16; ++index)
  {
    uint32_t const before = *logged;
    send_byte((uint8_t)(0x40u + index));
    while(*logged == before)
    {
    }
    wait_us(30);
  }
  DONE();
}

static void echo_irq(void)
{
  if(LL_USART_IsActiveFlag_RXNE_RXFNE(USART1))
  {
    LL_USART_TransmitData8(USART1, LL_USART_ReceiveData8(USART1));
  }
}

void link_echo(void)
{
  start_link();
  received_count = 0xfffffffdu;
  LL_USART_EnableIT_RXNE_RXFNE(USART1);
  NVIC_EnableIRQ(USART1_IRQn);
  idle();
}

void USART3_IRQHandler(void)
{
  if(LL_USART_IsActiveFlag_RXNE_RXFNE(USART3))
  {
    LL_USART_TransmitData8(USART3, LL_USART_ReceiveData8(USART3));
  }
}

void link_self(void)
{
  start_link();
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART3);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART3_CLKSOURCE_PCLK1);
  init(USART3, LINK_BAUD);
  enable(USART3);
  LL_USART_EnableIT_RXNE_RXFNE(USART3);
  NVIC_EnableIRQ(USART3_IRQn);
  received_count = 0xffffffffu;
  LL_USART_EnableIT_RXNE_RXFNE(USART1);
  LL_USART_EnableIT_ERROR(USART1);
  NVIC_EnableIRQ(USART1_IRQn);
  uint32_t volatile const* const logged = &log_len;
  for(unsigned index = 0; index < 16; ++index)
  {
    uint32_t const before = *logged;
    send_byte((uint8_t)(0x40u + index));
    while(*logged == before)
    {
    }
    wait_us(30);
  }
  DONE();
}

volatile uint32_t link_count = 512u;

void link_tx_count(void)
{
  start_link();
  for(uint32_t index = 0; index < link_count; ++index)
  {
    send_byte((uint8_t)index);
  }
  wait_complete();
  DONE();
}

void link_abort_gap(void)
{
  start_link();
  send_byte('a');
  wait_complete();
  wait_us(60);
  send_byte('b');
  wait_us(10);
  LL_USART_Disable(USART1);
  enable(USART1);
  wait_us(100);
  send_byte('c');
  wait_complete();
  DONE();
}

void link_tx_reset(void)
{
  start_link();
  for(uint8_t next = 0x20; next < 0x26; ++next)
  {
    send_byte(next);
  }
  while(!LL_USART_IsActiveFlag_TXE_TXFNF(USART1))
  {
  }
  wait_us(10);
  NVIC_SystemReset();
}

void link_abort_b2b(void)
{
  start_link();
  send_byte('a');
  send_byte('b');
  while(!LL_USART_IsActiveFlag_TXE_TXFNF(USART1))
  {
  }
  wait_us(10);
  LL_USART_Disable(USART1);
  enable(USART1);
  wait_us(200);
  send_byte('c');
  wait_complete();
  DONE();
}

static void halve_hclk_mid_block(void)
{
  uint32_t const fast = RCC->CFGR2 & ~RCC_CFGR2_HPRE;
  uint32_t const slow = fast | LL_RCC_SYSCLK_DIV_2;
  __asm__ volatile(".rept 40\n\tnop\n\t.endr\n\t"
                   "str %1, [%0]\n\t"
                   ".rept 40\n\tnop\n\t.endr\n\t"
                   "str %2, [%0]\n\t"
                   ".rept 40\n\tnop\n\t.endr"
                   :
                   : "r"(&RCC->CFGR2), "r"(slow), "r"(fast)
                   : "memory");
}

void link_tx_mid_switch(void)
{
  clock_160mhz();
  halve_hclk_mid_block();
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
  init(USART1, LINK_BAUD);
  enable(USART1);
  for(uint8_t next = 0x20; next < 0x60; ++next)
  {
    send_byte(next);
  }
  wait_complete();
  DONE();
}

void link_tx_fifo_stop(void)
{
  start_link();
  LL_USART_Disable(USART1);
  LL_USART_EnableFIFO(USART1);
  enable(USART1);
  wait_us(400);
  for(uint8_t next = 0x40; next < 0x49; ++next)
  {
    send_byte(next);
  }
  DONE();
}

void link_rx_dma(void)
{
  start_link();
  ring_size = sizeof big_ring;
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  uint32_t const source = (uint32_t)&USART1->RDR;
  uint32_t const next   = LIST_UPDATE | ((uint32_t)rx_node1 & 0xfffcu);
  rx_node0[0]           = LL_DMA_SRC_FIXED | LL_DMA_DEST_INCREMENT;
  rx_node0[1]           = LL_DMA_TCEM_EACH_LLITEM_TRANSFER
                        | LL_DMA_DIRECTION_PERIPH_TO_MEMORY
                        | LL_GPDMA1_REQUEST_USART1_RX;
  rx_node0[2]           = sizeof big_ring;
  rx_node0[3]           = source;
  rx_node0[4]           = (uint32_t)big_ring;
  rx_node0[5]           = next;
  rx_node1[0]           = sizeof big_ring;
  rx_node1[1]           = source;
  rx_node1[2]           = (uint32_t)big_ring;
  rx_node1[3]           = next;
  LL_DMA_SetTransferEventMode(GPDMA1, LL_DMA_CHANNEL_3, LL_DMA_TCEM_RPT_BLK_TRANSFER);
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_3, 0);
  LL_DMA_SetLinkedListBaseAddr(GPDMA1, LL_DMA_CHANNEL_3, (uint32_t)rx_node0 & 0xffff0000u);
  LL_DMA_ConfigLinkUpdate(
      GPDMA1, LL_DMA_CHANNEL_3, LL_DMA_UPDATE_CTR1 | LL_DMA_UPDATE_CTR2 | LIST_UPDATE,
      (uint32_t)rx_node0);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_3);
  LL_USART_EnableDMAReq_RX(USART1);
  idle();
}

void link_iwdg(void)
{
  start_link();
  LL_IWDG_Enable(IWDG);
  LL_IWDG_EnableWriteAccess(IWDG);
  LL_IWDG_SetPrescaler(IWDG, LL_IWDG_PRESCALER_4);
  LL_IWDG_SetReloadCounter(IWDG, 40);
  while(!LL_IWDG_IsReady(IWDG))
  {
  }
  LL_IWDG_ReloadCounter(IWDG);
  for(uint8_t next = 0;; ++next)
  {
    send_byte(next);
  }
}

#define AUDIO_BLOCK     512u
#define AUDIO_PERIOD_MS 16u

typedef struct
{
  uint32_t rx_bytes;
  uint32_t rx_hash;
  uint32_t tx_bytes;
  uint32_t tx_left;
  uint32_t blocks;
  uint32_t errors;
  uint32_t respond;
} audio_port;

audio_port               audio_ports[2];
static volatile uint32_t audio_ticks;

void SysTick_Handler(void)
{
  ++audio_ticks;
}

static USART_TypeDef* audio_usart(uint32_t port)
{
  return port == 0u ? USART1 : USART2;
}

static void audio_block(uint32_t port)
{
  audio_ports[port].tx_left = AUDIO_BLOCK;
  LL_USART_EnableIT_TXE_TXFNF(audio_usart(port));
}

static void audio_irq(uint32_t port)
{
  USART_TypeDef* const usart = audio_usart(port);
  audio_port* const    state = &audio_ports[port];
  if(LL_USART_IsActiveFlag_ORE(usart)
     || LL_USART_IsActiveFlag_FE(usart)
     || LL_USART_IsActiveFlag_NE(usart))
  {
    LL_USART_ClearFlag_ORE(usart);
    LL_USART_ClearFlag_FE(usart);
    LL_USART_ClearFlag_NE(usart);
    ++state->errors;
  }
  if(LL_USART_IsActiveFlag_RXNE_RXFNE(usart))
  {
    uint8_t const data = LL_USART_ReceiveData8(usart);
    state->rx_hash     = (state->rx_hash ^ data) * 16777619u;
    if(state->rx_bytes++ % AUDIO_BLOCK == 0u && state->respond != 0u && state->tx_left == 0u)
    {
      audio_block(port);
    }
  }
  if(LL_USART_IsEnabledIT_TXE_TXFNF(usart) && LL_USART_IsActiveFlag_TXE_TXFNF(usart))
  {
    LL_USART_TransmitData8(usart, (uint8_t)(state->tx_bytes++ * 7u + port));
    if(--state->tx_left == 0u)
    {
      LL_USART_DisableIT_TXE_TXFNF(usart);
      ++state->blocks;
    }
  }
}

static void audio_start(USART_TypeDef* usart, IRQn_Type irq)
{
  init(usart, LINK_BAUD);
  enable(usart);
  LL_USART_EnableIT_RXNE_RXFNE(usart);
  LL_USART_EnableIT_ERROR(usart);
  NVIC_EnableIRQ(irq);
}

void link_audio(void)
{
  clock_160mhz();
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART2_CLKSOURCE_PCLK1);
  audio_on = 1u;
  audio_start(USART1, USART1_IRQn);
  audio_start(USART2, USART2_IRQn);
  SysTick_Config(160000u);
  uint32_t next = 0u;
  for(;;)
  {
    while(audio_ticks < next)
    {
    }
    audio_block(0);
    audio_block(1);
    next += AUDIO_PERIOD_MS;
  }
}

void link_audio_peer(void)
{
  clock_160mhz();
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
  audio_on               = 1u;
  audio_ports[0].respond = 1u;
  audio_start(USART1, USART1_IRQn);
  idle();
}

void link_audio_tim6(void)
{
  clock_160mhz();
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART2_CLKSOURCE_PCLK1);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
  audio_on = 1u;
  audio_start(USART1, USART1_IRQn);
  audio_start(USART2, USART2_IRQn);
  SysTick_Config(160000u);
  uint32_t next    = 0u;
  uint32_t written = 0u;
  for(;;)
  {
    while(audio_ticks < next)
    {
      uint32_t const now = audio_ticks;
      if(now != written)
      {
        written   = now;
        TIM6->ARR = now;
      }
    }
    audio_block(0);
    audio_block(1);
    next += AUDIO_PERIOD_MS;
  }
}

void link_ue_toggle(void)
{
  start_link();
  for(uint32_t round = 0; round < 20000u; ++round)
  {
    send_byte((uint8_t)round);
    wait_us(1);
    LL_USART_Disable(USART1);
    enable(USART1);
  }
  send_byte('z');
  wait_complete();
  DONE();
}
