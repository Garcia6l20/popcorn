#include "periph_test.h"
#include "stm32u5xx.h"

#define HASH_MESSAGE_WORDS 76u
#define HASH_MAX_RESULTS   32u
#define HASH_TICK_CYCLES   4000u
#define HASH_ALGOS         4u
#define HASH_LENGTHS       5u
#define HASH_DMA_RESULTS   24u
#define HASH_DMA_REQUEST   89u
#define HMAC_KEY_BYTES     132u
#define HMAC_CASES         4u
#define HMAC_RESULTS       16u
#define HMAC_DMA_RESULTS   8u
#define SWAP_RESULTS       32u
#define SWAP_HASH_CSRS     38u
#define SWAP_HMAC_CSRS     54u
#define SWAP_KEY_BYTES     131u
#define DMA_SWAP_CASES     8u
#define DMA_SWAP_RESULTS   16u
#define DMA_SWAP_AT        21u
#define DMA_SWAP_TICKS     50u
#define SWAP_CR_SAVED \
  (HASH_CR_DMAE | HASH_CR_DATATYPE | HASH_CR_MODE | HASH_CR_ALGO | HASH_CR_LKEY | HASH_CR_MDMAT)

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
volatile uint32_t hmac_count;
uint32_t          hmac_digest[HMAC_RESULTS][8];
uint32_t          hmac_dinis_irqs[HMAC_RESULTS];
volatile uint32_t hmac_dma_count;
uint32_t          hmac_dma_digest[HMAC_DMA_RESULTS][8];
volatile uint32_t swap_count;
uint32_t          swap_digest[SWAP_RESULTS][8];
volatile uint32_t dma_swap_count;
uint32_t          dma_swap_digest[DMA_SWAP_RESULTS][8];
uint32_t          dma_swap_flags[DMA_SWAP_CASES];
uint32_t          dma_swap_words[DMA_SWAP_CASES];

static uint32_t          message[HASH_MESSAGE_WORDS];
static volatile uint32_t ticks;
static uint32_t const    lengths[HASH_LENGTHS] = {0u, 3u, 64u, 68u, 300u};
static uint32_t          hmac_key[HMAC_KEY_BYTES / 4u];
static uint32_t const    hmac_keys[HMAC_CASES]     = {20u, 21u, 64u, 131u};
static uint32_t const    hmac_messages[HMAC_CASES] = {0u, 68u, 300u, 50u};

static uint32_t swap_context[3u + SWAP_HMAC_CSRS];

static uint32_t const* volatile irq_words;
static volatile uint32_t irq_left;
static volatile uint32_t irq_bits;
static volatile uint32_t irq_first;
static volatile uint32_t irq_complete;
static volatile uint32_t irq_dinis;
static volatile uint32_t irq_dcis;
static volatile uint32_t irq_busy;
static volatile uint32_t irq_cycles;
static volatile uint32_t irq_phase;
static uint32_t          irq_key_bytes;
static uint32_t          irq_message_bytes;

void SysTick_Handler(void);
void HASH_IRQHandler(void);
void idle(void);
void hash_poll(void);
void hash_backtoback(void);
void hash_irq(void);
void hash_dma(void);
void hmac_poll(void);
void hmac_irq(void);
void hmac_dma(void);
void hash_swap(void);
void hash_dma_swap(void);

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
  uint8_t* const key = (uint8_t*)hmac_key;
  for(uint32_t index = 0; index < HMAC_KEY_BYTES; ++index)
  {
    key[index] = (uint8_t)(index * 53u + 7u);
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
  if(irq_phase == 0u || irq_phase == 3u)
  {
    return;
  }
  while((HASH->SR & HASH_SR_BUSY) != 0u)
  {
  }
  uint32_t const bytes = irq_phase == 1u ? irq_message_bytes : irq_key_bytes;
  irq_words            = irq_phase == 1u ? message : hmac_key;
  irq_left             = (bytes + 3u) / 4u;
  irq_bits             = (bytes % 4u) * 8u;
  irq_first            = 1;
  irq_phase            = irq_phase + 1u;
  HASH->IMR |= HASH_IMR_DINIE;
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
  irq_phase            = 0;
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

static void dma_start(uint32_t const* source, uint32_t bytes)
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
}

static void dma_wait(void)
{
  while((GPDMA1_Channel10->CCR & DMA_CCR_EN) != 0u)
  {
    __WFI();
  }
}

static void dma_words(uint32_t const* source, uint32_t bytes)
{
  dma_start(source, bytes);
  DMA_Channel_TypeDef* const channel = GPDMA1_Channel10;
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

static uint32_t hmac_control(uint32_t algo, uint32_t key_bytes)
{
  uint32_t const lkey = key_bytes > 64u ? HASH_CR_LKEY : 0u;
  return HASH_CR_MODE | lkey | (algo << HASH_CR_ALGO_Pos) | HASH_CR_DATATYPE_1;
}

static void hmac_record(uint32_t dinis_irqs)
{
  uint32_t const at = hmac_count;
  if(at >= HMAC_RESULTS)
  {
    FAIL(3);
  }
  for(uint32_t index = 0; index < 5u; ++index)
  {
    hmac_digest[at][index] = HASH->HR[index];
  }
  for(uint32_t index = 5; index < 8u; ++index)
  {
    hmac_digest[at][index] = HASH_DIGEST->HR[index];
  }
  hmac_dinis_irqs[at] = dinis_irqs;
  hmac_count          = at + 1u;
}

static void hmac_poll_phase(uint32_t const* words, uint32_t bytes)
{
  for(uint32_t index = 0; index < (bytes + 3u) / 4u; ++index)
  {
    wait_not_busy();
    HASH->DIN = words[index];
  }
  HASH->STR = (bytes % 4u) * 8u;
  HASH->STR |= HASH_STR_DCAL;
  wait_not_busy();
}

static void hmac_poll_run(uint32_t algo, uint32_t key_bytes, uint32_t message_bytes)
{
  HASH->CR = HASH_CR_INIT | hmac_control(algo, key_bytes);
  hmac_poll_phase(hmac_key, key_bytes);
  hmac_poll_phase(message, message_bytes);
  hmac_poll_phase(hmac_key, key_bytes);
  while((HASH->SR & HASH_SR_DCIS) == 0u)
  {
    __WFE();
  }
}

static void hmac_poll_one(uint32_t algo, uint32_t key_bytes, uint32_t message_bytes)
{
  hmac_poll_run(algo, key_bytes, message_bytes);
  hmac_record(0);
}

static void hmac_irq_one(uint32_t algo, uint32_t key_bytes, uint32_t message_bytes)
{
  irq_key_bytes     = key_bytes;
  irq_message_bytes = message_bytes;
  irq_words         = hmac_key;
  irq_left          = (key_bytes + 3u) / 4u;
  irq_bits          = (key_bytes % 4u) * 8u;
  irq_first         = 1;
  irq_complete      = 0;
  irq_dinis         = 0;
  irq_dcis          = 0;
  irq_phase         = 1;
  HASH->CR          = HASH_CR_INIT | hmac_control(algo, key_bytes);
  HASH->IMR         = HASH_IMR_DINIE | HASH_IMR_DCIE;
  while(irq_complete == 0u)
  {
    __WFI();
  }
  if(irq_dcis != 1u)
  {
    FAIL(4);
  }
  hmac_record(irq_dinis);
}

static void hmac_dma_phase(uint32_t const* words, uint32_t bytes, uint32_t control)
{
  HASH->STR = (bytes % 4u) * 8u;
  HASH->CR  = control | HASH_CR_DMAE;
  dma_words(words, (bytes + 3u) / 4u * 4u);
}

static void hmac_dma_one(uint32_t algo, uint32_t key_bytes, uint32_t message_bytes)
{
  uint32_t const control = hmac_control(algo, key_bytes);
  uint32_t const first   = (message_bytes / 32u) * 16u;
  hmac_dma_phase(hmac_key, key_bytes, control | HASH_CR_INIT);
  if(first != 0u)
  {
    HASH->CR = control | HASH_CR_DMAE | HASH_CR_MDMAT;
    dma_words(message, first);
  }
  hmac_dma_phase(message + first / 4u, message_bytes - first, control);
  hmac_dma_phase(hmac_key, key_bytes, control);
  while((HASH->SR & HASH_SR_DCIS) == 0u || (HASH->SR & HASH_SR_BUSY) != 0u)
  {
    __WFE();
  }
  uint32_t const at = hmac_dma_count;
  if(at >= HMAC_DMA_RESULTS)
  {
    FAIL(5);
  }
  for(uint32_t index = 0; index < 5u; ++index)
  {
    hmac_dma_digest[at][index] = HASH->HR[index];
  }
  for(uint32_t index = 5; index < 8u; ++index)
  {
    hmac_dma_digest[at][index] = HASH_DIGEST->HR[index];
  }
  hmac_dma_count = at + 1u;
}

static void swap_record(void)
{
  uint32_t const at = swap_count;
  if(at >= SWAP_RESULTS)
  {
    FAIL(6);
  }
  for(uint32_t index = 0; index < 5u; ++index)
  {
    swap_digest[at][index] = HASH->HR[index];
  }
  for(uint32_t index = 5; index < 8u; ++index)
  {
    swap_digest[at][index] = HASH_DIGEST->HR[index];
  }
  swap_count = at + 1u;
}

static void context_save(uint32_t csrs)
{
  wait_not_busy();
  while((HASH->SR & HASH_SR_DINIS) == 0u)
  {
    __WFE();
  }
  swap_context[0] = HASH->IMR & (HASH_IMR_DINIE | HASH_IMR_DCIE);
  swap_context[1] = HASH->STR & HASH_STR_NBLW;
  swap_context[2] = HASH->CR & SWAP_CR_SAVED;
  for(uint32_t index = 0; index < csrs; ++index)
  {
    swap_context[3u + index] = HASH->CSR[index];
  }
}

static void context_restore(uint32_t csrs)
{
  HASH->IMR = swap_context[0];
  HASH->STR = swap_context[1];
  HASH->CR  = swap_context[2];
  HASH->CR |= HASH_CR_INIT;
  for(uint32_t index = 0; index < csrs; ++index)
  {
    HASH->CSR[index] = swap_context[3u + index];
  }
}

static void poll_hash(uint32_t algo, uint32_t length)
{
  begin(algo);
  for(uint32_t index = 0; index < (length + 3u) / 4u; ++index)
  {
    wait_not_busy();
    HASH->DIN = message[index];
  }
  HASH->STR = (length % 4u) * 8u;
  HASH->STR |= HASH_STR_DCAL;
  while((HASH->SR & HASH_SR_DCIS) == 0u || (HASH->SR & HASH_SR_BUSY) != 0u)
  {
    __WFE();
  }
}

static void swap_poll(uint32_t algo, uint32_t length)
{
  poll_hash(algo, length);
  swap_record();
}

static void swap_hash(uint32_t algo)
{
  uint32_t const length = 300u;
  begin(algo);
  for(uint32_t index = 0; index < length / 4u; ++index)
  {
    wait_not_busy();
    HASH->DIN = message[index];
    if(index == 32u)
    {
      context_save(SWAP_HASH_CSRS);
      swap_poll((algo + 1u) % HASH_ALGOS, 68u);
      context_restore(SWAP_HASH_CSRS);
    }
  }
  HASH->STR = 0;
  HASH->STR |= HASH_STR_DCAL;
  while((HASH->SR & HASH_SR_DCIS) == 0u || (HASH->SR & HASH_SR_BUSY) != 0u)
  {
    __WFE();
  }
  swap_record();
}

static void swap_hmac(uint32_t algo, uint32_t suspended)
{
  HASH->CR = HASH_CR_INIT | hmac_control(algo, SWAP_KEY_BYTES);
  for(uint32_t phase = 0; phase < 3u; ++phase)
  {
    uint32_t const* const words = phase == 1u ? message : hmac_key;
    uint32_t const        bytes = phase == 1u ? 300u : SWAP_KEY_BYTES;
    for(uint32_t index = 0; index < (bytes + 3u) / 4u; ++index)
    {
      wait_not_busy();
      HASH->DIN = words[index];
      if(phase == suspended && index == 16u)
      {
        context_save(SWAP_HMAC_CSRS);
        hmac_poll_run((algo + 1u) % HASH_ALGOS, 21u, 68u);
        swap_record();
        context_restore(SWAP_HMAC_CSRS);
      }
    }
    HASH->STR = (bytes % 4u) * 8u;
    HASH->STR |= HASH_STR_DCAL;
    wait_not_busy();
  }
  while((HASH->SR & HASH_SR_DCIS) == 0u)
  {
    __WFE();
  }
  swap_record();
}

static void dma_swap_record(void)
{
  while((HASH->SR & HASH_SR_DCIS) == 0u || (HASH->SR & HASH_SR_BUSY) != 0u)
  {
    __WFE();
  }
  uint32_t const at = dma_swap_count;
  if(at >= DMA_SWAP_RESULTS)
  {
    FAIL(7);
  }
  for(uint32_t index = 0; index < 5u; ++index)
  {
    dma_swap_digest[at][index] = HASH->HR[index];
  }
  for(uint32_t index = 5; index < 8u; ++index)
  {
    dma_swap_digest[at][index] = HASH_DIGEST->HR[index];
  }
  dma_swap_count = at + 1u;
}

static uint32_t dma_remaining(void)
{
  DMA_Channel_TypeDef* const channel = GPDMA1_Channel10;
  return (channel->CBR1 & DMA_CBR1_BNDT) / 4u
       + ((channel->CSR & DMA_CSR_FIFOL) >> DMA_CSR_FIFOL_Pos);
}

static uint32_t wait_sr(uint32_t mask, uint32_t value)
{
  uint32_t const since = ticks;
  while((HASH->SR & mask) != value)
  {
    if(ticks - since > DMA_SWAP_TICKS)
    {
      return 0;
    }
  }
  return 1;
}

static uint32_t dma_suspend(uint32_t csrs, uint32_t words, uint32_t at)
{
  DMA_Channel_TypeDef* const channel = GPDMA1_Channel10;
  while(dma_remaining() > words - DMA_SWAP_AT)
  {
  }
  HASH->CR &= ~HASH_CR_DMAE;
  uint32_t flags = (HASH->SR & HASH_SR_DMAS) != 0u ? 1u : 0u;
  flags |= wait_sr(HASH_SR_DMAS, 0) << 1;
  uint32_t const remaining = dma_remaining();
  channel->CCR |= DMA_CCR_SUSP;
  while((channel->CSR & DMA_CSR_SUSPF) == 0u)
  {
  }
  channel->CCR |= DMA_CCR_RESET;
  channel->CFCR = 0x00007F00u;
  flags |= (HASH->SR & HASH_SR_DCIS) == 0u ? 4u : 0u;
  flags |= wait_sr(HASH_SR_DINIS, HASH_SR_DINIS) << 3;
  dma_swap_flags[at] = flags;
  dma_swap_words[at] = words - remaining;
  if(flags != 0xFu)
  {
    FAIL(8);
  }
  context_save(csrs);
  return remaining;
}

static void dma_resume(uint32_t csrs, uint32_t const* words, uint32_t total, uint32_t remaining)
{
  context_restore(csrs);
  dma_start(words + (total - remaining), 4u * remaining);
  HASH->CR |= HASH_CR_DMAE;
  dma_wait();
}

static void dma_swap_hash(uint32_t algo)
{
  uint32_t const words = 300u / 4u;
  HASH->STR            = 0;
  HASH->CR = HASH_CR_INIT | HASH_CR_DMAE | (algo << HASH_CR_ALGO_Pos) | HASH_CR_DATATYPE_1;
  dma_start(message, 4u * words);
  uint32_t const remaining = dma_suspend(SWAP_HASH_CSRS, words, 2u * algo);
  poll_hash((algo + 1u) % HASH_ALGOS, 68u);
  dma_swap_record();
  dma_resume(SWAP_HASH_CSRS, message, words, remaining);
  dma_swap_record();
}

static void dma_swap_hmac(uint32_t algo)
{
  uint32_t const words   = 300u / 4u;
  uint32_t const control = hmac_control(algo, SWAP_KEY_BYTES);
  hmac_dma_phase(hmac_key, SWAP_KEY_BYTES, control | HASH_CR_INIT);
  HASH->STR = 0;
  HASH->CR  = control | HASH_CR_DMAE;
  dma_start(message, 4u * words);
  uint32_t const remaining = dma_suspend(SWAP_HMAC_CSRS, words, 2u * algo + 1u);
  hmac_poll_run((algo + 1u) % HASH_ALGOS, 21u, 68u);
  dma_swap_record();
  dma_resume(SWAP_HMAC_CSRS, message, words, remaining);
  hmac_dma_phase(hmac_key, SWAP_KEY_BYTES, control);
  dma_swap_record();
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

void hmac_poll(void)
{
  start();
  for(uint32_t algo = 0; algo < HASH_ALGOS; ++algo)
  {
    for(uint32_t index = 0; index < HMAC_CASES; ++index)
    {
      hmac_poll_one(algo, hmac_keys[index], hmac_messages[index]);
    }
  }
  finish();
}

void hmac_irq(void)
{
  start();
  NVIC_EnableIRQ(HASH_IRQn);
  for(uint32_t algo = 0; algo < HASH_ALGOS; ++algo)
  {
    for(uint32_t index = 0; index < HMAC_CASES; ++index)
    {
      hmac_irq_one(algo, hmac_keys[index], hmac_messages[index]);
    }
  }
  finish();
}

void hmac_dma(void)
{
  start();
  RCC->AHB1ENR |= RCC_AHB1ENR_GPDMA1EN;
  (void)RCC->AHB1ENR;
  for(uint32_t algo = 0; algo < HASH_ALGOS; ++algo)
  {
    hmac_dma_one(algo, 20u, 68u);
    hmac_dma_one(algo, 131u, 300u);
  }
  finish();
}

void hash_swap(void)
{
  start();
  for(uint32_t algo = 0; algo < HASH_ALGOS; ++algo)
  {
    swap_hash(algo);
    for(uint32_t phase = 0; phase < 3u; ++phase)
    {
      swap_hmac(algo, phase);
    }
  }
  finish();
}

void hash_dma_swap(void)
{
  start();
  RCC->AHB1ENR |= RCC_AHB1ENR_GPDMA1EN;
  (void)RCC->AHB1ENR;
  for(uint32_t algo = 0; algo < HASH_ALGOS; ++algo)
  {
    dma_swap_hash(algo);
    dma_swap_hmac(algo);
  }
  finish();
}

int main(void)
{
  idle();
}
