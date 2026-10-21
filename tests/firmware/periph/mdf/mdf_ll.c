#include "periph_test.h"
#include "stm32u5xx.h"
#include "stm32u5xx_ll_dma.h"

#define MICS            2u
#define RING            128u
#define HALF            (RING / 2u)
#define LOGGED          4u
#define HALVES          8u
#define RESTART_HALVES  4u
#define IRQ_SAMPLES     64u
#define SHOTS           3u
#define SAD_RING        64u
#define SAD_FRAMES      30u
#define SAD_DETECTIONS  4u
#define CLEARED_ISR     0xFFFFFFFFu
#define MIC_STH         (0x1Fu << MDF_SITFCR_STH_Pos)
#define SPI_NORMAL      1u
#define LF_MASTER       0u
#define MANCHESTER      2u
#define SYNC_CONTINUOUS (2u << MDF_DFLTCR_ACQMOD_Pos)
#define SYNC_SINGLE     (3u << MDF_DFLTCR_ACQMOD_Pos)
#define MIC_CICR                     \
  ((0x2Cu << MDF_DFLTCICR_SCALE_Pos) \
   | (31u << MDF_DFLTCICR_MCICD_Pos) \
   | (5u << MDF_DFLTCICR_CICMOD_Pos))
#define MIC_HPFC (2u << MDF_DFLTRSFR_HPFC_Pos)
#define SAD_CICR ((15u << MDF_DFLTCICR_MCICD_Pos) | (5u << MDF_DFLTCICR_CICMOD_Pos))
#define SAD_HPFC (3u << MDF_DFLTRSFR_HPFC_Pos)
#define SAD_CR   ((2u << MDF_SADCR_FRSIZE_Pos) | (1u << MDF_SADCR_DATCAP_Pos))
#define SAD_CFGR                   \
  ((20u << MDF_SADCFGR_ANMIN_Pos)  \
   | (1u << MDF_SADCFGR_LFRNB_Pos) \
   | (3u << MDF_SADCFGR_ANSLP_Pos) \
   | (3u << MDF_SADCFGR_SNTHR_Pos))
#define CKGCR_MIC                 \
  ((4u << MDF_CKGCR_PROCDIV_Pos)  \
   | (1u << MDF_CKGCR_CCKDIV_Pos) \
   | (1u << MDF_CKGCR_TRGSRC_Pos) \
   | MDF_CKGCR_CCK0DIR            \
   | MDF_CKGCR_CCK0EN)
#define CKGCR_ADF                 \
  ((4u << MDF_CKGCR_PROCDIV_Pos)  \
   | (1u << MDF_CKGCR_CCKDIV_Pos) \
   | MDF_CKGCR_CCK0DIR            \
   | MDF_CKGCR_CCK0EN)
#define RING_LINK (DMA_CLLR_UB1 | DMA_CLLR_UDA | DMA_CLLR_ULL)
#define DMA_FLAGS   \
  (DMA_CFCR_TCF     \
   | DMA_CFCR_HTF   \
   | DMA_CFCR_DTEF  \
   | DMA_CFCR_ULEF  \
   | DMA_CFCR_USEF  \
   | DMA_CFCR_SUSPF \
   | DMA_CFCR_TOF)
#define SRAM4_RING ((uint32_t*)SRAM4_BASE)
#define SRAM4_NODE ((struct ring_node*)(SRAM4_BASE + 0x100u))

struct ring_node
{
  uint32_t br1;
  uint32_t dar;
  uint32_t llr;
};

uint16_t          ring[RING] __attribute__((aligned(4)));
volatile uint32_t halves;
volatile uint32_t done;
uint32_t          flags[MICS];
int32_t           samples[IRQ_SAMPLES];
volatile uint32_t sample_count;
volatile uint32_t sample_irqs;
uint32_t          shot_runs[SHOTS];
uint32_t          sad_levels[SAD_FRAMES];
volatile uint32_t sad_frames;
uint32_t          sad_detected[SAD_DETECTIONS];
volatile uint32_t sad_detections;
uint32_t          sad_state;

static struct ring_node node;

static MDF_Filter_TypeDef* const mics[MICS] = {MDF1_Filter0, MDF1_Filter1};

[[noreturn]] void idle(void);
[[noreturn]] void finish(void);

void starting(void);
void two_mics(void);
void two_mics_lf(void);
void two_mics_restart(void);
void independent_irq(void);
void single_shot(void);
void manchester(void);
void adf_sad(void);
void GPDMA1_Channel6_IRQHandler(void);
void MDF1_FLT0_IRQHandler(void);
void ADF1_IRQHandler(void);
int  main(void);

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

__attribute__((noinline)) void starting(void)
{
  __asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) void finish(void)
{
  done = 1u;
  idle();
}

static void kernel_clock(void)
{
  RCC->CR |= RCC_CR_HSION;
  while((RCC->CR & RCC_CR_HSIRDY) == 0)
  {
  }
  FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_1WS;
  RCC->CFGR1 = (RCC->CFGR1 & ~RCC_CFGR1_SW) | RCC_CFGR1_SW_0;
  while((RCC->CFGR1 & RCC_CFGR1_SWS) != RCC_CFGR1_SWS_0)
  {
  }
  RCC->AHB3ENR |= RCC_AHB3ENR_PWREN;
  (void)RCC->AHB3ENR;
  PWR->VOSR = (PWR->VOSR & ~PWR_VOSR_VOS) | PWR_VOSR_VOS_1;
  while((PWR->VOSR & PWR_VOSR_VOSRDY) == 0)
  {
  }
  RCC->PLL3CFGR =
      (2u << RCC_PLL3CFGR_PLL3SRC_Pos) | (3u << RCC_PLL3CFGR_PLL3M_Pos) | RCC_PLL3CFGR_PLL3QEN;
  RCC->PLL3DIVR = (127u << RCC_PLL3DIVR_PLL3N_Pos)
                | (1u << RCC_PLL3DIVR_PLL3P_Pos)
                | (24u << RCC_PLL3DIVR_PLL3Q_Pos)
                | (1u << RCC_PLL3DIVR_PLL3R_Pos);
  RCC->CR |= RCC_CR_PLL3ON;
  while((RCC->CR & RCC_CR_PLL3RDY) == 0)
  {
  }
}

static void mdf_clock(void)
{
  kernel_clock();
  RCC->CCIPR2 = (RCC->CCIPR2 & ~RCC_CCIPR2_MDF1SEL) | (2u << RCC_CCIPR2_MDF1SEL_Pos);
  RCC->AHB1ENR |= RCC_AHB1ENR_MDF1EN | RCC_AHB1ENR_GPDMA1EN;
  (void)RCC->AHB1ENR;
}

static void clock_generator(MDF_TypeDef* unit, uint32_t value)
{
  unit->CKGCR = 0;
  unit->CKGCR = value;
  unit->CKGCR |= MDF_CKGCR_CKDEN;
}

static void cleared(MDF_Filter_TypeDef* filter, uint32_t motor)
{
  filter->BSMXCR   = 0;
  filter->DFLTCR   = 0;
  filter->DFLTCICR = 0;
  filter->DFLTRSFR = 0;
  if(motor != 0)
  {
    filter->DFLTINTR = 0;
    filter->OLDCR    = 0;
  }
  filter->DLYCR = 0;
  if(motor != 0)
  {
    filter->SCDCR = 0;
  }
  filter->DFLTIER = 0;
  filter->DFLTISR = CLEARED_ISR;
}

static void mic_chain(MDF_Filter_TypeDef* filter, uint32_t cr)
{
  filter->DFLTCR   = cr;
  filter->DFLTCICR = MIC_CICR;
  filter->DFLTRSFR = 0;
  filter->DFLTRSFR |= MIC_HPFC;
}

static void serial(MDF_Filter_TypeDef* filter, uint32_t mode)
{
  filter->SITFCR = 0;
  filter->SITFCR = MIC_STH | (mode << MDF_SITFCR_SITFMOD_Pos);
}

static void ring_program(void)
{
  DMA_Channel_TypeDef* const channel = GPDMA1_Channel6;
  uint32_t const             link    = RING_LINK | ((uint32_t)&node & 0xFFFCu);
  node                               = (struct ring_node){4u * RING, (uint32_t)ring, link};
  channel->CTR1  = DMA_CTR1_SDW_LOG2_1 | DMA_CTR1_DDW_LOG2_0 | DMA_CTR1_DINC | DMA_CTR1_PAM_0;
  channel->CTR2  = LL_GPDMA1_REQUEST_MDF1_FLT0;
  channel->CBR1  = 4u * RING;
  channel->CSAR  = (uint32_t)&MDF1_Filter0->DFLTDR;
  channel->CDAR  = (uint32_t)ring;
  channel->CLBAR = (uint32_t)&node & 0xFFFF0000u;
  channel->CLLR  = link;
  channel->CFCR  = DMA_FLAGS;
  channel->CCR   = DMA_CCR_HTIE | DMA_CCR_TCIE;
}

static void ring_start(void)
{
  if((GPDMA1_Channel6->CCR & DMA_CCR_EN) == 0)
  {
    ring_program();
  }
  GPDMA1_Channel6->CCR |= DMA_CCR_EN;
}

static void channel_stop(DMA_Channel_TypeDef* channel)
{
  if((channel->CCR & DMA_CCR_EN) != 0)
  {
    channel->CCR |= DMA_CCR_SUSP;
    while((channel->CSR & DMA_CSR_SUSPF) == 0)
    {
    }
    channel->CCR |= DMA_CCR_RESET;
  }
  channel->CFCR = DMA_FLAGS;
}

void GPDMA1_Channel6_IRQHandler(void)
{
  uint32_t const status = GPDMA1_Channel6->CSR;
  uint32_t       first  = RING;
  if((status & DMA_CSR_HTF) != 0)
  {
    GPDMA1_Channel6->CFCR = DMA_CFCR_HTF;
    first                 = 0;
  }
  if((status & DMA_CSR_TCF) != 0)
  {
    GPDMA1_Channel6->CFCR = DMA_CFCR_TCF;
    first                 = HALF;
  }
  if(first == RING)
  {
    return;
  }
  for(uint32_t index = 0; index < LOGGED; ++index)
  {
    LOG(ring[first + index]);
  }
  ++halves;
}

static void pair_start(void)
{
  for(uint32_t f = 0; f < MICS; ++f)
  {
    mics[f]->SITFCR |= MDF_SITFCR_SITFEN;
    ring_start();
    mics[f]->DFLTCR |= MDF_DFLTCR_DFLTEN;
  }
  while((MDF1_Filter0->DFLTCR & MDF_DFLTCR_DFLTRUN) != 0)
  {
  }
  while((MDF1->GCR & MDF_GCR_TRGO) != 0)
  {
  }
  starting();
  MDF1->GCR |= MDF_GCR_TRGO;
}

static void pair_stop(void)
{
  for(uint32_t f = 0; f < MICS; ++f)
  {
    mics[f]->SITFCR &= ~MDF_SITFCR_SITFEN;
    channel_stop(GPDMA1_Channel6);
    mics[f]->DFLTCR &= ~MDF_DFLTCR_DFLTEN;
  }
}

static void pair_configure(uint32_t mode)
{
  mdf_clock();
  MDF1->GCR = 0;
  MDF1->GCR = 1u << MDF_GCR_ILVNB_Pos;
  clock_generator(MDF1, CKGCR_MIC);
  for(uint32_t f = 0; f < MICS; ++f)
  {
    serial(mics[f], mode);
  }
  for(uint32_t f = 0; f < MICS; ++f)
  {
    cleared(mics[f], 1u);
    mics[f]->BSMXCR = f + 1u;
    mic_chain(mics[f], SYNC_CONTINUOUS | MDF_DFLTCR_DMAEN);
    mics[f]->DFLTISR = CLEARED_ISR;
  }
  NVIC_EnableIRQ(GPDMA1_Channel6_IRQn);
}

static void wait_halves(uint32_t count)
{
  while(halves < count)
  {
    __WFI();
  }
}

static void pair_run(uint32_t mode)
{
  pair_configure(mode);
  pair_start();
  wait_halves(HALVES);
  for(uint32_t f = 0; f < MICS; ++f)
  {
    flags[f] = mics[f]->DFLTISR;
  }
  pair_stop();
  finish();
}

void two_mics(void)
{
  pair_run(SPI_NORMAL);
}

void two_mics_lf(void)
{
  pair_run(LF_MASTER);
}

void two_mics_restart(void)
{
  pair_configure(SPI_NORMAL);
  pair_start();
  wait_halves(RESTART_HALVES);
  pair_stop();
  pair_start();
  wait_halves(2u * RESTART_HALVES);
  pair_stop();
  finish();
}

void MDF1_FLT0_IRQHandler(void)
{
  if((MDF1_Filter0->DFLTISR & MDF_DFLTISR_FTHF) == 0)
  {
    return;
  }
  samples[sample_count++] = (int32_t)MDF1_Filter0->DFLTDR;
  samples[sample_count++] = (int32_t)MDF1_Filter0->DFLTDR;
  ++sample_irqs;
  if(sample_count >= IRQ_SAMPLES)
  {
    MDF1_Filter0->DFLTIER = 0;
  }
}

static void single_filter(uint32_t cr)
{
  mdf_clock();
  clock_generator(MDF1, CKGCR_MIC);
  serial(MDF1_Filter0, SPI_NORMAL);
  cleared(MDF1_Filter0, 1u);
  mic_chain(MDF1_Filter0, cr);
  MDF1_Filter0->SITFCR |= MDF_SITFCR_SITFEN;
}

static void filter_stop(void)
{
  MDF1_Filter0->SITFCR &= ~MDF_SITFCR_SITFEN;
  MDF1_Filter0->DFLTCR &= ~MDF_DFLTCR_DFLTEN;
}

void independent_irq(void)
{
  single_filter(MDF_DFLTCR_FTH);
  MDF1_Filter0->DFLTIER = MDF_DFLTIER_FTHIE;
  NVIC_EnableIRQ(MDF1_FLT0_IRQn);
  starting();
  MDF1_Filter0->DFLTCR |= MDF_DFLTCR_DFLTEN;
  while(sample_count < IRQ_SAMPLES)
  {
    __WFI();
  }
  filter_stop();
  finish();
}

void single_shot(void)
{
  single_filter(SYNC_SINGLE);
  MDF1_Filter0->DFLTCR |= MDF_DFLTCR_DFLTEN;
  starting();
  for(uint32_t shot = 0; shot < SHOTS; ++shot)
  {
    while((MDF1_Filter0->DFLTCR & MDF_DFLTCR_DFLTRUN) != 0)
    {
    }
    MDF1->GCR |= MDF_GCR_TRGO;
    while((MDF1_Filter0->DFLTISR & MDF_DFLTISR_RXNEF) == 0)
    {
    }
    samples[sample_count++] = (int32_t)MDF1_Filter0->DFLTDR;
    shot_runs[shot]         = MDF1_Filter0->DFLTCR & MDF_DFLTCR_DFLTRUN;
  }
  filter_stop();
  finish();
}

void manchester(void)
{
  mdf_clock();
  clock_generator(MDF1, CKGCR_MIC);
  serial(MDF1_Filter0, MANCHESTER);
  MDF1_Filter0->SITFCR |= MDF_SITFCR_SITFEN;
  finish();
}

void ADF1_IRQHandler(void)
{
  uint32_t const status = ADF1_Filter0->DFLTISR;
  if((status & MDF_DFLTISR_SDLVLF) != 0)
  {
    if(sad_frames < SAD_FRAMES)
    {
      sad_levels[sad_frames] = ADF1_Filter0->SADSDLVR;
    }
    ++sad_frames;
    ADF1_Filter0->DFLTISR = MDF_DFLTISR_SDLVLF;
  }
  if((status & MDF_DFLTISR_SDDETF) != 0)
  {
    if(sad_detections < SAD_DETECTIONS)
    {
      sad_detected[sad_detections] = sad_frames;
    }
    ++sad_detections;
    ADF1_Filter0->DFLTISR = MDF_DFLTISR_SDDETF;
  }
}

static void sad_ring(void)
{
  DMA_Channel_TypeDef* const channel = LPDMA1_Channel0;
  uint32_t const             link    = RING_LINK | ((uint32_t)SRAM4_NODE & 0xFFFCu);
  *SRAM4_NODE    = (struct ring_node){4u * SAD_RING, (uint32_t)SRAM4_RING, link};
  channel->CTR1  = DMA_CTR1_SDW_LOG2_1 | DMA_CTR1_DDW_LOG2_1 | DMA_CTR1_DINC;
  channel->CTR2  = LL_LPDMA1_REQUEST_ADF1_FLT0;
  channel->CBR1  = 4u * SAD_RING;
  channel->CSAR  = (uint32_t)&ADF1_Filter0->DFLTDR;
  channel->CDAR  = (uint32_t)SRAM4_RING;
  channel->CLBAR = (uint32_t)SRAM4_NODE & 0xFFFF0000u;
  channel->CLLR  = link;
  channel->CFCR  = DMA_FLAGS;
  channel->CCR   = DMA_CCR_EN;
}

void adf_sad(void)
{
  kernel_clock();
  RCC->CCIPR3 = (RCC->CCIPR3 & ~RCC_CCIPR3_ADF1SEL) | (2u << RCC_CCIPR3_ADF1SEL_Pos);
  RCC->AHB3ENR |= RCC_AHB3ENR_ADF1EN | RCC_AHB3ENR_LPDMA1EN;
  (void)RCC->AHB3ENR;
  for(uint32_t index = 0; index < SAD_RING; ++index)
  {
    SRAM4_RING[index] = 0;
  }
  clock_generator(ADF1, CKGCR_ADF);
  serial(ADF1_Filter0, LF_MASTER);
  cleared(ADF1_Filter0, 0u);
  ADF1_Filter0->DFLTCR   = MDF_DFLTCR_DMAEN;
  ADF1_Filter0->DFLTCICR = SAD_CICR;
  ADF1_Filter0->DFLTRSFR = SAD_HPFC;
  ADF1_Filter0->DFLTIER  = MDF_DFLTIER_SDDETIE | MDF_DFLTIER_SDLVLIE;
  ADF1_Filter0->SADCFGR  = SAD_CFGR;
  ADF1_Filter0->SADCR    = SAD_CR;
  ADF1_Filter0->SADCR |= MDF_SADCR_SADEN;
  sad_ring();
  NVIC_EnableIRQ(ADF1_IRQn);
  ADF1_Filter0->SITFCR |= MDF_SITFCR_SITFEN;
  starting();
  ADF1_Filter0->DFLTCR |= MDF_DFLTCR_DFLTEN;
  while(sad_frames < SAD_FRAMES)
  {
    __WFI();
  }
  sad_state = ADF1_Filter0->SADCR & MDF_SADCR_SADST;
  ADF1_Filter0->SITFCR &= ~MDF_SITFCR_SITFEN;
  ADF1_Filter0->DFLTCR &= ~MDF_DFLTCR_DFLTEN;
  channel_stop(LPDMA1_Channel0);
  finish();
}

int main(void)
{
  idle();
}
