#include "main.h"

#define WINDOW_LOW      1241u
#define WINDOW_HIGH     2482u
#define OVERSAMPLING_8X 7u

ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef handle_GPDMA1_Channel0;
TIM_HandleTypeDef htim15;
struct monitor    monitor;
uint16_t          samples[2u * SCAN_CHANNELS];

static DMA_NodeTypeDef  node;
static DMA_QListTypeDef queue;

static uint32_t const channels[SCAN_CHANNELS] = {
    ADC_CHANNEL_1,
    ADC_CHANNEL_2,
    ADC_CHANNEL_5,
    ADC_CHANNEL_6,
};

static uint32_t const ranks[SCAN_CHANNELS] = {
    ADC_REGULAR_RANK_1,
    ADC_REGULAR_RANK_2,
    ADC_REGULAR_RANK_3,
    ADC_REGULAR_RANK_4,
};

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

__attribute__((noipa)) void scanning(void)
{}

static void keep(uint16_t const* scan)
{
  for(uint32_t index = 0; index < SCAN_CHANNELS; ++index)
  {
    monitor.latest[index] = scan[index];
  }
  ++monitor.scans;
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef* hadc)
{
  (void)hadc;
  keep(&samples[0]);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc)
{
  (void)hadc;
  keep(&samples[SCAN_CHANNELS]);
}

void HAL_ADCEx_LevelOutOfWindow3Callback(ADC_HandleTypeDef* hadc)
{
  (void)hadc;
  ++monitor.alarms;
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef* hadc)
{
  (void)hadc;
  ++monitor.errors;
}

static void MX_ADC_Clock_Init(void)
{
  RCC_OscInitTypeDef oscillator = {
      .OscillatorType      = RCC_OSCILLATORTYPE_HSI,
      .HSIState            = RCC_HSI_ON,
      .HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT,
      .PLL                 = {.PLLState = RCC_PLL_NONE},
  };
  RCC_PeriphCLKInitTypeDef clock = {
      .PeriphClockSelection = RCC_PERIPHCLK_ADCDAC,
      .AdcDacClockSelection = RCC_ADCDACCLKSOURCE_HSI,
  };
  if(HAL_RCC_OscConfig(&oscillator) != HAL_OK || HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK)
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

static void MX_ADC1_Init(void)
{
  hadc1.Instance                                = ADC1;
  hadc1.Init.ClockPrescaler                     = ADC_CLOCK_ASYNC_DIV1;
  hadc1.Init.Resolution                         = ADC_RESOLUTION_12B;
  hadc1.Init.GainCompensation                   = 0;
  hadc1.Init.ScanConvMode                       = ADC_SCAN_ENABLE;
  hadc1.Init.EOCSelection                       = ADC_EOC_SEQ_CONV;
  hadc1.Init.LowPowerAutoWait                   = DISABLE;
  hadc1.Init.ContinuousConvMode                 = DISABLE;
  hadc1.Init.NbrOfConversion                    = SCAN_CHANNELS;
  hadc1.Init.DiscontinuousConvMode              = DISABLE;
  hadc1.Init.ExternalTrigConv                   = ADC_EXTERNALTRIG_T15_TRGO;
  hadc1.Init.ExternalTrigConvEdge               = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.ConversionDataManagement           = ADC_CONVERSIONDATA_DMA_CIRCULAR;
  hadc1.Init.Overrun                            = ADC_OVR_DATA_OVERWRITTEN;
  hadc1.Init.LeftBitShift                       = ADC_LEFTBITSHIFT_NONE;
  hadc1.Init.OversamplingMode                   = ENABLE;
  hadc1.Init.Oversampling.Ratio                 = OVERSAMPLING_8X;
  hadc1.Init.Oversampling.RightBitShift         = ADC_RIGHTBITSHIFT_3;
  hadc1.Init.Oversampling.TriggeredMode         = ADC_TRIGGEREDMODE_SINGLE_TRIGGER;
  hadc1.Init.Oversampling.OversamplingStopReset = ADC_REGOVERSAMPLING_CONTINUED_MODE;
  if(HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }
  if(HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
  for(uint32_t index = 0; index < SCAN_CHANNELS; ++index)
  {
    ADC_ChannelConfTypeDef channel = {
        .Channel      = channels[index],
        .Rank         = ranks[index],
        .SamplingTime = ADC_SAMPLETIME_68CYCLES,
        .SingleDiff   = ADC_SINGLE_ENDED,
        .OffsetNumber = ADC_OFFSET_NONE,
        .Offset       = 0,
    };
    if(HAL_ADC_ConfigChannel(&hadc1, &channel) != HAL_OK)
    {
      Error_Handler();
    }
  }
  ADC_AnalogWDGConfTypeDef watchdog = {
      .WatchdogNumber  = ADC_ANALOGWATCHDOG_3,
      .WatchdogMode    = ADC_ANALOGWATCHDOG_SINGLE_REG,
      .Channel         = ADC_CHANNEL_6,
      .ITMode          = ENABLE,
      .HighThreshold   = WINDOW_HIGH,
      .LowThreshold    = WINDOW_LOW,
      .FilteringConfig = ADC_AWD_FILTERING_NONE,
  };
  if(HAL_ADC_AnalogWDGConfig(&hadc1, &watchdog) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_ADC1_DMA_Init(void)
{
  DMA_NodeConfTypeDef config = {
      .NodeType = DMA_GPDMA_LINEAR_NODE,
      .Init =
          {
                 .Request               = GPDMA1_REQUEST_ADC1,
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
      .SrcAddress    = (uint32_t)&ADC1->DR,
      .DstAddress    = (uint32_t)samples,
      .DataSize      = sizeof samples,
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
  __HAL_LINKDMA(&hadc1, DMA_Handle, handle_GPDMA1_Channel0);
}

static void MX_TIM15_Init(void)
{
  htim15.Instance                = TIM15;
  htim15.Init.Prescaler          = 3999;
  htim15.Init.CounterMode        = TIM_COUNTERMODE_UP;
  htim15.Init.Period             = 9;
  htim15.Init.ClockDivision      = TIM_CLOCKDIVISION_DIV1;
  htim15.Init.RepetitionCounter  = 0;
  htim15.Init.AutoReloadPreload  = TIM_AUTORELOAD_PRELOAD_DISABLE;
  TIM_MasterConfigTypeDef master = {
      .MasterOutputTrigger = TIM_TRGO_UPDATE,
      .MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE,
  };
  if(HAL_TIM_Base_Init(&htim15)
     != HAL_OK
     || HAL_TIMEx_MasterConfigSynchronization(&htim15, &master)
     != HAL_OK)
  {
    Error_Handler();
  }
}

int main(void)
{
  HAL_Init();
  MX_ADC_Clock_Init();
  MX_GPDMA1_Init();
  MX_ADC1_Init();
  MX_ADC1_DMA_Init();
  MX_TIM15_Init();
  if(HAL_ADC_Start_DMA(&hadc1, (uint32_t const*)samples, 2u * SCAN_CHANNELS)
     != HAL_OK
     || HAL_TIM_Base_Start(&htim15)
     != HAL_OK)
  {
    Error_Handler();
  }
  scanning();
  for(;;)
  {
    __WFI();
  }
}
