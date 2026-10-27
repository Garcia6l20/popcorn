#include "main.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

#define VECTOR_COUNT (16u + (uint32_t)LPTIM6_IRQn + 1u)

SPI_HandleTypeDef   hspi4;
struct flash_result flash_result;
volatile uint32_t   ticker_runs;

extern uint32_t const g_pfnVectors[];

static uint32_t ram_vectors[VECTOR_COUNT] __attribute__((aligned(1024)));

static void relocate_vector_table(void)
{
  memcpy(ram_vectors, g_pfnVectors, sizeof ram_vectors);
  SCB->VTOR = (uint32_t)ram_vectors;
  __DSB();
}

static void SystemClock_Config(void)
{
  if(HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE0) != HAL_OK)
  {
    Error_Handler();
  }
  RCC_OscInitTypeDef oscillators = {
      .OscillatorType      = RCC_OSCILLATORTYPE_HSI,
      .HSIState            = RCC_HSI_ON,
      .HSIDiv              = RCC_HSI_DIV2,
      .HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT,
      .PLL =
          {
                .PLLState  = RCC_PLL_ON,
                .PLLSource = RCC_PLL1_SOURCE_HSI,
                .PLLM      = 2,
                .PLLN      = 31,
                .PLLP      = 2,
                .PLLQ      = 2,
                .PLLR      = 2,
                .PLLRGE    = RCC_PLL1_VCIRANGE_3,
                .PLLVCOSEL = RCC_PLL1_VCORANGE_WIDE,
                .PLLFRACN  = 2048,
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
  if(HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_GPIO_Init(void)
{
  __HAL_RCC_GPIOE_CLK_ENABLE();
}

static void MX_SPI4_Init(void)
{
  hspi4.Instance                     = SPI4;
  hspi4.Init.Mode                    = SPI_MODE_MASTER;
  hspi4.Init.Direction               = SPI_DIRECTION_2LINES;
  hspi4.Init.DataSize                = SPI_DATASIZE_8BIT;
  hspi4.Init.CLKPolarity             = SPI_POLARITY_LOW;
  hspi4.Init.CLKPhase                = SPI_PHASE_1EDGE;
  hspi4.Init.NSS                     = SPI_NSS_HARD_OUTPUT;
  hspi4.Init.BaudRatePrescaler       = SPI_BAUDRATEPRESCALER_8;
  hspi4.Init.FirstBit                = SPI_FIRSTBIT_MSB;
  hspi4.Init.TIMode                  = SPI_TIMODE_DISABLE;
  hspi4.Init.CRCCalculation          = SPI_CRCCALCULATION_DISABLE;
  hspi4.Init.CRCPolynomial           = 0x7;
  hspi4.Init.NSSPMode                = SPI_NSS_PULSE_DISABLE;
  hspi4.Init.NSSPolarity             = SPI_NSS_POLARITY_LOW;
  hspi4.Init.FifoThreshold           = SPI_FIFO_THRESHOLD_01DATA;
  hspi4.Init.MasterSSIdleness        = SPI_MASTER_SS_IDLENESS_00CYCLE;
  hspi4.Init.MasterInterDataIdleness = SPI_MASTER_INTERDATA_IDLENESS_00CYCLE;
  hspi4.Init.MasterReceiverAutoSusp  = SPI_MASTER_RX_AUTOSUSP_DISABLE;
  hspi4.Init.MasterKeepIOState       = SPI_MASTER_KEEP_IO_STATE_DISABLE;
  hspi4.Init.IOSwap                  = SPI_IO_SWAP_DISABLE;
  hspi4.Init.ReadyMasterManagement   = SPI_RDY_MASTER_MANAGEMENT_INTERNALLY;
  hspi4.Init.ReadyPolarity           = SPI_RDY_POLARITY_HIGH;
  if(HAL_SPI_Init(&hspi4) != HAL_OK)
  {
    Error_Handler();
  }
}

__attribute__((noipa)) void flash_page_program_sent(void)
{}

__attribute__((noipa)) void flash_page_program_done(void)
{}

__attribute__((noipa)) void flash_task_done(void)
{}

static void flash_task(void* argument)
{
  (void)argument;
  static uint8_t const read_id[4]      = {0x9F};
  static uint8_t const write_enable[1] = {0x06};
  static uint8_t const read_status[2]  = {0x05};
  static uint8_t       answer[4 + FLASH_PAGE_BYTES];
  static uint8_t       command[4 + FLASH_PAGE_BYTES] = {
      0x02, (FLASH_PAGE_ADDRESS >> 16) & 0xFF, (FLASH_PAGE_ADDRESS >> 8) & 0xFF,
      FLASH_PAGE_ADDRESS & 0xFF};

  spi_exchange(read_id, answer, sizeof read_id);
  memcpy(flash_result.jedec, &answer[1], sizeof flash_result.jedec);

  for(uint32_t index = 0; index < FLASH_PAGE_BYTES; ++index)
  {
    command[4 + index] = (uint8_t)(0xA0 + index);
  }
  spi_exchange(write_enable, answer, sizeof write_enable);
  spi_exchange(command, answer, sizeof command);
  flash_page_program_sent();
  do
  {
    spi_exchange(read_status, answer, sizeof read_status);
    ++flash_result.status_polls;
  } while((answer[1] & 0x01) != 0);
  flash_page_program_done();

  command[0] = 0x03;
  memset(&command[4], 0, FLASH_PAGE_BYTES);
  spi_exchange(command, answer, sizeof command);
  memcpy(flash_result.readback, &answer[4], FLASH_PAGE_BYTES);

  flash_result.done = 1;
  flash_task_done();
  vTaskDelete(NULL);
}

static void ticker_task(void* argument)
{
  (void)argument;
  for(;;)
  {
    ++ticker_runs;
    vTaskDelay(1);
  }
}

__attribute__((noinline)) void vAssertCalled(char const* file, int line)
{
  (void)file;
  (void)line;
  taskDISABLE_INTERRUPTS();
  for(;;)
  {
  }
}

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef* htim)
{
  if(htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
}

int main(void)
{
  relocate_vector_table();
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_SPI4_Init();
  if(xTaskCreate(flash_task, "flash", 512, NULL, 2, NULL) != pdPASS
     || xTaskCreate(ticker_task, "ticker", configMINIMAL_STACK_SIZE, NULL, 1, NULL) != pdPASS)
  {
    Error_Handler();
  }
  vTaskStartScheduler();
  Error_Handler();
}
