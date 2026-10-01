#include "main.h"

SAI_HandleTypeDef hsai_BlockA1;
DMA_HandleTypeDef handle_GPDMA1_Channel0;
struct playback   playback;

int16_t const sine[2u * SINE_FRAMES] = {
    0,      0,      4277,   -4277,  8481,   -8481,  12539,  -12539, 16383,  -16383, 19947,  -19947,
    23170,  -23170, 25996,  -25996, 28377,  -28377, 30273,  -30273, 31650,  -31650, 32487,  -32487,
    32767,  -32767, 32487,  -32487, 31650,  -31650, 30273,  -30273, 28377,  -28377, 25996,  -25996,
    23170,  -23170, 19947,  -19947, 16383,  -16383, 12539,  -12539, 8481,   -8481,  4277,   -4277,
    0,      0,      -4277,  4277,   -8481,  8481,   -12539, 12539,  -16383, 16383,  -19947, 19947,
    -23170, 23170,  -25996, 25996,  -28377, 28377,  -30273, 30273,  -31650, 31650,  -32487, 32487,
    -32767, 32767,  -32487, 32487,  -31650, 31650,  -30273, 30273,  -28377, 28377,  -25996, 25996,
    -23170, 23170,  -19947, 19947,  -16384, 16384,  -12539, 12539,  -8481,  8481,   -4277,  4277,
};

static DMA_NodeTypeDef  node;
static DMA_QListTypeDef queue;

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef* hsai)
{
  (void)hsai;
  ++playback.halves;
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef* hsai)
{
  (void)hsai;
  ++playback.completes;
}

static void MX_SAI1_Clock_Init(void)
{
  RCC_PeriphCLKInitTypeDef clock = {
      .PeriphClockSelection = RCC_PERIPHCLK_SAI1,
      .Sai1ClockSelection   = RCC_SAI1CLKSOURCE_PLL2,
      .PLL2 =
          {
                 .PLL2Source   = RCC_PLLSOURCE_MSI,
                 .PLL2M        = 1,
                 .PLL2N        = 96,
                 .PLL2P        = 25,
                 .PLL2Q        = 2,
                 .PLL2R        = 2,
                 .PLL2RGE      = RCC_PLLVCIRANGE_0,
                 .PLL2FRACN    = 0,
                 .PLL2ClockOut = RCC_PLL2_DIVP,
                 },
  };
  if(HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_GPDMA1_Init(void)
{
  __HAL_RCC_GPDMA1_CLK_ENABLE();
  HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
}

static void MX_SAI1_Init(void)
{
  hsai_BlockA1.Instance            = SAI1_Block_A;
  hsai_BlockA1.Init.AudioMode      = SAI_MODEMASTER_TX;
  hsai_BlockA1.Init.Synchro        = SAI_ASYNCHRONOUS;
  hsai_BlockA1.Init.OutputDrive    = SAI_OUTPUTDRIVE_DISABLE;
  hsai_BlockA1.Init.NoDivider      = SAI_MASTERDIVIDER_DISABLE;
  hsai_BlockA1.Init.FIFOThreshold  = SAI_FIFOTHRESHOLD_EMPTY;
  hsai_BlockA1.Init.AudioFrequency = SAI_AUDIO_FREQUENCY_48K;
  hsai_BlockA1.Init.SynchroExt     = SAI_SYNCEXT_DISABLE;
  hsai_BlockA1.Init.MckOutput      = SAI_MCK_OUTPUT_DISABLE;
  hsai_BlockA1.Init.MonoStereoMode = SAI_STEREOMODE;
  hsai_BlockA1.Init.CompandingMode = SAI_NOCOMPANDING;
  hsai_BlockA1.Init.TriState       = SAI_OUTPUT_NOTRELEASED;
  if(HAL_SAI_InitProtocol(&hsai_BlockA1, SAI_I2S_STANDARD, SAI_PROTOCOL_DATASIZE_16BIT, 2)
     != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_SAI1_DMA_Init(void)
{
  DMA_NodeConfTypeDef config = {
      .NodeType = DMA_GPDMA_LINEAR_NODE,
      .Init =
          {
                 .Request               = GPDMA1_REQUEST_SAI1_A,
                 .BlkHWRequest          = DMA_BREQ_SINGLE_BURST,
                 .Direction             = DMA_MEMORY_TO_PERIPH,
                 .SrcInc                = DMA_SINC_INCREMENTED,
                 .DestInc               = DMA_DINC_FIXED,
                 .SrcDataWidth          = DMA_SRC_DATAWIDTH_HALFWORD,
                 .DestDataWidth         = DMA_DEST_DATAWIDTH_HALFWORD,
                 .SrcBurstLength        = 1,
                 .DestBurstLength       = 1,
                 .TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT0,
                 .TransferEventMode     = DMA_TCEM_BLOCK_TRANSFER,
                 .Mode                  = DMA_NORMAL,
                 },
      .DataHandlingConfig =
          {.DataExchange = DMA_EXCHANGE_NONE, .DataAlignment = DMA_DATA_RIGHTALIGN_ZEROPADDED},
      .TriggerConfig = {.TriggerPolarity = DMA_TRIG_POLARITY_MASKED},
      .SrcAddress    = (uint32_t)sine,
      .DstAddress    = (uint32_t)&SAI1_Block_A->DR,
      .DataSize      = sizeof sine,
  };
  if(HAL_DMAEx_List_BuildNode(&config, &node)
     != HAL_OK
     || HAL_DMAEx_List_InsertNode_Tail(&queue, &node)
     != HAL_OK
     || HAL_DMAEx_List_SetCircularMode(&queue)
     != HAL_OK)
  {
    Error_Handler();
  }
  handle_GPDMA1_Channel0.Instance       = GPDMA1_Channel0;
  handle_GPDMA1_Channel0.InitLinkedList = (DMA_InitLinkedListTypeDef){
      .Priority          = DMA_LOW_PRIORITY_LOW_WEIGHT,
      .LinkStepMode      = DMA_LSM_FULL_EXECUTION,
      .LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT0,
      .TransferEventMode = DMA_TCEM_BLOCK_TRANSFER,
      .LinkedListMode    = DMA_LINKEDLIST_CIRCULAR,
  };
  if(HAL_DMAEx_List_Init(&handle_GPDMA1_Channel0)
     != HAL_OK
     || HAL_DMAEx_List_LinkQ(&handle_GPDMA1_Channel0, &queue)
     != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(&hsai_BlockA1, hdmatx, handle_GPDMA1_Channel0);
}

int main(void)
{
  HAL_Init();
  MX_SAI1_Clock_Init();
  MX_GPDMA1_Init();
  MX_SAI1_Init();
  MX_SAI1_DMA_Init();
  if(HAL_SAI_Transmit_DMA(&hsai_BlockA1, (uint8_t*)sine, 2u * SINE_FRAMES) != HAL_OK)
  {
    Error_Handler();
  }
  for(;;)
  {
    __WFI();
  }
}
