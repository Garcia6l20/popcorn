#include "periph_test.h"
#include "stm32u5xx.h"

#define HASH_MESSAGE_WORDS 76u
#define HASH_MAX_RESULTS   32u
#define HASH_TICK_CYCLES   4000u
#define HASH_ALGOS         4u
#define HASH_LENGTHS       5u
#define HASH_DMA_RESULTS   24u
#define HASH_DMA_REQUEST   89u

volatile uint32_t hash_done;
volatile uint32_t hash_count;
uint32_t          hash_digest[HASH_MAX_RESULTS][8];
uint32_t          hash_cycles[HASH_MAX_RESULTS];
uint32_t          hash_ticks[HASH_MAX_RESULTS];
uint32_t          hash_dinis_irqs[HASH_MAX_RESULTS];
uint32_t          hash_dcis_irqs[HASH_MAX_RESULTS];
uint32_t          hash_busy_feeds[HASH_MAX_RESULTS];
volatile uint32_t hash_dma_count;
uint32_t          hash_dma_digest[HASH_DMA_RESULTS][8];
uint32_t          hash_dma_control[HASH_DMA_RESULTS];

static uint32_t          message[HASH_MESSAGE_WORDS];
static volatile uint32_t ticks;
static uint32_t const    lengths[HASH_LENGTHS] = {0u, 3u, 64u, 68u, 300u};

static uint32_t const* volatile irq_words;
static volatile uint32_t irq_left;
static volatile uint32_t irq_bits;
static volatile uint32_t irq_first;
static volatile uint32_t irq_complete;
static volatile uint32_t irq_dinis;
static volatile uint32_t irq_dcis;
static volatile uint32_t irq_busy;
static volatile uint32_t irq_cycles;

void SysTick_Handler(void);
void HASH_IRQHandler(void);
void idle(void);
void hash_poll(void);
void hash_backtoback(void);
void hash_irq(void);
void hash_dma(void);

void SysTick_Handler(void)
{
  ++ticks;
}

static void start(void)
{
  uint8_t* const bytes = (uint8_t*)message;
  for(uint32_t index = 0; index < 4u * HASH_MESSAGE_WORDS; ++index)
  {
    bytes[index] = (uint8_t)(index * 37u + 11u);
  }
  RCC->AHB2ENR1 |= RCC_AHB2ENR1_HASHEN;
  (void)RCC->AHB2ENR1;
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  SysTick_Config(HASH_TICK_CYCLES);
}

static void finish(void)
{
  SysTick->CTRL = 0;
  HASH->IMR     = 0;
  hash_done     = 1;
  for(;;)
  {
    __WFI();
  }
}

static void begin(uint32_t algo)
{
  HASH->CR = HASH_CR_INIT | (algo << HASH_CR_ALGO_Pos) | HASH_CR_DATATYPE_1;
}

static void record(uint32_t cycles, uint32_t since)
{
  uint32_t const at = hash_count;
  if(at >= HASH_MAX_RESULTS)
  {
    FAIL(1);
  }
  for(uint32_t index = 0; index < 5u; ++index)
  {
    hash_digest[at][index] = HASH->HR[index];
  }
  for(uint32_t index = 5; index < 8u; ++index)
  {
    hash_digest[at][index] = HASH_DIGEST->HR[index];
  }
  hash_cycles[at] = cycles;
  hash_ticks[at]  = ticks - since;
  hash_count      = at + 1u;
}

static void wait_not_busy(void)
{
  while((HASH->SR & HASH_SR_BUSY) != 0u)
  {
    __WFE();
  }
}

static void poll_one(uint32_t algo, uint32_t length)
{
  uint32_t const since = ticks;
  uint32_t const full  = length / 4u;
  uint32_t const rest  = length % 4u;
  HASH->CR             = HASH_CR_INIT;
  HASH->CR &= ~HASH_CR_ALGO;
  HASH->CR |= (algo << HASH_CR_ALGO_Pos) | HASH_CR_INIT | HASH_CR_DATATYPE_1;
  for(uint32_t index = 0; index < full; ++index)
  {
    wait_not_busy();
    HASH->DIN = message[index];
  }
  HASH->STR = (HASH->STR & ~HASH_STR_NBLW) | (rest * 8u);
  if(rest != 0u)
  {
    wait_not_busy();
    HASH->DIN = message[full];
  }
  HASH->STR |= HASH_STR_DCAL;
  while((HASH->SR & HASH_SR_DCIS) == 0u || (HASH->SR & HASH_SR_BUSY) != 0u)
  {
    __WFE();
  }
  record(DWT->CYCCNT, since);
}

static void backtoback_one(uint32_t algo, uint32_t length)
{
  uint32_t const since = ticks;
  uint32_t const words = (length + 3u) / 4u;
  begin(algo);
  for(uint32_t index = 0; index < words; ++index)
  {
    HASH->DIN = message[index];
  }
  HASH->STR = (length % 4u) * 8u;
  HASH->STR = HASH_STR_DCAL;
  while((HASH->SR & HASH_SR_DCIS) == 0u)
  {
  }
  record(DWT->CYCCNT, since);
}

static void irq_feed(void)
{
  uint32_t left = irq_left;
  if(left > 16u)
  {
    uint32_t const count = irq_first != 0u ? 17u : 16u;
    irq_first            = 0;
    for(uint32_t index = 0; index < count; ++index)
    {
      HASH->DIN = *irq_words;
      irq_words = irq_words + 1;
    }
    irq_left = left - count;
    return;
  }
  HASH->IMR &= ~HASH_IMR_DINIE;
  for(; left > 0u; --left)
  {
    HASH->DIN = *irq_words;
    irq_words = irq_words + 1;
  }
  irq_left  = 0;
  HASH->STR = irq_bits;
  HASH->STR = HASH_STR_DCAL;
}

void HASH_IRQHandler(void)
{
  uint32_t const status = HASH->SR;
  if((status & HASH_SR_DCIS) != 0u)
  {
    irq_cycles = DWT->CYCCNT;
    ++irq_dcis;
    HASH->IMR    = 0;
    irq_complete = 1;
    return;
  }
  if((status & HASH_SR_DINIS) != 0u && (HASH->IMR & HASH_IMR_DINIE) != 0u)
  {
    ++irq_dinis;
    if((status & HASH_SR_BUSY) != 0u)
    {
      ++irq_busy;
    }
    irq_feed();
  }
}

static void irq_one(uint32_t algo, uint32_t length)
{
  uint32_t const since = ticks;
  irq_words            = message;
  irq_left             = (length + 3u) / 4u;
  irq_bits             = (length % 4u) * 8u;
  irq_first            = 1;
  irq_complete         = 0;
  irq_dinis            = 0;
  irq_dcis             = 0;
  irq_busy             = 0;
  begin(algo);
  HASH->IMR = HASH_IMR_DINIE | HASH_IMR_DCIE;
  while(irq_complete == 0u)
  {
    __WFI();
  }
  hash_dinis_irqs[hash_count] = irq_dinis;
  hash_dcis_irqs[hash_count]  = irq_dcis;
  hash_busy_feeds[hash_count] = irq_busy;
  record(irq_cycles, since);
}

static void dma_words(uint32_t const* source, uint32_t bytes)
{
  DMA_Channel_TypeDef* const channel = GPDMA1_Channel10;
  channel->CFCR                      = 0x00007F00u;
  channel->CTR1                      = DMA_CTR1_SINC | DMA_CTR1_SDW_LOG2_1 | DMA_CTR1_DDW_LOG2_1;
  channel->CTR2                      = DMA_CTR2_DREQ | HASH_DMA_REQUEST;
  channel->CBR1                      = bytes;
  channel->CSAR                      = (uint32_t)source;
  channel->CDAR                      = (uint32_t)&HASH->DIN;
  channel->CLLR                      = 0;
  channel->CCR                       = DMA_CCR_EN;
  while((channel->CCR & DMA_CCR_EN) != 0u)
  {
    __WFI();
  }
}

static void dma_record(void)
{
  while((HASH->SR & HASH_SR_DCIS) == 0u || (HASH->SR & HASH_SR_BUSY) != 0u)
  {
    __WFE();
  }
  uint32_t const at = hash_dma_count;
  if(at >= HASH_DMA_RESULTS)
  {
    FAIL(2);
  }
  for(uint32_t index = 0; index < 5u; ++index)
  {
    hash_dma_digest[at][index] = HASH->HR[index];
  }
  for(uint32_t index = 5; index < 8u; ++index)
  {
    hash_dma_digest[at][index] = HASH_DIGEST->HR[index];
  }
  hash_dma_control[at] = HASH->CR;
  hash_dma_count       = at + 1u;
}

static void dma_single(uint32_t algo, uint32_t length)
{
  HASH->STR = 0;
  HASH->CR  = HASH_CR_INIT | HASH_CR_DMAE | (algo << HASH_CR_ALGO_Pos) | HASH_CR_DATATYPE_1;
  dma_words(message, length);
  HASH->STR |= HASH_STR_DCAL;
  dma_record();
}

static void dma_partial(uint32_t algo, uint32_t length)
{
  HASH->STR = (length % 4u) * 8u;
  HASH->CR  = HASH_CR_INIT | HASH_CR_DMAE | (algo << HASH_CR_ALGO_Pos) | HASH_CR_DATATYPE_1;
  dma_words(message, (length + 3u) / 4u * 4u);
  dma_record();
}

static void dma_split(uint32_t algo, uint32_t length)
{
  uint32_t const first = (length / 32u) * 16u;
  HASH->STR            = 0;
  HASH->CR =
      HASH_CR_INIT | HASH_CR_DMAE | HASH_CR_MDMAT | (algo << HASH_CR_ALGO_Pos) | HASH_CR_DATATYPE_1;
  dma_words(message, first);
  HASH->CR &= ~HASH_CR_MDMAT;
  HASH->CR |= HASH_CR_DMAE;
  dma_words(message + first / 4u, length - first);
  dma_record();
}

void idle(void)
{
  for(;;)
  {
  }
}

void hash_poll(void)
{
  start();
  for(uint32_t algo = 0; algo < HASH_ALGOS; ++algo)
  {
    for(uint32_t index = 0; index < HASH_LENGTHS; ++index)
    {
      poll_one(algo, lengths[index]);
    }
  }
  finish();
}

void hash_backtoback(void)
{
  start();
  for(uint32_t algo = 0; algo < HASH_ALGOS; ++algo)
  {
    for(uint32_t index = 0; index < HASH_LENGTHS; ++index)
    {
      backtoback_one(algo, lengths[index]);
    }
  }
  finish();
}

void hash_irq(void)
{
  start();
  NVIC_EnableIRQ(HASH_IRQn);
  for(uint32_t algo = 0; algo < HASH_ALGOS; ++algo)
  {
    for(uint32_t index = 0; index < HASH_LENGTHS; ++index)
    {
      irq_one(algo, lengths[index]);
    }
  }
  finish();
}

void hash_dma(void)
{
  start();
  RCC->AHB1ENR |= RCC_AHB1ENR_GPDMA1EN;
  (void)RCC->AHB1ENR;
  for(uint32_t algo = 0; algo < HASH_ALGOS; ++algo)
  {
    dma_single(algo, 68u);
    dma_single(algo, 300u);
    dma_split(algo, 68u);
    dma_split(algo, 300u);
    dma_partial(algo, 67u);
    dma_partial(algo, 66u);
  }
  finish();
}

int main(void)
{
  idle();
}
