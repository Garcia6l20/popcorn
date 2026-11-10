# STM32L4

A Machine runs any of the 42 shipped STM32L471, L475, L476, L485 and L486 order codes on a
Cortex-M4 core with its FPU and without TrustZone. Each code loads from the shipped description
alone:

```python
from popcorn.stm32 import mcu

part = mcu("STM32L476RGTx")
```

The [STM32L4 parts](generated/families/stm32l4.md) page lists every code with its die, package,
memory and state.
Each row links to the peripheral page of its code, such as
[STM32L476RGTx](generated/STM32L476RGTx.md), with its instances and their models.

## Lines and order codes

All 42 codes sit on the die DIE415, the `DBGMCU_IDCODE` DEV_ID 0x415 of RM0351 48.6.1. The
order-code letter after the pin count gives the flash size, `C` 256 KiB, `E` 512 KiB and `G`
1024 KiB. Every code has 96 KiB of SRAM1 and 32 KiB of SRAM2.

| Line | Order codes | Reference code of the line layer |
|---|---|---|
| STM32L471 | 10 | STM32L471ZGJx |
| STM32L475 | 6 | STM32L475VGTx |
| STM32L476 | 19 | STM32L476ZGJx |
| STM32L485 | 2 | STM32L485JEYx |
| STM32L486 | 5 | STM32L486ZGTx |

The instances of a code follow its line and package, and its peripheral page lists them. The
STM32L471 has no OTG_FS and no LCD, the STM32L485 and STM32L486 carry the AES unit. The STM32L485
follows the STM32L486 documents, see [Limits](limits.md#stm32l485). The packages are LQFP64,
LQFP100, LQFP144, UFBGA132, UFBGA144, WLCSP72, WLCSP81 and WLCSP99. The suffix `P` marks the codes
with the external SMPS pinout, `STM32L476JGYxP`, `STM32L476QGIxP`, `STM32L476VGYxP` and
`STM32L476ZGTxP`.

The examples classify a code by its die, flash size, package and SMPS supply. The 12 codes in
the WLCSP72, WLCSP81, WLCSP99 and UFBGA144 packages classify through the `JY`, `MY`, `VY` and
`ZJ` package suffixes of the family, and the `P` suffix sets the SMPS supply of the STM32L4
codes only. So every code has a class on its family page, with the state `attaches` when no
example runs on its class.

## What is modelled

These instances have a built-in model on every code that has them. [Limits](limits.md) lists
what each model leaves out.

| Block | Instances | Notes |
|---|---|---|
| RCC | RCC | MSI, HSI16, HSE, LSI, LSE, the PLLs and the CLK48 selection |
| PWR | PWR | voltage ranges 1 and 2 with VOSF, `PWR_CR2.USV` and `IOSV` gates |
| FLASH | FLASH | two banks, double word and fast programming, option bytes, RDP and PCROP |
| GPIO | GPIOA to GPIOH | `GPIOx_ASCR` analog switches, the analog level of each pin |
| EXTI, SYSCFG | EXTI, SYSCFG | line routing, the SRAM2 software erase |
| DMA | DMA1, DMA2 | channels with the CSELR request selection of RM0351 |
| USART | USART1 to USART3, UART4, UART5, LPUART1 | single register set, no FIFO |
| SPI | SPI1 to SPI3 | controller and target, 4 to 16 bit frames, 32-bit FIFOs, linked to `SpiTarget` devices |
| I2C | I2C1 to I2C3 | controller, linked to `I2CTarget` devices |
| TIM | TIM1 to TIM8, TIM15 to TIM17 | update, one-pulse, capture and compare |
| LPTIM | LPTIM1, LPTIM2 | counter, one-shot, encoder, triggers and external clocks |
| ADC | ADC1 to ADC3, ADC_Common | channels on the GPIO analog level, internal channels, injected queue, watchdogs |
| DAC | DAC1 | two channels, waves, sample and hold, the pad and the on-chip ADC input |
| RTC | RTC | calendar, wakeup timer, alarms, timestamps, tampers, backup registers |
| IWDG, CRC | IWDG, CRC | window and hardware start, fixed polynomial CRC |
| WWDG | WWDG | window, early wake-up interrupt |
| SAI | SAI1, SAI2 | frames to the board's audio endpoints |
| RNG | RNG | one word per phase from a seed or a queue, clock and seed errors |
| USB | OTG_FS | full-speed device with its packet memory |
| DBGMCU | DBGMCU | `DBGMCU_IDCODE` reads DIE415 |

The model column of a part page marks the instances without a model as `none`. On the STM32L4
these are CAN, COMP, DFSDM, FIREWALL, FMC, LCD, OPAMP, QUADSPI, SDMMC, SWPMI, TSC, VREFBUF and
the AES. They keep plain register storage. A firmware access logs one `model_warning`
per register and raises `ModelError` under `strict`.

A timer whose description has no `TIMx_SMCR`, TIM6, TIM7, TIM16 and TIM17, serves the reserved
word at offset 0x08. A read gives 0 and logs nothing, so `HAL_TIMEx_MasterConfigSynchronization`
runs on them. A write is dropped with one `model_warning`, and raises `ModelError` under
`strict`. The read value is inferred, RM0351 Table 215 gives none.

## Boot memory

Every code aliases its main flash at address 0, RM0351 2.6.1, and VTOR resets to 0. The core
boots from the vector table read through the alias. `SYSCFG_MEMRMP` is storage and address 0
always maps the flash. The system memory regions at 0x1FFF0000 and 0x1FFF8000 hold no
bootloader code and read 0xFFFFFFFF, so ES0250 2.2.18, a bootloader without SPI and CAN
support, has nothing to apply to. A firmware that jumps to the system bootloader reaches erased
memory.

## Bit-banding

Every code maps the two Cortex-M4 bit-band windows from its description. The alias window at
0x22000000 maps SRAM1 at 0x20000000 and the one at 0x42000000 maps the peripherals at
0x40000000. A CPU access of 1, 2 or 4 bytes reaches the bit it names. A read returns 0 or 1 and
a write sets or clears the bit from bit 0 of the value. The peripheral models see the access at
the target address, so an alias write to `RCC_CR` reaches the RCC model like a plain write.

## Memory map

| Region | Start | Size |
|---|---|---|
| Flash | 0x08000000 | 256, 512 or 1024 KiB |
| SRAM1 | 0x20000000 | 96 KiB |
| SRAM2 | 0x10000000 | 32 KiB |
| System memory | 0x1FFF0000 and 0x1FFF8000 | 28 KiB each |
| OTP and option bytes, bank 1 | 0x1FFF7000 | 4 KiB |
| Option bytes, bank 2 | 0x1FFFF000 | 4 KiB |

The flash holds two banks of half the size, and bank 2 starts at the flash base plus half the
size, `0x08020000` on a 256 KiB code and `0x08080000` on a 1024 KiB code, RM0351 3.3.1.
SRAM2 is not aliased past 0x1000FFFF.

## Option bytes

`Machine(option_bytes=OptionBytes(...))` takes two keys on the STM32L4:

| Key | Accepted | Effect |
|---|---|---|
| `swap_bank` | every code | `FLASH_OPTR.BFB2` loads as 1 and the core boots from the bank 2 base |
| `dualbank` | codes of 512 KiB or less | `False` clears `FLASH_OPTR.DUALBANK`, the flash is one bank |

Every other field stays at its default. A field away from its default raises `ConfigError`
keyed `option_bytes.<field>`.

```python
from popcorn import Machine, OptionBytes
from popcorn.stm32 import mcu

machine = Machine(mcu("STM32L476RGTx"), option_bytes=OptionBytes(swap_bank=True))
```

Firmware reaches the other option fields through `FLASH_OPTSTRT`. A loaded `RDP` of 0xAA is
level 0, 0xCC is level 2 and any other value is level 1. At level 1 a debugger access to the
flash raises `BusAccessError`, and so does one to the OTP area or SRAM2, inferred. At level 2
every debugger access does. A PCROP area reads 0 for a CPU, DMA or debugger load, inferred,
sets `FLASH_SR.RDERR` and drops a debugger store. Writing RDP 0xAA at level 1 erases the flash
and zeroes SRAM2 at once, inferred. The
[TrustZone guide](../guide/trustzone.md) lists the debug views these rules reach, and
[Limits](limits.md) the details.

## Examples

The 15 examples of `examples/stm32l476` run on the STM32L476RCTx. The `gpio`, `pwr_flash`,
`rcc` and `usart` examples also run on the 1 MiB LQFP64 codes STM32L471RGTx, STM32L475RGTx and
STM32L486RGTx, and `usb_cdc` on the two with an OTG_FS. The state column of the parts page
names the evidence behind each code.

## Inferred rules

Rules that no reference manual section settles are marked inferred in [Limits](limits.md).
The STM32L4 ones are the peripheral timings and orderings that need a NUCLEO-L476RG to
confirm, among them:

- the FLASH fast programming and RDP regression sequences, with the level 1 refusal of the OTP
  area and SRAM2, the 0 a PCROP area reads and the SRAM2 zeroing,
- the RNG seed error recovery, where SECS clears only on an RNGEN 0 write,
- the ADC trigger latencies and the injected conversion order,
- the LPTIM clock-change and trigger delays,
- the DAC sample and hold phases and calibration,
- the RTC timestamp and tamper delays,
- the SRAM2 erase time and the register value of a reserved TIM word.
