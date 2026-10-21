#include "main.h"

#define PROC_DIVIDER     5u
#define CCK_DIVIDER      2u
#define CIC_DECIMATION   32u
#define GAIN_MINUS_12_DB (-4)
#define CLOCK_THRESHOLD  31u

MDF_HandleTypeDef mdf_filter0;
MDF_HandleTypeDef mdf_filter1;
DMA_HandleTypeDef handle_GPDMA1_Channel0;
struct capture    capture;
int16_t           ring[RING];

static DMA_NodeTypeDef  ring_node;
static DMA_QListTypeDef ring_queue;

static MDF_FilterConfigTypeDef const microphone = {
    .DataSource      = MDF_DATA_SOURCE_BSMX,
    .Delay           = 0,
    .CicMode         = MDF_ONE_FILTER_SINC5,
    .DecimationRatio = CIC_DECIMATION,
    .Offset          = 0,
    .Gain            = GAIN_MINUS_12_DB,
    .ReshapeFilter   = {.Activation = ENABLE, .DecimationRatio = MDF_RSF_DECIMATION_RATIO_4},
    .HighPassFilter  = {.Activation = ENABLE, .CutOffFrequency = MDF_HPF_CUTOFF_0_0025FPCM},
    .Integrator      = {.Activation = DISABLE},
    .SoundActivity   = {.Activation = DISABLE},
    .AcquisitionMode = MDF_MODE_SYNC_CONT,
    .FifoThreshold   = MDF_FIFO_THRESHOLD_NOT_EMPTY,
    .DiscardSamples  = 0,
    .Trigger         = {.Source = MDF_FILTER_TRIG_TRGO, .Edge = MDF_FILTER_TRIG_RISING_EDGE},
};

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void captured(void)
{}

void HAL_MDF_AcqHalfCpltCallback(MDF_HandleTypeDef* hmdf)
{
  (void)hmdf;
  ++capture.halves;
}

void HAL_MDF_AcqCpltCallback(MDF_HandleTypeDef* hmdf)
{
  (void)hmdf;
  ++capture.completes;
}

void HAL_MDF_ErrorCallback(MDF_HandleTypeDef* hmdf)
{
  ++capture.errors;
  capture.error_codes |= hmdf->ErrorCode;
}

static void SystemClock_Config(void)
{
  if(HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE2) != HAL_OK)
  {
    Error_Handler();
  }
  RCC_OscInitTypeDef oscillator = {
      .OscillatorType      = RCC_OSCILLATORTYPE_HSI,
      .HSIState            = RCC_HSI_ON,
      .HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT,
      .PLL                 = {.PLLState = RCC_PLL_NONE},
  };
  RCC_ClkInitTypeDef clocks = {
      .ClockType      = RCC_CLOCKTYPE_HCLK
                      | RCC_CLOCKTYPE_SYSCLK
                      | RCC_CLOCKTYPE_PCLK1
                      | RCC_CLOCKTYPE_PCLK2
                      | RCC_CLOCKTYPE_PCLK3,
      .SYSCLKSource   = RCC_SYSCLKSOURCE_HSI,
      .AHBCLKDivider  = RCC_SYSCLK_DIV1,
      .APB1CLKDivider = RCC_HCLK_DIV1,
      .APB2CLKDivider = RCC_HCLK_DIV1,
      .APB3CLKDivider = RCC_HCLK_DIV1,
  };
  if(HAL_RCC_OscConfig(&oscillator) != HAL_OK
     || HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_MDF1_Clock_Init(void)
{
  RCC_PeriphCLKInitTypeDef clock = {
      .PeriphClockSelection = RCC_PERIPHCLK_MDF1,
      .Mdf1ClockSelection   = RCC_MDF1CLKSOURCE_PLL3,
      .PLL3 =
          {
                 .PLL3Source   = RCC_PLLSOURCE_HSI,
                 .PLL3M        = 4,
                 .PLL3N        = 128,
                 .PLL3P        = 2,
                 .PLL3Q        = 25,
                 .PLL3R        = 2,
                 .PLL3RGE      = RCC_PLLVCIRANGE_0,
                 .PLL3FRACN    = 0,
                 .PLL3ClockOut = RCC_PLL3_DIVQ,
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

static void microphone_init(MDF_HandleTypeDef* hmdf, MDF_Filter_TypeDef* filter, uint32_t bitstream)
{
  hmdf->Instance                                = filter;
  hmdf->Init.CommonParam.InterleavedFilters     = 1;
  hmdf->Init.CommonParam.ProcClockDivider       = PROC_DIVIDER;
  hmdf->Init.CommonParam.OutputClock.Activation = ENABLE;
  hmdf->Init.CommonParam.OutputClock.Pins       = MDF_OUTPUT_CLOCK_0;
  hmdf->Init.CommonParam.OutputClock.Divider    = CCK_DIVIDER;
  hmdf->Init.CommonParam.OutputClock.Trigger    = (MDF_ClockTriggerTypeDef){.Activation = DISABLE};
  hmdf->Init.SerialInterface.Activation         = ENABLE;
  hmdf->Init.SerialInterface.Mode               = MDF_SITF_LF_MASTER_SPI_MODE;
  hmdf->Init.SerialInterface.ClockSource        = MDF_SITF_CCK0_SOURCE;
  hmdf->Init.SerialInterface.Threshold          = CLOCK_THRESHOLD;
  hmdf->Init.FilterBistream                     = bitstream;
  if(HAL_MDF_Init(hmdf) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_MDF1_Init(void)
{
  microphone_init(&mdf_filter0, MDF1_Filter0, MDF_BITSTREAM0_FALLING);
  microphone_init(&mdf_filter1, MDF1_Filter1, MDF_BITSTREAM1_RISING);
}

static void MX_MDF1_DMA_Init(void)
{
  DMA_NodeConfTypeDef node = {
      .NodeType = DMA_GPDMA_LINEAR_NODE,
      .Init =
          {
                 .Request               = GPDMA1_REQUEST_MDF1_FLT0,
                 .BlkHWRequest          = DMA_BREQ_SINGLE_BURST,
                 .Direction             = DMA_PERIPH_TO_MEMORY,
                 .SrcInc                = DMA_SINC_FIXED,
                 .DestInc               = DMA_DINC_INCREMENTED,
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
      .SrcAddress    = (uint32_t)&MDF1_Filter0->DFLTDR + 2u,
      .DstAddress    = (uint32_t)ring,
      .DataSize      = sizeof ring,
  };
  if(HAL_DMAEx_List_BuildNode(&node, &ring_node) != HAL_OK
     || HAL_DMAEx_List_InsertNode_Tail(&ring_queue, &ring_node) != HAL_OK
     || HAL_DMAEx_List_SetCircularMode(&ring_queue) != HAL_OK)
  {
    Error_Handler();
  }
  handle_GPDMA1_Channel0.Instance       = GPDMA1_Channel0;
  handle_GPDMA1_Channel0.InitLinkedList = (DMA_InitLinkedListTypeDef){
      .Priority          = DMA_HIGH_PRIORITY,
      .LinkStepMode      = DMA_LSM_FULL_EXECUTION,
      .LinkAllocatedPort = DMA_LINK_ALLOCATED_PORT0,
      .TransferEventMode = DMA_TCEM_BLOCK_TRANSFER,
      .LinkedListMode    = DMA_LINKEDLIST_CIRCULAR,
  };
  if(HAL_DMAEx_List_Init(&handle_GPDMA1_Channel0) != HAL_OK
     || HAL_DMAEx_List_LinkQ(&handle_GPDMA1_Channel0, &ring_queue) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(&mdf_filter0, hdma, handle_GPDMA1_Channel0);
}

static void start_microphones(void)
{
  MDF_DmaConfigTypeDef const dma = {
      .Address    = (uint32_t)ring,
      .DataLength = sizeof ring,
      .MsbOnly    = ENABLE,
  };
  if(HAL_MDF_AcqStart(&mdf_filter1, &microphone) != HAL_OK
     || HAL_MDF_AcqStart_DMA(&mdf_filter0, &microphone, &dma) != HAL_OK
     || HAL_MDF_GenerateTrgo(&mdf_filter0) != HAL_OK)
  {
    Error_Handler();
  }
}

static void stop_microphones(void)
{
  if(HAL_MDF_AcqStop_DMA(&mdf_filter0) != HAL_OK || HAL_MDF_AcqStop(&mdf_filter1) != HAL_OK)
  {
    Error_Handler();
  }
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_MDF1_Clock_Init();
  MX_GPDMA1_Init();
  MX_MDF1_Init();
  MX_MDF1_DMA_Init();
  start_microphones();
  while(capture.completes < COMPLETES)
  {
    __WFI();
  }
  stop_microphones();
  captured();
  for(;;)
  {
    __WFI();
  }
}
