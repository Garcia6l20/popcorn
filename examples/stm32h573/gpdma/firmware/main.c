#include "main.h"

#include <stdint.h>

#define BLOCK_BYTES 256u
#define HEAD_BYTES  64u
#define TAIL_BYTES  32u

UART_HandleTypeDef huart9;
DMA_HandleTypeDef  handle_GPDMA1_Channel0;

volatile uint32_t transfers_done;

static uint8_t          block[BLOCK_BYTES];
static uint8_t          head[HEAD_BYTES];
static uint8_t          tail[TAIL_BYTES];
static DMA_NodeTypeDef  head_node;
static DMA_NodeTypeDef  tail_node;
static DMA_QListTypeDef queue;

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void all_transfers_done(void)
{}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart)
{
  if(huart->Instance == UART9)
  {
    ++transfers_done;
  }
}

static void MX_GPDMA1_Init(void)
{
  __HAL_RCC_GPDMA1_CLK_ENABLE();
  HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
}

static void MX_UART9_Init(void)
{
  huart9.Instance                    = UART9;
  huart9.Init.BaudRate               = 921600;
  huart9.Init.WordLength             = UART_WORDLENGTH_8B;
  huart9.Init.StopBits               = UART_STOPBITS_1;
  huart9.Init.Parity                 = UART_PARITY_NONE;
  huart9.Init.Mode                   = UART_MODE_TX;
  huart9.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
  huart9.Init.OverSampling           = UART_OVERSAMPLING_16;
  huart9.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
  huart9.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
  huart9.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if(HAL_UART_Init(&huart9) != HAL_OK)
  {
    Error_Handler();
  }
}

static DMA_InitTypeDef transfer_config(uint32_t event_mode)
{
  return (DMA_InitTypeDef){
      .Request               = GPDMA1_REQUEST_UART9_TX,
      .BlkHWRequest          = DMA_BREQ_SINGLE_BURST,
      .Direction             = DMA_MEMORY_TO_PERIPH,
      .SrcInc                = DMA_SINC_INCREMENTED,
      .DestInc               = DMA_DINC_FIXED,
      .SrcDataWidth          = DMA_SRC_DATAWIDTH_BYTE,
      .DestDataWidth         = DMA_DEST_DATAWIDTH_BYTE,
      .Priority              = DMA_LOW_PRIORITY_LOW_WEIGHT,
      .SrcBurstLength        = 1,
      .DestBurstLength       = 1,
      .TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT0,
      .TransferEventMode     = event_mode,
      .Mode                  = DMA_NORMAL,
  };
}

static void link_normal_channel(void)
{
  handle_GPDMA1_Channel0.Instance = GPDMA1_Channel0;
  handle_GPDMA1_Channel0.Init     = transfer_config(DMA_TCEM_BLOCK_TRANSFER);
  if(HAL_DMA_Init(&handle_GPDMA1_Channel0) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(&huart9, hdmatx, handle_GPDMA1_Channel0);
}

static void build_node(DMA_NodeTypeDef* node, uint8_t const* data, uint32_t size)
{
  DMA_DataHandlingConfTypeDef const handling = {
      .DataExchange  = DMA_EXCHANGE_NONE,
      .DataAlignment = DMA_DATA_RIGHTALIGN_ZEROPADDED,
  };
  DMA_TriggerConfTypeDef const trigger = {.TriggerPolarity = DMA_TRIG_POLARITY_MASKED};
  DMA_NodeConfTypeDef          config  = {
      .NodeType           = DMA_GPDMA_LINEAR_NODE,
      .Init               = transfer_config(DMA_TCEM_LAST_LL_ITEM_TRANSFER),
      .DataHandlingConfig = handling,
      .TriggerConfig      = trigger,
      .SrcAddress         = (uint32_t)data,
      .DstAddress         = (uint32_t)&UART9->TDR,
      .DataSize           = size,
  };
  if(HAL_DMAEx_List_BuildNode(&config, node) != HAL_OK)
  {
    Error_Handler();
  }
}

static void link_linked_list_channel(void)
{
  build_node(&head_node, head, HEAD_BYTES);
  build_node(&tail_node, tail, TAIL_BYTES);
  if(HAL_DMAEx_List_InsertNode_Tail(&queue, &head_node) != HAL_OK)
  {
    Error_Handler();
  }
  if(HAL_DMAEx_List_InsertNode_Tail(&queue, &tail_node) != HAL_OK)
  {
    Error_Handler();
  }
  if(HAL_DMA_DeInit(&handle_GPDMA1_Channel0) != HAL_OK)
  {
    Error_Handler();
  }
  handle_GPDMA1_Channel0.InitLinkedList = (DMA_InitLinkedListTypeDef){
      .Priority          = DMA_LOW_PRIORITY_LOW_WEIGHT,
      .LinkStepMode      = DMA_LSM_FULL_EXECUTION,
      .LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT0,
      .TransferEventMode = DMA_TCEM_LAST_LL_ITEM_TRANSFER,
      .LinkedListMode    = DMA_LINKEDLIST_NORMAL,
  };
  if(HAL_DMAEx_List_Init(&handle_GPDMA1_Channel0) != HAL_OK)
  {
    Error_Handler();
  }
  if(HAL_DMAEx_List_LinkQ(&handle_GPDMA1_Channel0, &queue) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(&huart9, hdmatx, handle_GPDMA1_Channel0);
}

static void send(uint8_t const* data, uint16_t size)
{
  uint32_t const before = transfers_done;
  if(HAL_UART_Transmit_DMA(&huart9, data, size) != HAL_OK)
  {
    Error_Handler();
  }
  __disable_irq();
  while(transfers_done == before)
  {
    __WFI();
    __enable_irq();
    __disable_irq();
  }
  __enable_irq();
}

int main(void)
{
  HAL_Init();
  for(uint32_t index = 0; index < BLOCK_BYTES; ++index)
  {
    block[index] = (uint8_t)index;
  }
  for(uint32_t index = 0; index < HEAD_BYTES; ++index)
  {
    head[index] = (uint8_t)('A' + index % 26);
  }
  for(uint32_t index = 0; index < TAIL_BYTES; ++index)
  {
    tail[index] = (uint8_t)('a' + index % 26);
  }
  MX_GPDMA1_Init();
  MX_UART9_Init();

  link_normal_channel();
  send(block, BLOCK_BYTES);

  link_linked_list_channel();
  send(head, HEAD_BYTES);

  all_transfers_done();
  for(;;)
  {
    __WFI();
  }
}
