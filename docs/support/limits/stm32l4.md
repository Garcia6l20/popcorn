# STM32L4

## STM32L4 core

The Cortex-M4 of the STM32L47x/L48x runs as ARMv7-M specifies, without the core errata of
ES0250:

- ES0250 2.1.1: a single-word load to SP interrupted in its data phase completes once, the base
  register updated once.
- ES0250 2.1.2: a VDIV or VSQRT running when an interrupt is taken completes before the handler's
  first instruction, with or without lazy FP context save.
- ES0250 2.1.3: an exception return that overlaps a buffered store vectors to the interrupt the
  NVIC acknowledged.

## STM32L485

ST publishes no datasheet and no errata sheet that names the STM32L485. The two shipped order
codes, STM32L485JCYx and STM32L485JEYx, are modelled from the STM32L486 documents: RM0351, the
STM32L486 datasheet DS10199 and the errata sheet ES0250. Every STM32L485 behaviour is inferred from
the STM32L486, and no STM32L485 board has been read back.

## STM32L4 instances without a model

QUADSPI, DFSDM1, LCD, CAN1, COMP, OPAMP, TSC, SWPMI1, SDMMC1, FMC, FIREWALL and VREFBUF
keep plain register storage, as does the AES of the STM32L485 and STM32L486. The generic rules
of "Peripherals without a model" apply to them. The ES0250 items of these blocks, 2.3, 2.5, 2.6,
2.7, 2.10 to 2.14, 2.16, 2.25 and 2.26, describe silicon behaviour a storage block does not
produce.

- ES0250 2.2.26, the OPAMP output consuming extra current from VDDA, is electrical. The OPAMP
  registers store and the supply current is not modelled. ES0250 2.11.1, the OPAMP
  characteristics degrading with the current sunk on its output, is electrical too.

## STM32L4 errata

The errata of ES0250 Rev 13 sit in the section of the IP they concern. This table lists them
by IP. Each section states per item whether the model reproduces it, corrects the reference
manual as the erratum does, or has no counterpart.

| IP | ES0250 items | Section |
|---|---|---|
| Cortex-M4 core | 2.1.1 to 2.1.3 | STM32L4 core |
| RCC clocks | 2.2.1, 2.2.3, 2.2.8, 2.2.10, 2.2.11, 2.2.22, 2.2.24, 2.2.25 | RCC |
| PWR | 2.2.5, 2.2.21 | PWR |
| FLASH | 2.2.2, 2.2.9, 2.2.14, 2.2.15, 2.2.23 | FLASH |
| GPIO | 2.2.6, 2.2.16, 2.2.17, 2.2.20 | GPIO |
| SYSCFG | 2.2.7, 2.2.12, 2.2.19 | SYSCFG |
| System memory | 2.2.18 | [STM32L4](stm32l4.md) |
| RTC and TAMP | 2.2.13, 2.19.1 to 2.19.7 | RTC and TAMP |
| DMA | 2.4.1, 2.4.2 | DMA |
| ADC | 2.2.4, 2.8.1 to 2.8.18 | ADC |
| DAC | 2.9.1 | DAC |
| OPAMP | 2.2.26, 2.11.1 | STM32L4 instances without a model |
| RNG | 2.15.1 | RNG |
| TIM | 2.17.1 to 2.17.3 | TIM |
| LPTIM | 2.18.1 to 2.18.4 | LPTIM |
| I2C | 2.20.1 to 2.20.13 | I2C |
| USART, UART | 2.21.1 to 2.21.7 | USART, UART and LPUART |
| LPUART | 2.22.1 to 2.22.4 | USART, UART and LPUART |
| SPI | 2.23.1 to 2.23.3 | SPI |
| SAI | 2.24.1 to 2.24.4 | SAI |
| OTG_FS | 2.27.1, 2.27.2 | OTG_FS |
