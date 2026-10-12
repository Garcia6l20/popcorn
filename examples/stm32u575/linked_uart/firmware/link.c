#include "link.h"

#include <stdio.h>
#include <string.h>

#define WATCHDOG_TIMEOUT_MS 500u

volatile uint32_t starve;
uint32_t          reset_by_watchdog;

static UART_HandleTypeDef console;
static IWDG_HandleTypeDef watchdog;
static char               stdout_buffer[128];
static struct link*       uart4_link;
static struct link*       usart3_link;

int  _write(int file, char const* data, int length);
void SysTick_Handler(void);
void UART4_IRQHandler(void);
void USART3_IRQHandler(void);
void GPDMA1_Channel0_IRQHandler(void);
void GPDMA1_Channel1_IRQHandler(void);
void GPDMA1_Channel2_IRQHandler(void);
void GPDMA1_Channel3_IRQHandler(void);

int _write(int file, char const* data, int length)
{
  (void)file;
  if(HAL_UART_Transmit(&console, (uint8_t const*)data, (uint16_t)length, HAL_MAX_DELAY) != HAL_OK)
  {
    return -1;
  }
  return length;
}

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void links_ready(void)
{}

static void clock_init(void)
{
  if(HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }
  RCC_OscInitTypeDef oscillators = {
      .OscillatorType      = RCC_OSCILLATORTYPE_HSI,
      .HSIState            = RCC_HSI_ON,
      .HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT,
      .PLL =
          {
                .PLLState  = RCC_PLL_ON,
                .PLLSource = RCC_PLLSOURCE_HSI,
                .PLLMBOOST = RCC_PLLMBOOST_DIV1,
                .PLLM      = 1,
                .PLLN      = 10,
                .PLLP      = 2,
                .PLLQ      = 2,
                .PLLR      = 1,
                .PLLRGE    = RCC_PLLVCIRANGE_1,
                .PLLFRACN  = 0,
                },
  };
  if(HAL_RCC_OscConfig(&oscillators) != HAL_OK)
  {
    Error_Handler();
  }
  RCC_ClkInitTypeDef clocks = {
      .ClockType      = RCC_CLOCKTYPE_HCLK
                      | RCC_CLOCKTYPE_SYSCLK
                      | RCC_CLOCKTYPE_PCLK1
                      | RCC_CLOCKTYPE_PCLK2
                      | RCC_CLOCKTYPE_PCLK3,
      .SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK,
      .AHBCLKDivider  = RCC_SYSCLK_DIV1,
      .APB1CLKDivider = RCC_HCLK_DIV1,
      .APB2CLKDivider = RCC_HCLK_DIV1,
      .APB3CLKDivider = RCC_HCLK_DIV1,
  };
  if(HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

static void uart_init(UART_HandleTypeDef* uart, USART_TypeDef* instance, uint32_t baud)
{
  uart->Instance                    = instance;
  uart->Init.BaudRate               = baud;
  uart->Init.WordLength             = UART_WORDLENGTH_8B;
  uart->Init.StopBits               = UART_STOPBITS_1;
  uart->Init.Parity                 = UART_PARITY_NONE;
  uart->Init.Mode                   = UART_MODE_TX_RX;
  uart->Init.HwFlowCtl              = UART_HWCONTROL_NONE;
  uart->Init.OverSampling           = UART_OVERSAMPLING_16;
  uart->Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
  uart->Init.ClockPrescaler         = UART_PRESCALER_DIV1;
  uart->AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if(HAL_UART_Init(uart) != HAL_OK)
  {
    Error_Handler();
  }
}

static void watchdog_init(void)
{
  reset_by_watchdog = __HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) ? 1u : 0u;
  __HAL_RCC_CLEAR_RESET_FLAGS();
  __HAL_DBGMCU_FREEZE_IWDG();
  watchdog.Instance       = IWDG;
  watchdog.Init.Prescaler = IWDG_PRESCALER_32;
  watchdog.Init.Window    = IWDG_WINDOW_DISABLE;
  watchdog.Init.Reload    = WATCHDOG_TIMEOUT_MS * (LSI_VALUE / 32u) / 1000u;
  watchdog.Init.EWI       = 0;
  if(HAL_IWDG_Init(&watchdog) != HAL_OK)
  {
    Error_Handler();
  }
}

void board_init(void)
{
  HAL_Init();
  clock_init();
  uart_init(&console, USART1, 115200u);
  setvbuf(stdout, stdout_buffer, _IOLBF, sizeof stdout_buffer);
  watchdog_init();
  __HAL_RCC_GPDMA1_CLK_ENABLE();
}

static DMA_InitTypeDef transfer(uint32_t request, uint32_t direction)
{
  bool const to_uart = direction == DMA_MEMORY_TO_PERIPH;
  return (DMA_InitTypeDef){
      .Request               = request,
      .BlkHWRequest          = DMA_BREQ_SINGLE_BURST,
      .Direction             = direction,
      .SrcInc                = to_uart ? DMA_SINC_INCREMENTED : DMA_SINC_FIXED,
      .DestInc               = to_uart ? DMA_DINC_FIXED : DMA_DINC_INCREMENTED,
      .SrcDataWidth          = DMA_SRC_DATAWIDTH_BYTE,
      .DestDataWidth         = DMA_DEST_DATAWIDTH_BYTE,
      .Priority              = DMA_LOW_PRIORITY_LOW_WEIGHT,
      .SrcBurstLength        = 1,
      .DestBurstLength       = 1,
      .TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT0,
      .TransferEventMode     = DMA_TCEM_BLOCK_TRANSFER,
      .Mode                  = DMA_NORMAL,
  };
}

static void tx_dma_init(struct link* link, DMA_Channel_TypeDef* channel, uint32_t request)
{
  link->tx_dma.Instance = channel;
  link->tx_dma.Init     = transfer(request, DMA_MEMORY_TO_PERIPH);
  if(HAL_DMA_Init(&link->tx_dma) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(&link->uart, hdmatx, link->tx_dma);
}

static void rx_dma_init(struct link* link, DMA_Channel_TypeDef* channel, uint32_t request)
{
  DMA_NodeConfTypeDef config = {
      .NodeType = DMA_GPDMA_LINEAR_NODE,
      .Init     = transfer(request, DMA_PERIPH_TO_MEMORY),
      .DataHandlingConfig =
          {.DataExchange = DMA_EXCHANGE_NONE, .DataAlignment = DMA_DATA_RIGHTALIGN_ZEROPADDED},
      .TriggerConfig = {.TriggerPolarity = DMA_TRIG_POLARITY_MASKED},
      .SrcAddress    = (uint32_t)&link->uart.Instance->RDR,
      .DstAddress    = (uint32_t)link->ring,
      .DataSize      = RING_BYTES,
  };
  if(HAL_DMAEx_List_BuildNode(&config, &link->rx_node) != HAL_OK
     || HAL_DMAEx_List_InsertNode_Tail(&link->rx_queue, &link->rx_node) != HAL_OK
     || HAL_DMAEx_List_SetCircularMode(&link->rx_queue) != HAL_OK)
  {
    Error_Handler();
  }
  link->rx_dma.Instance       = channel;
  link->rx_dma.InitLinkedList = (DMA_InitLinkedListTypeDef){
      .Priority          = DMA_LOW_PRIORITY_LOW_WEIGHT,
      .LinkStepMode      = DMA_LSM_FULL_EXECUTION,
      .LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT0,
      .TransferEventMode = DMA_TCEM_BLOCK_TRANSFER,
      .LinkedListMode    = DMA_LINKEDLIST_CIRCULAR,
  };
  if(HAL_DMAEx_List_Init(&link->rx_dma) != HAL_OK
     || HAL_DMAEx_List_LinkQ(&link->rx_dma, &link->rx_queue) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(&link->uart, hdmarx, link->rx_dma);
}

static void receive(struct link* link)
{
  link->ring_read        = 0;
  link->partial_size     = 0;
  link->partial_overlong = false;
  if(HAL_UARTEx_ReceiveToIdle_DMA(&link->uart, link->ring, RING_BYTES) != HAL_OK)
  {
    Error_Handler();
  }
}

static void enable_irq(IRQn_Type irq)
{
  HAL_NVIC_SetPriority(irq, 5, 0);
  HAL_NVIC_EnableIRQ(irq);
}

void link_init(struct link* link, USART_TypeDef* instance)
{
  memset(link, 0, sizeof *link);
  if(instance == UART4)
  {
    uart4_link = link;
  }
  else
  {
    usart3_link = link;
  }
  uart_init(&link->uart, instance, LINK_BAUD);
  if(instance == UART4)
  {
    tx_dma_init(link, GPDMA1_Channel0, GPDMA1_REQUEST_UART4_TX);
    rx_dma_init(link, GPDMA1_Channel1, GPDMA1_REQUEST_UART4_RX);
    enable_irq(GPDMA1_Channel0_IRQn);
    enable_irq(GPDMA1_Channel1_IRQn);
  }
  else
  {
    tx_dma_init(link, GPDMA1_Channel2, GPDMA1_REQUEST_USART3_TX);
    rx_dma_init(link, GPDMA1_Channel3, GPDMA1_REQUEST_USART3_RX);
    enable_irq(GPDMA1_Channel2_IRQn);
    enable_irq(GPDMA1_Channel3_IRQn);
  }
  link->tx_done_tick = HAL_GetTick() - TX_GAP_TICKS;
  receive(link);
}

bool link_can_send(struct link const* link)
{
  return !link->tx_busy && HAL_GetTick() - link->tx_done_tick >= TX_GAP_TICKS;
}

void link_send(struct link* link, uint16_t size)
{
  link->tx_busy = true;
  if(HAL_UART_Transmit_DMA(&link->uart, link->tx, size) != HAL_OK)
  {
    Error_Handler();
  }
}

bool link_has_message(struct link const* link)
{
  return link->message_ready;
}

uint16_t link_take(struct link* link, uint8_t* out)
{
  if(!link->message_ready)
  {
    return 0;
  }
  uint16_t const size = link->message_size;
  memcpy(out, link->message, size);
  link->message_ready = false;
  return size;
}

uint32_t block_checksum(uint8_t const* data, uint16_t size)
{
  uint32_t sum  = 0;
  uint32_t sums = 0;
  for(uint16_t index = 0; index < size; ++index)
  {
    sum  = (sum + data[index]) & 0xffffu;
    sums = (sums + sum) & 0xffffu;
  }
  return sums << 16 | sum;
}

void idle(bool (*work)(void))
{
  if(starve == 0)
  {
    HAL_IWDG_Refresh(&watchdog);
  }
  __disable_irq();
  if(!work())
  {
    __WFI();
  }
  __enable_irq();
}

static struct link* link_of(UART_HandleTypeDef const* huart)
{
  if(uart4_link != NULL && huart == &uart4_link->uart)
  {
    return uart4_link;
  }
  if(usart3_link != NULL && huart == &usart3_link->uart)
  {
    return usart3_link;
  }
  return NULL;
}

static void consume(struct link* link, uint16_t position)
{
  while(link->ring_read != position)
  {
    if(link->partial_size < MESSAGE_BYTES)
    {
      link->partial[link->partial_size++] = link->ring[link->ring_read];
    }
    else
    {
      link->partial_overlong = true;
    }
    ++link->received;
    link->ring_read = (uint16_t)((link->ring_read + 1u) % RING_BYTES);
  }
}

static void cut(struct link* link)
{
  if(link->partial_size != 0 && !link->partial_overlong)
  {
    if(link->message_ready)
    {
      ++link->messages_dropped;
    }
    else
    {
      memcpy(link->message, link->partial, link->partial_size);
      link->message_size  = link->partial_size;
      link->message_ready = true;
    }
  }
  link->partial_size     = 0;
  link->partial_overlong = false;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t size)
{
  struct link* const link = link_of(huart);
  if(link == NULL)
  {
    return;
  }
  consume(link, (uint16_t)(size % RING_BYTES));
  if(HAL_UARTEx_GetRxEventType(huart) == HAL_UART_RXEVENT_IDLE)
  {
    cut(link);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart)
{
  struct link* const link = link_of(huart);
  if(link == NULL)
  {
    return;
  }
  ++link->errors;
  if(huart->RxState == HAL_UART_STATE_READY)
  {
    receive(link);
  }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart)
{
  struct link* const link = link_of(huart);
  if(link != NULL)
  {
    link->tx_done_tick = HAL_GetTick();
    link->tx_busy      = false;
  }
}

void HAL_UART_MspInit(UART_HandleTypeDef* huart)
{
  GPIO_InitTypeDef pins = {
      .Mode  = GPIO_MODE_AF_PP,
      .Pull  = GPIO_NOPULL,
      .Speed = GPIO_SPEED_FREQ_HIGH,
  };
  if(huart->Instance == USART1)
  {
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    pins.Pin       = GPIO_PIN_9 | GPIO_PIN_10;
    pins.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &pins);
  }
  else if(huart->Instance == UART4)
  {
    __HAL_RCC_UART4_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    pins.Pin       = GPIO_PIN_0 | GPIO_PIN_1;
    pins.Alternate = GPIO_AF8_UART4;
    HAL_GPIO_Init(GPIOA, &pins);
    enable_irq(UART4_IRQn);
  }
  else if(huart->Instance == USART3)
  {
    __HAL_RCC_USART3_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    pins.Pin       = GPIO_PIN_10 | GPIO_PIN_11;
    pins.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOB, &pins);
    enable_irq(USART3_IRQn);
  }
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void UART4_IRQHandler(void)
{
  HAL_UART_IRQHandler(&uart4_link->uart);
}

void USART3_IRQHandler(void)
{
  HAL_UART_IRQHandler(&usart3_link->uart);
}

void GPDMA1_Channel0_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&uart4_link->tx_dma);
}

void GPDMA1_Channel1_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&uart4_link->rx_dma);
}

void GPDMA1_Channel2_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&usart3_link->tx_dma);
}

void GPDMA1_Channel3_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&usart3_link->rx_dma);
}
