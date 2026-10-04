#include "main.h"

#define I2C_TIMING_100K_AT_4M 0x00420F13u
#define BLOCK_BYTES           (2u * sizeof(int16_t))
#define CHANNEL_STRIDE        ((int32_t)(FRAMES * sizeof(int16_t)))

SAI_HandleTypeDef hsai_BlockA1;
SAI_HandleTypeDef hsai_BlockB1;
I2C_HandleTypeDef hi2c1;
DMA_HandleTypeDef handle_GPDMA1_Channel12;
DMA_HandleTypeDef handle_GPDMA1_Channel13;
struct stream     stream;
int16_t           playback[2][FRAMES];
int16_t           capture[2][FRAMES];

static DMA_NodeTypeDef  tx_node;
static DMA_NodeTypeDef  rx_node;
static DMA_QListTypeDef tx_queue;
static DMA_QListTypeDef rx_queue;

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void codec_stopped(void)
{}

static void tx_half(DMA_HandleTypeDef* hdma)
{
  (void)hdma;
  ++stream.tx_halves;
}

static void tx_complete(DMA_HandleTypeDef* hdma)
{
  (void)hdma;
  ++stream.tx_completes;
}

static void rx_half(DMA_HandleTypeDef* hdma)
{
  (void)hdma;
  ++stream.rx_halves;
}

static void rx_complete(DMA_HandleTypeDef* hdma)
{
  (void)hdma;
  ++stream.rx_completes;
}

static void MX_SAI1_Clock_Init(void)
{
  RCC_OscInitTypeDef oscillator = {
      .OscillatorType      = RCC_OSCILLATORTYPE_HSI,
      .HSIState            = RCC_HSI_ON,
      .HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT,
      .PLL                 = {.PLLState = RCC_PLL_NONE},
  };
  RCC_PeriphCLKInitTypeDef clock = {
      .PeriphClockSelection = RCC_PERIPHCLK_SAI1,
      .Sai1ClockSelection   = RCC_SAI1CLKSOURCE_HSI,
  };
  if(HAL_RCC_OscConfig(&oscillator) != HAL_OK || HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_GPDMA1_Init(void)
{
  __HAL_RCC_GPDMA1_CLK_ENABLE();
  HAL_NVIC_SetPriority(GPDMA1_Channel12_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(GPDMA1_Channel12_IRQn);
  HAL_NVIC_SetPriority(GPDMA1_Channel13_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(GPDMA1_Channel13_IRQn);
}

static void MX_I2C1_Init(void)
{
  hi2c1.Instance              = I2C1;
  hi2c1.Init.Timing           = I2C_TIMING_100K_AT_4M;
  hi2c1.Init.OwnAddress1      = 0;
  hi2c1.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2      = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
  if(HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
}

static void
    slave_block(SAI_HandleTypeDef* hsai, SAI_Block_TypeDef* block, uint32_t mode, uint32_t synchro)
{
  hsai->Instance            = block;
  hsai->Init.AudioMode      = mode;
  hsai->Init.Synchro        = synchro;
  hsai->Init.OutputDrive    = SAI_OUTPUTDRIVE_DISABLE;
  hsai->Init.NoDivider      = SAI_MASTERDIVIDER_DISABLE;
  hsai->Init.FIFOThreshold  = SAI_FIFOTHRESHOLD_EMPTY;
  hsai->Init.AudioFrequency = SAI_AUDIO_FREQUENCY_MCKDIV;
  hsai->Init.SynchroExt     = SAI_SYNCEXT_DISABLE;
  hsai->Init.MckOutput      = SAI_MCK_OUTPUT_DISABLE;
  hsai->Init.MonoStereoMode = SAI_STEREOMODE;
  hsai->Init.CompandingMode = SAI_NOCOMPANDING;
  hsai->Init.TriState       = SAI_OUTPUT_NOTRELEASED;
  if(HAL_SAI_InitProtocol(hsai, SAI_I2S_STANDARD, SAI_PROTOCOL_DATASIZE_16BIT, 2) != HAL_OK)
  {
    Error_Handler();
  }
}

static void stereo_list(
    DMA_HandleTypeDef*         handle,
    DMA_Channel_TypeDef*       channel,
    DMA_NodeTypeDef*           node,
    DMA_QListTypeDef*          queue,
    DMA_NodeConfTypeDef const* config)
{
  if(HAL_DMAEx_List_BuildNode(config, node)
     != HAL_OK
     || HAL_DMAEx_List_InsertNode_Tail(queue, node)
     != HAL_OK
     || HAL_DMAEx_List_SetCircularMode(queue)
     != HAL_OK)
  {
    Error_Handler();
  }
  handle->Instance       = channel;
  handle->InitLinkedList = (DMA_InitLinkedListTypeDef){
      .Priority          = DMA_HIGH_PRIORITY,
      .LinkStepMode      = DMA_LSM_FULL_EXECUTION,
      .LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT0,
      .TransferEventMode = DMA_TCEM_REPEATED_BLOCK_TRANSFER,
      .LinkedListMode    = DMA_LINKEDLIST_CIRCULAR,
  };
  if(HAL_DMAEx_List_Init(handle) != HAL_OK || HAL_DMAEx_List_LinkQ(handle, queue) != HAL_OK)
  {
    Error_Handler();
  }
}

static DMA_NodeConfTypeDef stereo_node(uint32_t request, uint32_t direction)
{
  return (DMA_NodeConfTypeDef){
      .NodeType = DMA_GPDMA_2D_NODE,
      .Init =
          {
                 .Request      = request,
                 .BlkHWRequest = DMA_BREQ_SINGLE_BURST,
                 .Direction    = direction,
                 .SrcInc  = direction == DMA_MEMORY_TO_PERIPH ? DMA_SINC_INCREMENTED : DMA_SINC_FIXED,
                 .DestInc = direction == DMA_MEMORY_TO_PERIPH ? DMA_DINC_FIXED : DMA_DINC_INCREMENTED,
                 .SrcDataWidth          = DMA_SRC_DATAWIDTH_HALFWORD,
                 .DestDataWidth         = DMA_DEST_DATAWIDTH_HALFWORD,
                 .SrcBurstLength        = 1,
                 .DestBurstLength       = 1,
                 .TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT0,
                 .TransferEventMode     = DMA_TCEM_REPEATED_BLOCK_TRANSFER,
                 .Mode                  = DMA_NORMAL,
                 },
      .DataHandlingConfig =
          {.DataExchange = DMA_EXCHANGE_NONE, .DataAlignment = DMA_DATA_RIGHTALIGN_ZEROPADDED},
      .TriggerConfig = {.TriggerPolarity = DMA_TRIG_POLARITY_MASKED},
      .DataSize      = BLOCK_BYTES,
  };
}

static void MX_SAI1_DMA_Init(void)
{
  DMA_NodeConfTypeDef tx = stereo_node(GPDMA1_REQUEST_SAI1_A, DMA_MEMORY_TO_PERIPH);
  tx.SrcAddress          = (uint32_t)playback;
  tx.DstAddress          = (uint32_t)&SAI1_Block_A->DR;
  tx.RepeatBlockConfig   = (DMA_RepeatBlockConfTypeDef){
      .RepeatCount      = FRAMES,
      .SrcAddrOffset    = CHANNEL_STRIDE - 2,
      .BlkSrcAddrOffset = -(2 * CHANNEL_STRIDE - 2),
  };
  stereo_list(&handle_GPDMA1_Channel12, GPDMA1_Channel12, &tx_node, &tx_queue, &tx);

  DMA_NodeConfTypeDef rx = stereo_node(GPDMA1_REQUEST_SAI1_B, DMA_PERIPH_TO_MEMORY);
  rx.SrcAddress          = (uint32_t)&SAI1_Block_B->DR;
  rx.DstAddress          = (uint32_t)capture;
  rx.RepeatBlockConfig   = (DMA_RepeatBlockConfTypeDef){
      .RepeatCount       = FRAMES,
      .DestAddrOffset    = CHANNEL_STRIDE - 2,
      .BlkDestAddrOffset = -(2 * CHANNEL_STRIDE - 2),
  };
  stereo_list(&handle_GPDMA1_Channel13, GPDMA1_Channel13, &rx_node, &rx_queue, &rx);

  handle_GPDMA1_Channel12.XferHalfCpltCallback = tx_half;
  handle_GPDMA1_Channel12.XferCpltCallback     = tx_complete;
  handle_GPDMA1_Channel13.XferHalfCpltCallback = rx_half;
  handle_GPDMA1_Channel13.XferCpltCallback     = rx_complete;
}

static void fill_playback(void)
{
  for(uint32_t frame = 0; frame < FRAMES; ++frame)
  {
    playback[0][frame] = (int16_t)(frame * 512u);
    playback[1][frame] = (int16_t)-playback[0][frame];
  }
}

static void start_streams(void)
{
  if(HAL_DMAEx_List_Start_IT(&handle_GPDMA1_Channel12) != HAL_OK)
  {
    Error_Handler();
  }
  hsai_BlockA1.Instance->CR1 |= SAI_xCR1_DMAEN;
  while((hsai_BlockA1.Instance->SR & SAI_xSR_FLVL) == 0)
  {
  }
  if(HAL_DMAEx_List_Start_IT(&handle_GPDMA1_Channel13) != HAL_OK)
  {
    Error_Handler();
  }
  hsai_BlockB1.Instance->CR1 |= SAI_xCR1_DMAEN;
  __HAL_SAI_ENABLE(&hsai_BlockB1);
  __HAL_SAI_ENABLE(&hsai_BlockA1);
}

static void codec_power(uint8_t on)
{
  if(HAL_I2C_Mem_Write(&hi2c1, CODEC_ADDRESS << 1, CODEC_POWER, I2C_MEMADD_SIZE_8BIT, &on, 1, 10)
     != HAL_OK)
  {
    Error_Handler();
  }
}

int main(void)
{
  HAL_Init();
  MX_SAI1_Clock_Init();
  MX_GPDMA1_Init();
  MX_I2C1_Init();
  slave_block(&hsai_BlockA1, SAI1_Block_A, SAI_MODESLAVE_TX, SAI_ASYNCHRONOUS);
  slave_block(&hsai_BlockB1, SAI1_Block_B, SAI_MODESLAVE_RX, SAI_SYNCHRONOUS);
  MX_SAI1_DMA_Init();
  fill_playback();
  start_streams();
  codec_power(1u);
  while(stream.rx_completes < BUFFERS)
  {
    __WFI();
  }
  codec_power(0u);
  codec_stopped();
  for(;;)
  {
    __WFI();
  }
}
