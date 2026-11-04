#include "main.h"

void SysTick_Handler(void);

static uint64_t const record[2] = {0x12345678C0FFEE00u, 0x0BADF00D9ABCDEF0u};

volatile uint32_t data_page;
volatile uint32_t data_address;
volatile uint32_t erase_status;
volatile uint32_t page_error;
volatile uint32_t program_status;
volatile uint32_t rdp_level;
volatile uint32_t user_config;
volatile uint32_t flash_kb;

__attribute__((noipa)) void clock_configured(void)
{}

__attribute__((noipa)) void page_erased(void)
{}

__attribute__((noipa)) void flash_done(void)
{}

__attribute__((noipa)) void options_read(void)
{}

__attribute__((noipa)) void set_latency(uint32_t latency)
{
  __HAL_FLASH_SET_LATENCY(latency);
}

__attribute__((noinline)) void Error_Handler(void)
{
  __disable_irq();
  for(;;)
  {
  }
}

static void SystemClock_Config(void)
{
  __HAL_RCC_PWR_CLK_ENABLE();
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
                .PLLM      = 1,
                .PLLN      = 10,
                .PLLP      = RCC_PLLP_DIV7,
                .PLLQ      = RCC_PLLQ_DIV2,
                .PLLR      = RCC_PLLR_DIV2,
                },
  };
  if(HAL_RCC_OscConfig(&oscillators) != HAL_OK)
  {
    Error_Handler();
  }
  RCC_ClkInitTypeDef clocks = {
      .ClockType =
          RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2,
      .SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK,
      .AHBCLKDivider  = RCC_SYSCLK_DIV1,
      .APB1CLKDivider = RCC_HCLK_DIV1,
      .APB2CLKDivider = RCC_HCLK_DIV1,
  };
  if(HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
  clock_configured();
}

static void program_page(void)
{
  data_page                    = FLASH_BANK_SIZE / FLASH_PAGE_SIZE - 1u;
  data_address                 = FLASH_BASE + FLASH_SIZE - FLASH_PAGE_SIZE;
  FLASH_EraseInitTypeDef erase = {
      .TypeErase = FLASH_TYPEERASE_PAGES,
      .Banks     = FLASH_BANK_2,
      .Page      = data_page,
      .NbPages   = 1,
  };
  uint32_t error = 0;
  HAL_FLASH_Unlock();
  erase_status = HAL_FLASHEx_Erase(&erase, &error);
  page_error   = error;
  page_erased();
  program_status = HAL_OK;
  for(uint32_t word = 0; word < 2u && program_status == HAL_OK; ++word)
  {
    program_status =
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, data_address + 8u * word, record[word]);
  }
  HAL_FLASH_Lock();
  flash_done();
}

static void read_options(void)
{
  FLASH_OBProgramInitTypeDef options = {.WRPArea = OB_WRPAREA_BANK1_AREAA};
  HAL_FLASHEx_OBGetConfig(&options);
  rdp_level   = options.RDPLevel;
  user_config = options.USERConfig;
  flash_kb    = *(uint16_t const volatile*)FLASHSIZE_BASE;
  options_read();
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  program_page();
  read_options();
  set_latency(__HAL_FLASH_GET_LATENCY());
  for(;;)
  {
    __WFI();
  }
}
