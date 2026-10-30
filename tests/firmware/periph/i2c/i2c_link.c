#include "i2c_link.h"

#include "stm32u5xx_ll_bus.h"
#include "stm32u5xx_ll_i2c.h"

#define LINK_MAGIC       0x12C11Bu
#define LINK_CONTROLLER  1u
#define LINK_TARGET      2u
#define LINK_WRITE       1u
#define LINK_READ        2u
#define LINK_WRITE_SR    3u
#define LINK_WAIT_US     4u
#define LINK_RESET       5u
#define LINK_RCC         6u
#define LINK_STAMP       7u
#define LINK_REPEAT      8u
#define LINK_KICK        9u
#define LINK_STALL       10u
#define LINK_OWN         11u
#define LINK_ENABLE      12u
#define LINK_DISABLE     13u
#define LINK_BOOT        0xB0000000u
#define LINK_WRITTEN     0x57000000u
#define LINK_BYTE        0x5D000000u
#define LINK_READ_DONE   0x5E000000u
#define LINK_TIME        0x5A000000u
#define LINK_ADDRESSED   0xADD00000u
#define LINK_DATA        0xDA7A0000u
#define LINK_STOPPED     0x5709u
#define LINK_NACKED      0x4AC0u
#define LINK_TIMED_OUT   0x7140u
#define LINK_NACK_NEXT   0xEEu
#define LINK_SLOW        0xF000FFFFu
#define LINK_TIMING      0x00100305u
#define LINK_MODE_TARGET 0u
#define LINK_MODE_OFF    1u
#define LINK_MODE_BUSY   2u
#define LINK_MODE_HELD   3u

typedef struct
{
  uint32_t magic;
  uint32_t entry;
  uint32_t pc;
  uint32_t log_len;
  uint32_t count;
  uint32_t first_tx;
  uint32_t own;
  uint32_t mode;
  uint32_t timeout;
  uint32_t stall_us;
  uint32_t script[64];
  uint32_t log[256];
} link_area_t;

#define LINK_AREA ((volatile link_area_t*)0x20080000u)

volatile uint32_t link_target_active;
static uint8_t    link_next_tx;

void link_run(void);

static void link_log(uint32_t value)
{
  LINK_AREA->log[LINK_AREA->log_len++ & 0xffu] = value;
}

static void link_wait_us(uint32_t us)
{
  uint32_t const from = DWT->CYCCNT;
  while(DWT->CYCCNT - from < us * 4u)
  {
  }
}

static void link_configure(uint32_t timing, uint32_t own)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_I2C1);
  LL_I2C_InitTypeDef config = {
      .PeripheralMode  = LL_I2C_MODE_I2C,
      .Timing          = timing,
      .AnalogFilter    = LL_I2C_ANALOGFILTER_ENABLE,
      .DigitalFilter   = 0,
      .OwnAddress1     = own << 1,
      .TypeAcknowledge = LL_I2C_ACK,
      .OwnAddrSize     = LL_I2C_OWNADDRESS1_7BIT,
  };
  LL_I2C_Init(I2C1, &config);
}

static void link_timeout(void)
{
  if(LINK_AREA->timeout == 0)
  {
    return;
  }
  LL_I2C_Disable(I2C1);
  LL_I2C_SetSMBusTimeoutA(I2C1, LINK_AREA->timeout - 1u);
  LL_I2C_EnableSMBusTimeout(I2C1, LL_I2C_SMBUS_TIMEOUTA);
  LL_I2C_EnableIT_ERR(I2C1);
  NVIC_EnableIRQ(I2C1_ER_IRQn);
  LL_I2C_Enable(I2C1);
}

static void link_target_configure(void)
{
  uint32_t const mode = LINK_AREA->mode;
  if(mode == LINK_MODE_HELD)
  {
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_I2C1);
    LL_APB1_GRP1_ForceReset(LL_APB1_GRP1_PERIPH_I2C1);
    return;
  }
  if(mode == LINK_MODE_BUSY)
  {
    link_configure(LINK_SLOW, 0);
    LL_I2C_HandleTransfer(
        I2C1, 0x7Fu << 1, LL_I2C_ADDRSLAVE_7BIT, 1, LL_I2C_MODE_AUTOEND,
        LL_I2C_GENERATE_START_WRITE);
    return;
  }
  link_configure(LINK_TIMING, LINK_AREA->own);
  LL_I2C_EnableGeneralCall(I2C1);
  LL_I2C_EnableIT_ADDR(I2C1);
  LL_I2C_EnableIT_RX(I2C1);
  LL_I2C_EnableIT_TX(I2C1);
  LL_I2C_EnableIT_NACK(I2C1);
  LL_I2C_EnableIT_STOP(I2C1);
  link_target_active = 1;
  NVIC_EnableIRQ(I2C1_EV_IRQn);
  link_timeout();
  if(mode == LINK_MODE_OFF)
  {
    LL_I2C_Disable(I2C1);
  }
}

static void link_controller_configure(void)
{
  link_configure(LINK_TIMING, 0);
  LL_I2C_DisableOwnAddress1(I2C1);
}

void link_target_event(void)
{
  if(LL_I2C_IsActiveFlag_ADDR(I2C1))
  {
    link_log(
        LINK_ADDRESSED
        | LL_I2C_GetAddressMatchCode(I2C1)
        | (LL_I2C_GetTransferDirection(I2C1) >> I2C_ISR_DIR_Pos));
    if(LL_I2C_GetTransferDirection(I2C1) == LL_I2C_DIRECTION_READ)
    {
      link_next_tx = (uint8_t)(LINK_AREA->first_tx != 0 ? LINK_AREA->first_tx : 0xB0u);
      LL_I2C_ClearFlag_TXE(I2C1);
    }
    LL_I2C_ClearFlag_ADDR(I2C1);
  }
  if(LL_I2C_IsActiveFlag_TXIS(I2C1))
  {
    LL_I2C_TransmitData8(I2C1, link_next_tx++);
  }
  if(LL_I2C_IsActiveFlag_NACK(I2C1))
  {
    link_log(LINK_NACKED);
    LL_I2C_ClearFlag_NACK(I2C1);
  }
  if(LL_I2C_IsActiveFlag_RXNE(I2C1))
  {
    uint8_t const byte = LL_I2C_ReceiveData8(I2C1);
    link_log(LINK_DATA | byte);
    if(byte == LINK_NACK_NEXT)
    {
      LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_NACK);
    }
  }
  if(LL_I2C_IsActiveFlag_STOP(I2C1))
  {
    link_log(LINK_STOPPED);
    LL_I2C_ClearFlag_STOP(I2C1);
  }
}

void link_error_event(void)
{
  if(LL_I2C_IsActiveSMBusFlag_TIMEOUT(I2C1))
  {
    link_log(LINK_TIMED_OUT);
    LL_I2C_ClearSMBusFlag_TIMEOUT(I2C1);
  }
}

static uint32_t link_ended(void)
{
  while(!LL_I2C_IsActiveFlag_STOP(I2C1))
  {
  }
  LL_I2C_ClearFlag_STOP(I2C1);
  uint32_t const nack = LL_I2C_IsActiveFlag_NACK(I2C1);
  LL_I2C_ClearFlag_NACK(I2C1);
  return nack;
}

static void link_write(uint32_t arg, uint32_t end)
{
  uint32_t const address = (arg >> 16) & 0x7Fu;
  uint32_t const count   = (arg >> 8) & 0xFFu;
  uint32_t const first   = arg & 0xFFu;
  LL_I2C_HandleTransfer(
      I2C1, address << 1, LL_I2C_ADDRSLAVE_7BIT, count, end, LL_I2C_GENERATE_START_WRITE);
  uint32_t sent = 0;
  for(; sent < count; ++sent)
  {
    while(!LL_I2C_IsActiveFlag_TXIS(I2C1) && !LL_I2C_IsActiveFlag_NACK(I2C1))
    {
    }
    if(LL_I2C_IsActiveFlag_NACK(I2C1))
    {
      break;
    }
    if(LINK_AREA->stall_us != 0)
    {
      link_wait_us(LINK_AREA->stall_us);
    }
    LL_I2C_TransmitData8(I2C1, (uint8_t)(first + sent));
  }
  uint32_t nack = 0;
  if(end == LL_I2C_MODE_AUTOEND || LL_I2C_IsActiveFlag_NACK(I2C1))
  {
    nack = link_ended();
  }
  else
  {
    while(!LL_I2C_IsActiveFlag_TC(I2C1) && !LL_I2C_IsActiveFlag_NACK(I2C1))
    {
    }
    if(LL_I2C_IsActiveFlag_NACK(I2C1))
    {
      nack = link_ended();
    }
  }
  link_log(LINK_WRITTEN | nack << 16 | address << 8 | sent);
}

static void link_read(uint32_t arg)
{
  uint32_t const address = (arg >> 16) & 0x7Fu;
  uint32_t const count   = (arg >> 8) & 0xFFu;
  LL_I2C_HandleTransfer(
      I2C1, address << 1, LL_I2C_ADDRSLAVE_7BIT, count, LL_I2C_MODE_AUTOEND,
      LL_I2C_GENERATE_START_READ);
  for(uint32_t index = 0; index < count; ++index)
  {
    while(!LL_I2C_IsActiveFlag_RXNE(I2C1) && !LL_I2C_IsActiveFlag_NACK(I2C1))
    {
    }
    if(!LL_I2C_IsActiveFlag_RXNE(I2C1))
    {
      break;
    }
    link_log(LINK_BYTE | LL_I2C_ReceiveData8(I2C1));
  }
  link_log(LINK_READ_DONE | link_ended() << 16 | address << 8);
}

static void link_kick(uint32_t arg)
{
  uint32_t const address = (arg >> 16) & 0x7Fu;
  LL_I2C_HandleTransfer(
      I2C1, address << 1, LL_I2C_ADDRSLAVE_7BIT, 1, LL_I2C_MODE_AUTOEND,
      LL_I2C_GENERATE_START_WRITE);
  LL_I2C_TransmitData8(I2C1, (uint8_t)arg);
}

static void link_own(uint32_t address)
{
  LL_I2C_DisableOwnAddress1(I2C1);
  LL_I2C_SetOwnAddress1(I2C1, address << 1, LL_I2C_OWNADDRESS1_7BIT);
  LL_I2C_EnableOwnAddress1(I2C1);
}

static void link_rcc(void)
{
  LL_APB1_GRP1_ForceReset(LL_APB1_GRP1_PERIPH_I2C1);
  LL_APB1_GRP1_ReleaseReset(LL_APB1_GRP1_PERIPH_I2C1);
  if(LINK_AREA->entry == LINK_CONTROLLER)
  {
    link_controller_configure();
  }
  else
  {
    link_target_configure();
  }
}

void link_run(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  link_log(LINK_BOOT | LINK_AREA->pc);
  if(LINK_AREA->entry == LINK_CONTROLLER)
  {
    link_controller_configure();
  }
  else
  {
    link_target_configure();
  }
  for(;;)
  {
    uint32_t const step = LINK_AREA->script[LINK_AREA->pc++ & 0x3fu];
    uint32_t const arg  = step & 0xffffffu;
    switch(step >> 24)
    {
      case LINK_WRITE:
        link_write(arg, LL_I2C_MODE_AUTOEND);
        break;
      case LINK_WRITE_SR:
        link_write(arg, LL_I2C_MODE_SOFTEND);
        break;
      case LINK_READ:
        link_read(arg);
        break;
      case LINK_WAIT_US:
        link_wait_us(arg);
        break;
      case LINK_RESET:
        NVIC_SystemReset();
        break;
      case LINK_RCC:
        link_rcc();
        break;
      case LINK_STAMP:
        link_log(LINK_TIME | (DWT->CYCCNT & 0xffffffu));
        break;
      case LINK_REPEAT:
        if(++LINK_AREA->count < (arg >> 8))
        {
          LINK_AREA->pc = arg & 0xffu;
        }
        break;
      case LINK_KICK:
        link_kick(arg);
        break;
      case LINK_STALL:
        LINK_AREA->stall_us = arg;
        break;
      case LINK_OWN:
        link_own(arg & 0x7Fu);
        break;
      case LINK_ENABLE:
        LL_I2C_Enable(I2C1);
        break;
      case LINK_DISABLE:
        LL_I2C_Disable(I2C1);
        break;
      default:
        LINK_AREA->pc--;
        for(;;)
        {
          __WFI();
        }
    }
  }
}

uint32_t link_wanted(void)
{
  return LINK_AREA->magic == LINK_MAGIC;
}
