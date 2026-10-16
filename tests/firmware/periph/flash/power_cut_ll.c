#include "periph_test.h"
#include "stm32u5xx.h"

#define POLICY    (*(uint32_t const volatile*)0x080FC000u)
#define SCENARIO  (*(uint32_t const volatile*)0x080FC010u)
#define LOG_PAGE  0x080FE000u
#define SLOT_SIZE 128u
#define SLOTS     64u
#define MAGIC     0x50444D53u
#define VERSION   7u
#define NSKEY1    0x45670123u
#define NSKEY2    0xcdef89abu

uint32_t nmi_count;
uint32_t ecc_address[4];

static uint32_t const record[32] = {MAGIC, VERSION, ~VERSION, 0xFFFFFFFFu};

void     NMI_Handler(void);
void     append(uint32_t slot, uint32_t const* words);
void     stored(void);
void     erase_log(void);
void     erasing(void);
uint32_t scan(void);

void NMI_Handler(void)
{
  if(nmi_count < 4u)
  {
    ecc_address[nmi_count] = FLASH->ECCR & (FLASH_ECCR_ADDR_ECC | FLASH_ECCR_BK_ECC);
  }
  ++nmi_count;
  if(POLICY == 1u)
  {
    SET_BIT(FLASH->ECCR, FLASH_ECCR_ECCD);
  }
}

static void unlock(void)
{
  if(READ_BIT(FLASH->NSCR, FLASH_NSCR_LOCK) != 0u)
  {
    FLASH->NSKEYR = NSKEY1;
    FLASH->NSKEYR = NSKEY2;
  }
}

static void wait_idle(void)
{
  while(READ_BIT(FLASH->NSSR, FLASH_NSSR_BSY) != 0u)
  {
  }
}

__attribute__((noipa)) void stored(void)
{
  __asm__ volatile("" ::: "memory");
}

__attribute__((noipa)) void erasing(void)
{
  __asm__ volatile("" ::: "memory");
}

__attribute__((noipa)) void append(uint32_t slot, uint32_t const* words)
{
  uint32_t volatile* const to = (uint32_t volatile*)(LOG_PAGE + SLOT_SIZE * slot);
  unlock();
  __disable_irq();
  SET_BIT(FLASH->NSCR, FLASH_NSCR_PG | FLASH_NSCR_BWR);
  for(uint32_t i = 0; i < 32u; ++i)
  {
    to[i] = words[i];
  }
  stored();
  CLEAR_BIT(FLASH->NSCR, FLASH_NSCR_PG | FLASH_NSCR_BWR);
  wait_idle();
  __enable_irq();
}

__attribute__((noipa)) void erase_log(void)
{
  unlock();
  FLASH->NSCR = FLASH_NSCR_PER | FLASH_NSCR_BKER | (63u << FLASH_NSCR_PNB_Pos);
  SET_BIT(FLASH->NSCR, FLASH_NSCR_STRT);
  erasing();
  wait_idle();
  CLEAR_BIT(FLASH->NSCR, FLASH_NSCR_PER | FLASH_NSCR_BKER | FLASH_NSCR_PNB);
}

__attribute__((noipa)) uint32_t scan(void)
{
  uint32_t valid = 0;
  for(uint32_t slot = 0; slot < SLOTS; ++slot)
  {
    uint32_t const volatile* const at = (uint32_t const volatile*)(LOG_PAGE + SLOT_SIZE * slot);
    uint32_t                       copy[4];
    for(uint32_t word = 0; word < 4u; ++word)
    {
      copy[word] = at[word];
    }
    if(copy[0] == MAGIC && copy[2] == ~copy[1])
    {
      ++valid;
    }
  }
  return valid;
}

int main(void)
{
  if(SCENARIO == 1u)
  {
    append(0, record);
  }
  if(SCENARIO == 2u)
  {
    erase_log();
  }
  if(SCENARIO == 3u)
  {
    for(uint32_t pass = 0; pass < 1000u; ++pass)
    {
      (void)scan();
    }
  }
  LOG(scan());
  DONE();
}
