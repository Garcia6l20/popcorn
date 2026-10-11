#include "periph_test.h"
#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_pwr.h"
#include "stm32u5xx_ll_rcc.h"
#include "stm32u5xx_ll_tim.h"
#include "stm32u5xx_ll_usart.h"

#define IRQ_MAX_STORES 32u
#define UART_BAUD      460800u
#define UART_BLOCK     512u
#define UART_PERIOD_MS 16u

volatile uint32_t irq_period_cycles;
volatile uint32_t irq_stores;
volatile uint32_t irq_count;
volatile uint32_t spins;
volatile uint32_t done;
volatile uint32_t irq_array[IRQ_MAX_STORES];

typedef struct
{
  uint32_t tx_bytes;
  uint32_t tx_left;
  uint32_t blocks;
} uart_port_state;

uart_port_state          uart_ports[2];
static volatile uint32_t uart_on;
static volatile uint32_t uart_ticks;

void TIM6_IRQHandler(void);
void SysTick_Handler(void);
void USART1_IRQHandler(void);
void USART2_IRQHandler(void);
void irq_rate(void);
void irq_wfi(void);
void irq_unmask(void);
void spin_store(void);
void uart_tx(void);

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
}

void TIM6_IRQHandler(void)
{
  LL_TIM_ClearFlag_UPDATE(TIM6);
  ++irq_count;
  uint32_t const stores = irq_stores < IRQ_MAX_STORES ? irq_stores : IRQ_MAX_STORES;
  for(uint32_t index = 0; index < stores; ++index)
  {
    irq_array[index] = irq_count + index;
  }
}

static void start_tim6(void)
{
  clock_160mhz();
  if(irq_period_cycles == 0u)
  {
    return;
  }
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
  LL_TIM_InitTypeDef config = {
      .Prescaler         = 0,
      .CounterMode       = LL_TIM_COUNTERMODE_UP,
      .Autoreload        = irq_period_cycles - 1u,
      .ClockDivision     = LL_TIM_CLOCKDIVISION_DIV1,
      .RepetitionCounter = 0,
  };
  LL_TIM_Init(TIM6, &config);
  LL_TIM_ClearFlag_UPDATE(TIM6);
  LL_TIM_EnableIT_UPDATE(TIM6);
  NVIC_EnableIRQ(TIM6_IRQn);
  LL_TIM_EnableCounter(TIM6);
}

void irq_rate(void)
{
  start_tim6();
  while(!done)
  {
  }
  DONE();
}

void spin_store(void)
{
  start_tim6();
  while(!done)
  {
    ++spins;
  }
  DONE();
}

void irq_wfi(void)
{
  start_tim6();
  for(;;)
  {
    __WFI();
  }
}

void irq_unmask(void)
{
  __disable_irq();
  start_tim6();
  for(;;)
  {
    while(!LL_TIM_IsActiveFlag_UPDATE(TIM6))
    {
    }
    __enable_irq();
    __disable_irq();
  }
}

void SysTick_Handler(void)
{
  ++uart_ticks;
}

static USART_TypeDef* uart_usart(uint32_t port)
{
  return port == 0u ? USART1 : USART2;
}

static void uart_irq(uint32_t port)
{
  USART_TypeDef* const   usart = uart_usart(port);
  uart_port_state* const state = &uart_ports[port];
  if(LL_USART_IsEnabledIT_TXE_TXFNF(usart) && LL_USART_IsActiveFlag_TXE_TXFNF(usart))
  {
    LL_USART_TransmitData8(usart, (uint8_t)(state->tx_bytes++ * 7u + port));
    if(--state->tx_left == 0u)
    {
      LL_USART_DisableIT_TXE_TXFNF(usart);
      ++state->blocks;
    }
  }
}

void USART1_IRQHandler(void)
{
  if(uart_on != 0u)
  {
    uart_irq(0);
  }
}

void USART2_IRQHandler(void)
{
  if(uart_on != 0u)
  {
    uart_irq(1);
  }
}

static void uart_block(uint32_t port)
{
  uart_ports[port].tx_left = UART_BLOCK;
  LL_USART_EnableIT_TXE_TXFNF(uart_usart(port));
}

static void uart_start(USART_TypeDef* usart, IRQn_Type irq)
{
  LL_USART_InitTypeDef config = {
      .PrescalerValue      = LL_USART_PRESCALER_DIV1,
      .BaudRate            = UART_BAUD,
      .DataWidth           = LL_USART_DATAWIDTH_8B,
      .StopBits            = LL_USART_STOPBITS_1,
      .Parity              = LL_USART_PARITY_NONE,
      .TransferDirection   = LL_USART_DIRECTION_TX_RX,
      .HardwareFlowControl = LL_USART_HWCONTROL_NONE,
      .OverSampling        = LL_USART_OVERSAMPLING_16,
  };
  LL_USART_Init(usart, &config);
  LL_USART_ConfigAsyncMode(usart);
  LL_USART_Enable(usart);
  while(!LL_USART_IsActiveFlag_TEACK(usart) || !LL_USART_IsActiveFlag_REACK(usart))
  {
  }
  LL_USART_EnableIT_RXNE_RXFNE(usart);
  LL_USART_EnableIT_ERROR(usart);
  NVIC_EnableIRQ(irq);
}

void uart_tx(void)
{
  clock_160mhz();
  LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART1_CLKSOURCE_PCLK2);
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
  LL_RCC_SetUSARTClockSource(LL_RCC_USART2_CLKSOURCE_PCLK1);
  uart_on = 1u;
  uart_start(USART1, USART1_IRQn);
  uart_start(USART2, USART2_IRQn);
  SysTick_Config(160000u);
  uint32_t next = 0u;
  for(;;)
  {
    while(uart_ticks < next)
    {
    }
    uart_block(0);
    uart_block(1);
    next += UART_PERIOD_MS;
  }
}

int main(void)
{
  irq_wfi();
}
