#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

#define REG(address) (*(volatile uint32_t*)(address))

#define RCC_AHB2ENR1 REG(0x46020C8Cu)
#define RCC_APB2ENR  REG(0x46020CA4u)

#define USART1_CR1 REG(0x40013800u)
#define USART1_BRR REG(0x4001380Cu)
#define USART1_ISR REG(0x4001381Cu)
#define USART1_RDR REG(0x40013824u)
#define USART1_TDR REG(0x40013828u)

#define GPIOA_MODER REG(0x42020000u)
#define GPIOA_IDR   REG(0x42020010u)
#define GPIOA_ODR   REG(0x42020014u)

#define NVIC_ISER0 REG(0xE000E100u)

#define ISR_RXFNE (1u << 5)
#define ISR_TXFNF (1u << 7)

volatile uint32_t pin_seen;
volatile uint32_t wwdg_entries;
volatile uint32_t echoed;

void WWDG_IRQHandler(void)
{
  wwdg_entries = wwdg_entries + 1u;
}

__attribute__((noipa)) void ready(void)
{
  __asm__ volatile("" ::: "memory");
}

static void init(void)
{
  RCC_AHB2ENR1 |= 1u;
  RCC_APB2ENR |= 1u << 14;
  GPIOA_MODER = (GPIOA_MODER & ~((3u << 0) | (3u << 10))) | (1u << 10);
  USART1_BRR  = 35u;
  USART1_CR1  = (1u << 3) | (1u << 2) | 1u;
  NVIC_ISER0  = 1u;
  __asm__ volatile("cpsie i" ::: "memory");
}

static void poll(void)
{
  uint32_t const level = GPIOA_IDR & 1u;
  pin_seen             = level;
  GPIOA_ODR            = (GPIOA_ODR & ~(1u << 5)) | (level << 5);
  if((USART1_ISR & ISR_RXFNE) != 0u)
  {
    uint32_t const data = USART1_RDR & 0xFFu;
    while((USART1_ISR & ISR_TXFNF) == 0u)
    {
    }
    USART1_TDR = data;
    echoed     = echoed + 1u;
  }
}

[[noreturn]] void Reset_Handler(void)
{
  for(uint32_t *from = _sidata, *to = _sdata; to < _edata;)
  {
    *to++ = *from++;
  }
  for(uint32_t* to = _sbss; to < _ebss;)
  {
    *to++ = 0;
  }
  init();
  ready();
  for(;;)
  {
    poll();
  }
}

__attribute__((section(".isr_vector"), used)) const void* const vectors[17] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [16] = (const void*)WWDG_IRQHandler,
};
