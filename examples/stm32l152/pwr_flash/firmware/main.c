#include "main.h"

void SysTick_Handler(void);

__attribute__((aligned(256), used)) static uint32_t const data_page[PAGE_WORDS] = {
    0xA5A5A5A5u,
    0x5A5A5A5Au,
    0xA5A5A5A5u,
    0x5A5A5A5Au,
};

static uint32_t const record[4] = {0xC0FFEE00u, 0x12345678u, 0x9ABCDEF0u, 0x0BADF00Du};

volatile uint32_t boot_count;
volatile uint32_t erase_status;
volatile uint32_t page_error;
volatile uint32_t program_status;

__attribute__((noipa)) void clock_configured(void)
{}

__attribute__((noipa)) void eeprom_counted(void)
{}

__attribute__((noipa)) void page_erased(void)
{}

__attribute__((noipa)) void flash_done(void)
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
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  while(__HAL_PWR_GET_FLAG(PWR_FLAG_VOS))
  {
  }
  RCC_OscInitTypeDef oscillators = {
      .OscillatorType      = RCC_OSCILLATORTYPE_HSI,
      .HSIState            = RCC_HSI_ON,
      .HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT,
      .PLL =
          {
                .PLLState  = RCC_PLL_ON,
                .PLLSource = RCC_PLLSOURCE_HSI,
                .PLLMUL    = RCC_PLL_MUL6,
                .PLLDIV    = RCC_PLL_DIV3,
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
  if(HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
  clock_configured();
}

static void count_boot(void)
{
  uint32_t const counter = FLASH_EEPROM_BASE;
  boot_count             = *(__IO uint32_t const*)counter + 1u;
  HAL_FLASHEx_DATAEEPROM_Unlock();
  if(HAL_FLASHEx_DATAEEPROM_Program(FLASH_TYPEPROGRAMDATA_WORD, counter, boot_count) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_FLASHEx_DATAEEPROM_Lock();
  eeprom_counted();
}

static void program_page(void)
{
  uint32_t const         page  = (uint32_t)data_page;
  FLASH_EraseInitTypeDef erase = {
      .TypeErase   = FLASH_TYPEERASE_PAGES,
      .PageAddress = page,
      .NbPages     = 1,
  };
  uint32_t error = 0;
  HAL_FLASH_Unlock();
  erase_status = HAL_FLASHEx_Erase(&erase, &error);
  page_error   = error;
  page_erased();
  program_status = HAL_OK;
  for(uint32_t word = 0; word < 4u && program_status == HAL_OK; ++word)
  {
    program_status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, page + 4u * word, record[word]);
  }
  HAL_FLASH_Lock();
  flash_done();
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  count_boot();
  program_page();
  set_latency(__HAL_FLASH_GET_LATENCY());
  for(;;)
  {
    __WFI();
  }
}
