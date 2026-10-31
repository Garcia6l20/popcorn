#include "periph_test.h"
#include "stm32l1xx.h"
#include "stm32l1xx_hal.h"
#include "stm32l1xx_ll_bus.h"
#include "stm32l1xx_ll_i2c.h"

#define PCLK1_HZ 2097152u
#define EEPROM   0x50u
#define MEMORY   0x0040u

static I2C_HandleTypeDef i2c;
static DMA_HandleTypeDef dma_rx;
static uint8_t           dma_buffer[4];
static uint8_t const     pattern[] = {0xA1u, 0xB2u, 0xC3u};
static uint8_t           next_tx;

#define TARGET_OWN       0x42u
#define TARGET_ADDRESS   0xADD00000u
#define TARGET_STOP      0x5709u
#define TARGET_DATA      0xDA7A0000u
#define TARGET_NACK_NEXT 0xEEu
#define TARGET_TX_FIRST  0xB0u
#define TARGET_NACKED    0x4AC0u

void idle(void);
void write_eeprom(void);
void nack(void);
void read_one(void);
void read_two(void);
void read_four(void);
void hal_read_one(void);
void hal_read_two(void);
void hal_read_three(void);
void hal_read_none(void);
void hal_ready(void);
void hal_slave(void);
void dma_write(void);
void dma_read(void);
void dma_hal_read(void);
void DMA1_Channel7_IRQHandler(void);
void SysTick_Handler(void);
void target(void);
void I2C1_EV_IRQHandler(void);
void I2C1_ER_IRQHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void idle(void)
{
  for(;;)
  {
    __WFI();
  }
}

static void start(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_I2C1);
  LL_I2C_ConfigSpeed(I2C1, PCLK1_HZ, 100000u, LL_I2C_DUTYCYCLE_2);
  LL_I2C_Enable(I2C1);
  LL_I2C_GenerateStartCondition(I2C1);
  while(!LL_I2C_IsActiveFlag_SB(I2C1))
  {
  }
}

static void send(uint8_t byte)
{
  while(!LL_I2C_IsActiveFlag_TXE(I2C1))
  {
    if(LL_I2C_IsActiveFlag_AF(I2C1))
    {
      FAIL(1);
    }
  }
  LL_I2C_TransmitData8(I2C1, byte);
}

static void stop(void)
{
  LL_I2C_GenerateStopCondition(I2C1);
  while(LL_I2C_IsActiveFlag_BUSY(I2C1))
  {
  }
  LOG(I2C1->SR1);
  LOG(I2C1->SR2);
}

void write_eeprom(void)
{
  static uint8_t const out[] = {0x00, 0x10, 0xA5, 0x5A, 0x3C};
  start();
  LL_I2C_TransmitData8(I2C1, EEPROM << 1);
  while(!LL_I2C_IsActiveFlag_ADDR(I2C1))
  {
    if(LL_I2C_IsActiveFlag_AF(I2C1))
    {
      FAIL(2);
    }
  }
  LL_I2C_ClearFlag_ADDR(I2C1);
  LOG(I2C1->SR2);
  for(uint32_t index = 0; index < sizeof out; ++index)
  {
    send(out[index]);
  }
  while(!LL_I2C_IsActiveFlag_BTF(I2C1))
  {
  }
  LOG(I2C1->SR1);
  stop();
  DONE();
}

void nack(void)
{
  start();
  LL_I2C_TransmitData8(I2C1, (EEPROM + 1u) << 1);
  while(!LL_I2C_IsActiveFlag_AF(I2C1))
  {
    if(LL_I2C_IsActiveFlag_ADDR(I2C1))
    {
      FAIL(3);
    }
  }
  LOG(I2C1->SR1);
  LOG(I2C1->SR2);
  stop();
  LL_I2C_ClearFlag_AF(I2C1);
  LOG(I2C1->SR1);
  DONE();
}

static void address_read(void)
{
  start();
  LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_ACK);
  LL_I2C_TransmitData8(I2C1, EEPROM << 1 | 1u);
  while(!LL_I2C_IsActiveFlag_ADDR(I2C1))
  {
    if(LL_I2C_IsActiveFlag_AF(I2C1))
    {
      FAIL(4);
    }
  }
}

static void wait_btf(void)
{
  while(!LL_I2C_IsActiveFlag_BTF(I2C1))
  {
  }
}

void read_one(void)
{
  address_read();
  LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_NACK);
  LL_I2C_ClearFlag_ADDR(I2C1);
  LL_I2C_GenerateStopCondition(I2C1);
  while(!LL_I2C_IsActiveFlag_RXNE(I2C1))
  {
  }
  LOG(LL_I2C_ReceiveData8(I2C1));
  stop();
  DONE();
}

void read_two(void)
{
  address_read();
  LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_NACK);
  LL_I2C_EnableBitPOS(I2C1);
  LL_I2C_ClearFlag_ADDR(I2C1);
  wait_btf();
  LL_I2C_GenerateStopCondition(I2C1);
  LOG(LL_I2C_ReceiveData8(I2C1));
  LOG(LL_I2C_ReceiveData8(I2C1));
  stop();
  LL_I2C_DisableBitPOS(I2C1);
  DONE();
}

void read_four(void)
{
  address_read();
  LL_I2C_ClearFlag_ADDR(I2C1);
  while(!LL_I2C_IsActiveFlag_RXNE(I2C1))
  {
  }
  LOG(LL_I2C_ReceiveData8(I2C1));
  wait_btf();
  LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_NACK);
  LOG(LL_I2C_ReceiveData8(I2C1));
  wait_btf();
  LL_I2C_GenerateStopCondition(I2C1);
  LOG(LL_I2C_ReceiveData8(I2C1));
  LOG(LL_I2C_ReceiveData8(I2C1));
  stop();
  DONE();
}

static void hal_setup_own(uint32_t own)
{
  HAL_Init();
  __HAL_RCC_I2C1_CLK_ENABLE();
  i2c.Instance             = I2C1;
  i2c.Init.ClockSpeed      = 100000u;
  i2c.Init.DutyCycle       = I2C_DUTYCYCLE_2;
  i2c.Init.OwnAddress1     = own;
  i2c.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
  i2c.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  i2c.Init.OwnAddress2     = 0u;
  i2c.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  i2c.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
  if(HAL_I2C_Init(&i2c) != HAL_OK)
  {
    FAIL(10);
  }
}

static void hal_setup(void)
{
  hal_setup_own(0u);
}

static void hal_mem_read(uint16_t size)
{
  uint8_t got[sizeof pattern] = {0};
  hal_setup();
  if(HAL_I2C_Mem_Write(
         &i2c, EEPROM << 1, MEMORY, I2C_MEMADD_SIZE_16BIT, (uint8_t*)pattern, size, 10u)
     != HAL_OK)
  {
    FAIL(11);
  }
  uint32_t const written = HAL_GetTick();
  if(HAL_I2C_IsDeviceReady(&i2c, EEPROM << 1, 100u, 10u) != HAL_OK)
  {
    FAIL(12);
  }
  LOG(HAL_GetTick() - written);
  if(HAL_I2C_Mem_Read(&i2c, EEPROM << 1, MEMORY, I2C_MEMADD_SIZE_16BIT, got, size, 10u) != HAL_OK)
  {
    FAIL(13);
  }
  for(uint32_t index = 0; index < size; ++index)
  {
    LOG(got[index]);
  }
  LOG(i2c.ErrorCode);
  DONE();
}

void hal_read_one(void)
{
  hal_mem_read(1u);
}

void hal_read_two(void)
{
  hal_mem_read(2u);
}

void hal_read_three(void)
{
  hal_mem_read(3u);
}

void hal_read_none(void)
{
  uint8_t got[2] = {0};
  hal_setup();
  if(HAL_I2C_Mem_Read(&i2c, EEPROM << 1, MEMORY, I2C_MEMADD_SIZE_16BIT, got, 0u, 10u) != HAL_OK)
  {
    FAIL(14);
  }
  while(I2C1->SR2 & I2C_SR2_BUSY)
  {
  }
  LOG(I2C1->SR1);
  if(HAL_I2C_Mem_Write(&i2c, EEPROM << 1, MEMORY, I2C_MEMADD_SIZE_16BIT, (uint8_t*)pattern, 2u, 10u)
     != HAL_OK)
  {
    FAIL(15);
  }
  if(HAL_I2C_IsDeviceReady(&i2c, EEPROM << 1, 100u, 10u) != HAL_OK)
  {
    FAIL(16);
  }
  if(HAL_I2C_Mem_Read(&i2c, EEPROM << 1, MEMORY, I2C_MEMADD_SIZE_16BIT, got, 2u, 10u) != HAL_OK)
  {
    FAIL(17);
  }
  LOG(got[0]);
  LOG(got[1]);
  LOG(i2c.ErrorCode);
  DONE();
}

void hal_slave(void)
{
  uint8_t got[2] = {0};
  hal_setup_own(TARGET_OWN << 1);
  for(uint32_t call = 0; call < 2u; ++call)
  {
    LOG(HAL_I2C_Slave_Receive(&i2c, got, 2u, 20u));
    LOG(got[0]);
    LOG(got[1]);
    HAL_Delay(2u);
  }
  DONE();
}

void hal_ready(void)
{
  hal_setup();
  LOG(HAL_I2C_IsDeviceReady(&i2c, EEPROM << 1, 1u, 10u));
  uint32_t const before = HAL_GetTick();
  LOG(HAL_I2C_IsDeviceReady(&i2c, (EEPROM + 1u) << 1, 3u, 10u));
  LOG(HAL_GetTick() - before);
  LOG(I2C1->SR1);
  LOG(HAL_I2C_IsDeviceReady(&i2c, EEPROM << 1, 1u, 10u));
  LOG(I2C1->SR1);
  LOG(I2C1->SR2);
  DONE();
}

static void dma_channel(
    DMA_Channel_TypeDef* channel,
    uint32_t             control,
    uint8_t const*       memory,
    uint32_t             items)
{
  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
  channel->CCR   = 0u;
  channel->CPAR  = (uint32_t)&I2C1->DR;
  channel->CMAR  = (uint32_t)memory;
  channel->CNDTR = items;
  channel->CCR   = control | DMA_CCR_MINC | DMA_CCR_EN;
}

void dma_write(void)
{
  static uint8_t const out[] = {0xD1u, 0xD2u, 0xD3u};
  dma_channel(DMA1_Channel6, DMA_CCR_DIR, out, sizeof out);
  start();
  LL_I2C_TransmitData8(I2C1, EEPROM << 1);
  while(!LL_I2C_IsActiveFlag_ADDR(I2C1))
  {
    if(LL_I2C_IsActiveFlag_AF(I2C1))
    {
      FAIL(30);
    }
  }
  LL_I2C_EnableDMAReq_TX(I2C1);
  LL_I2C_ClearFlag_ADDR(I2C1);
  while(!(DMA1->ISR & DMA_ISR_TCIF6))
  {
  }
  LL_I2C_DisableDMAReq_TX(I2C1);
  wait_btf();
  LOG(I2C1->SR1);
  stop();
  DONE();
}

void dma_read(void)
{
  dma_channel(DMA1_Channel7, 0u, dma_buffer, sizeof dma_buffer);
  address_read();
  LL_I2C_EnableDMAReq_RX(I2C1);
  LL_I2C_EnableLastDMA(I2C1);
  LL_I2C_ClearFlag_ADDR(I2C1);
  while(!(DMA1->ISR & DMA_ISR_TCIF7))
  {
  }
  LL_I2C_GenerateStopCondition(I2C1);
  LL_I2C_DisableDMAReq_RX(I2C1);
  LL_I2C_DisableLastDMA(I2C1);
  while(LL_I2C_IsActiveFlag_BUSY(I2C1))
  {
  }
  for(uint32_t index = 0; index < sizeof dma_buffer; ++index)
  {
    LOG(dma_buffer[index]);
  }
  LOG(I2C1->SR2);
  DONE();
}

void DMA1_Channel7_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&dma_rx);
}

void dma_hal_read(void)
{
  static uint8_t got[2];
  hal_setup();
  __HAL_RCC_DMA1_CLK_ENABLE();
  dma_rx.Instance                 = DMA1_Channel7;
  dma_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
  dma_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
  dma_rx.Init.MemInc              = DMA_MINC_ENABLE;
  dma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  dma_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  dma_rx.Init.Mode                = DMA_NORMAL;
  dma_rx.Init.Priority            = DMA_PRIORITY_LOW;
  if(HAL_DMA_Init(&dma_rx) != HAL_OK)
  {
    FAIL(31);
  }
  __HAL_LINKDMA(&i2c, hdmarx, dma_rx);
  NVIC_EnableIRQ(DMA1_Channel7_IRQn);
  if(HAL_I2C_Mem_Read_DMA(&i2c, EEPROM << 1, MEMORY, I2C_MEMADD_SIZE_16BIT, got, sizeof got)
     != HAL_OK)
  {
    FAIL(32);
  }
  uint32_t const begun = HAL_GetTick();
  while(HAL_I2C_GetState(&i2c) != HAL_I2C_STATE_READY)
  {
    if(HAL_GetTick() - begun > 10u)
    {
      FAIL(33);
    }
  }
  while(I2C1->SR2 & I2C_SR2_BUSY)
  {
  }
  LOG(got[0]);
  LOG(got[1]);
  LOG(i2c.ErrorCode);
  DONE();
}

void I2C1_EV_IRQHandler(void)
{
  uint32_t const status = I2C1->SR1;
  if(status & I2C_SR1_ADDR)
  {
    uint32_t const transmit = I2C1->SR2 & I2C_SR2_TRA;
    LOG(TARGET_ADDRESS | (TARGET_OWN << 1) | (transmit ? 1u : 0u));
    next_tx = TARGET_TX_FIRST;
    if(transmit)
    {
      LL_I2C_TransmitData8(I2C1, next_tx++);
    }
  }
  else if((status & I2C_SR1_TXE) && (I2C1->SR2 & I2C_SR2_TRA))
  {
    LL_I2C_TransmitData8(I2C1, next_tx++);
  }
  if(status & I2C_SR1_RXNE)
  {
    uint8_t const byte = LL_I2C_ReceiveData8(I2C1);
    LOG(TARGET_DATA | byte);
    LL_I2C_AcknowledgeNextData(I2C1, byte == TARGET_NACK_NEXT ? LL_I2C_NACK : LL_I2C_ACK);
  }
  if(status & I2C_SR1_STOPF)
  {
    LOG(TARGET_STOP);
    LL_I2C_Enable(I2C1);
  }
}

void I2C1_ER_IRQHandler(void)
{
  if(LL_I2C_IsActiveFlag_AF(I2C1))
  {
    LOG(TARGET_NACKED);
    LL_I2C_ClearFlag_AF(I2C1);
  }
}

void target(void)
{
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_I2C1);
  LL_I2C_SetPeriphClock(I2C1, PCLK1_HZ);
  LL_I2C_SetOwnAddress1(I2C1, TARGET_OWN << 1, LL_I2C_OWNADDRESS1_7BIT);
  LL_I2C_EnableIT_EVT(I2C1);
  LL_I2C_EnableIT_BUF(I2C1);
  LL_I2C_EnableIT_ERR(I2C1);
  NVIC_EnableIRQ(I2C1_EV_IRQn);
  NVIC_EnableIRQ(I2C1_ER_IRQn);
  LL_I2C_Enable(I2C1);
  LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_ACK);
  for(;;)
  {
    __WFI();
  }
}

int main(void)
{
  write_eeprom();
  return 0;
}
