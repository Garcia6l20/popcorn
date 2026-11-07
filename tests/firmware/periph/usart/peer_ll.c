#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_dma.h"
#include "stm32u5xx_ll_pwr.h"
#include "stm32u5xx_ll_rcc.h"
#include "stm32u5xx_ll_usart.h"

void idle(void);

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

static void enable(USART_TypeDef* usart)
{
  LL_USART_Enable(usart);
  while(!LL_USART_IsActiveFlag_TEACK(usart) || !LL_USART_IsActiveFlag_REACK(usart))
  {
  }
}

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

static void wait_us(uint32_t us)
{
  uint32_t const from  = DWT->CYCCNT;
  uint32_t const ticks = us * 160u;
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

void peer_host(void);

#define PEER_BATCH 16u
#define PEER_FAST  921600u

uint8_t peer_out[PEER_BATCH * 4u];
uint8_t peer_in[PEER_BATCH * 7u];
uint8_t peer_hello[4];
uint8_t peer_first[7];
uint8_t peer_baud[7];

static uint8_t peer_get(void)
{
  while(!LL_USART_IsActiveFlag_RXNE_RXFNE(USART1))
  {
  }
  return LL_USART_ReceiveData8(USART1);
}

static void peer_command(uint8_t const* command, uint8_t* event)
{
  for(unsigned index = 0; index < 4; ++index)
  {
    send_byte(command[index]);
  }
  for(unsigned index = 0; index < 7; ++index)
  {
    event[index] = peer_get();
  }
}

void peer_host(void)
{
  static uint8_t const first[4] = {0x01, 0x03, 0x0C, 0x00};
  static uint8_t const baud[4]  = {0x01, 0x18, 0xFC, 0x00};
  clock_160mhz();
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
  init(USART1, 115200u);
  LL_USART_SetHWFlowCtrl(USART1, LL_USART_HWCONTROL_RTS_CTS);
  enable(USART1);
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);
  GPIOA->MODER = (GPIOA->MODER & ~0x3Cu) | 0x14u;
  GPIOA->BSRR  = 1u << 2;
  wait_us(1000);
  GPIOA->BSRR = 1u << 1;
  for(unsigned index = 0; index < 4; ++index)
  {
    peer_hello[index] = peer_get();
  }
  peer_command(first, peer_first);
  peer_command(baud, peer_baud);
  wait_complete();
  LL_USART_Disable(USART1);
  LL_USART_SetBaudRate(
      USART1, 160000000u, LL_USART_PRESCALER_DIV1, LL_USART_OVERSAMPLING_16, PEER_FAST);
  enable(USART1);
  for(unsigned index = 0; index < PEER_BATCH; ++index)
  {
    peer_out[4 * index]     = 0x01;
    peer_out[4 * index + 1] = (uint8_t)(0x20u + index);
    peer_out[4 * index + 2] = 0x0C;
    peer_out[4 * index + 3] = 0x00;
  }
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPDMA1);
  LL_DMA_ConfigTransfer(
      GPDMA1, LL_DMA_CHANNEL_1,
      LL_DMA_SRC_FIXED
          | LL_DMA_DEST_INCREMENT
          | LL_DMA_SRC_DATAWIDTH_BYTE
          | LL_DMA_DEST_DATAWIDTH_BYTE);
  LL_DMA_SetDataTransferDirection(GPDMA1, LL_DMA_CHANNEL_1, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
  LL_DMA_SetPeriphRequest(GPDMA1, LL_DMA_CHANNEL_1, LL_GPDMA1_REQUEST_USART1_RX);
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_1, sizeof peer_in);
  LL_DMA_SetSrcAddress(GPDMA1, LL_DMA_CHANNEL_1, (uint32_t)&USART1->RDR);
  LL_DMA_SetDestAddress(GPDMA1, LL_DMA_CHANNEL_1, (uint32_t)peer_in);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_1);
  LL_USART_EnableDMAReq_RX(USART1);
  LL_DMA_ConfigTransfer(
      GPDMA1, LL_DMA_CHANNEL_0,
      LL_DMA_SRC_INCREMENT
          | LL_DMA_DEST_FIXED
          | LL_DMA_SRC_DATAWIDTH_BYTE
          | LL_DMA_DEST_DATAWIDTH_BYTE);
  LL_DMA_SetDataTransferDirection(GPDMA1, LL_DMA_CHANNEL_0, LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
  LL_DMA_SetPeriphRequest(GPDMA1, LL_DMA_CHANNEL_0, LL_GPDMA1_REQUEST_USART1_TX);
  LL_DMA_SetBlkDataLength(GPDMA1, LL_DMA_CHANNEL_0, sizeof peer_out);
  LL_DMA_SetSrcAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)peer_out);
  LL_DMA_SetDestAddress(GPDMA1, LL_DMA_CHANNEL_0, (uint32_t)&USART1->TDR);
  LL_DMA_EnableChannel(GPDMA1, LL_DMA_CHANNEL_0);
  LL_USART_ClearFlag_TC(USART1);
  LL_USART_EnableDMAReq_TX(USART1);
  while(!LL_DMA_IsActiveFlag_TC(GPDMA1, LL_DMA_CHANNEL_1))
  {
  }
  LOG(0x50454552u);
  idle();
}

int main(void)
{
  peer_host();
}
