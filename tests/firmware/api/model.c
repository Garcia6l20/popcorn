#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];

#define NVIC_ISER0 (*(volatile uint32_t*)0xE000E100u)

#define TIMER_CR  (*(volatile uint32_t*)0x40030000u)
#define TIMER_ARR (*(volatile uint32_t*)0x40030004u)
#define TIMER_SR  (*(volatile uint32_t*)0x40030008u)
#define TIMER_CNT (*(volatile uint32_t*)0x4003000Cu)

#define EEPROM_CR (*(volatile uint32_t*)0x40040000u)
#define EEPROM_DR (*(volatile uint32_t*)0x40040004u)

#define EEPROM_START 1u
#define EEPROM_STOP  2u
#define EEPROM_WRITE 0xA0u
#define EEPROM_READ  0xA1u

#define UNMODELLED_DATA (*(volatile uint32_t*)0x40020000u)
#define UNMAPPED        (*(volatile uint32_t*)0x60000000u)

volatile uint32_t timer_irqs;
volatile uint32_t sink;

void TIMER_IRQHandler(void)
{
  TIMER_SR   = 0u;
  timer_irqs = timer_irqs + 1u;
}

__attribute__((noipa)) void start_timer(uint32_t period_us)
{
  TIMER_ARR = period_us;
  TIMER_CR  = 1u;
}

__attribute__((noipa)) void stop_timer(void)
{
  TIMER_CR = 0u;
}

__attribute__((noipa)) uint32_t wait_timer_irqs(uint32_t count)
{
  while(timer_irqs < count)
  {
    __asm__ volatile("wfi" ::: "memory");
  }
  return TIMER_CNT;
}

__attribute__((noipa)) uint32_t eeprom_roundtrip(uint32_t at, uint32_t word)
{
  EEPROM_CR = EEPROM_START;
  EEPROM_DR = EEPROM_WRITE;
  EEPROM_DR = at;
  for(uint32_t byte = 0; byte < 4u; ++byte)
  {
    EEPROM_DR = (word >> (8u * byte)) & 0xFFu;
  }
  EEPROM_CR = EEPROM_STOP;

  EEPROM_CR     = EEPROM_START;
  EEPROM_DR     = EEPROM_WRITE;
  EEPROM_DR     = at;
  EEPROM_CR     = EEPROM_START;
  EEPROM_DR     = EEPROM_READ;
  uint32_t read = 0;
  for(uint32_t byte = 0; byte < 4u; ++byte)
  {
    read |= (EEPROM_DR & 0xFFu) << (8u * byte);
  }
  EEPROM_CR = EEPROM_STOP;
  return read;
}

__attribute__((noipa)) uint32_t read_timer_count(void)
{
  return TIMER_CNT;
}

__attribute__((noipa)) void write_timer_cr(uint32_t value)
{
  TIMER_CR = value;
}

__attribute__((noipa)) uint32_t touch_unmodelled(void)
{
  return UNMODELLED_DATA;
}

__attribute__((noipa)) void write_then_fault(uint32_t value)
{
  TIMER_CR = value;
  sink     = UNMAPPED;
}

__attribute__((noipa)) void ready(void)
{
  __asm__ volatile("" ::: "memory");
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
  NVIC_ISER0 = 1u << 4;
  __asm__ volatile("cpsie i" ::: "memory");
  ready();
  for(;;)
  {
    __asm__ volatile("wfi" ::: "memory");
  }
}

__attribute__((section(".isr_vector"), used)) const void* const vectors[21] = {
    [0]  = &_estack,
    [1]  = (const void*)Reset_Handler,
    [20] = (const void*)TIMER_IRQHandler,
};
