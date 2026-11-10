# STM32L1

A Machine runs any of the 132 shipped STM32L100, L151, L152 and L162 order codes on a Cortex-M3
core without TrustZone. Each code loads from the shipped description alone:

```python
from popcorn.stm32 import mcu

part = mcu("STM32L152RETx")
```

The [STM32L1 parts](generated/families/stm32l1.md) page lists every code with its die, package,
memory and state.
Each row links to the peripheral page of its code, such as
[STM32L152RETx](generated/STM32L152RETx.md), with its instances and their models.

## Categories

RM0038 sorts the family into six categories by die and flash size. The DBGMCU `DEV_ID` names the
die, and the parts page prints it as `DIE416` and so on. The order-code suffix `A` marks the
later die of a flash size, DIE429 at 32 to 128 KiB and DIE436 at 256 KiB. Per RM0038 30.6.1,
the Cat.3 codes in WLCSP64, `STM32L151RCYx`, and with more than 100 pins also sit on DIE436, so
`STM32L152RCTx` is on DIE427 and `STM32L152RCTxA` and `STM32L152ZCTx` on DIE436. The suffix `X`
marks the dual-bank Cat.6 codes. The SRAM sizes follow the datasheets, `STM32L100C6UxA`,
`STM32L100R8TxA` and `STM32L100RBTxA` hold 4, 8 and 16 KiB.

| Category | Die | Flash | SRAM | Data EEPROM | Order codes |
|---|---|---|---|---|---|
| Cat.1 | DIE416 | 32, 64, 128 KiB | 4 to 16 KiB | 2 or 4 KiB | 35 |
| Cat.2 | DIE429 | 32, 64, 128 KiB | 4 to 32 KiB | 2 or 4 KiB | 35 |
| Cat.3 | DIE427, DIE436 | 256 KiB | 16 to 32 KiB | 4 or 8 KiB | 29 |
| Cat.4 | DIE436 | 384 KiB | 48 KiB | 12 KiB | 15 |
| Cat.5 | DIE437 | 512 KiB | 80 KiB | 16 KiB | 14 |
| Cat.6 | DIE437 | 384 KiB | 80 KiB | 16 KiB | 4 |

The instances of a code follow its line and package, and its peripheral page lists them. Cat.1
and Cat.2 have one DMA controller and no SPI3 or TIM5. Cat.3 and up add DMA2 and SPI3. Their
L151, L152 and L162 codes also add TIM5 and OPAMP, and GPIOF and GPIOG where the package brings
them out. Cat.4 to Cat.6 add UART4 and UART5. Every Cat.4 code has SDIO, and the 9 Cat.4 codes
with more than 64 pins have FSMC. The L100, L152 and L162 codes carry the LCD and the L162 codes
the AES unit.

## What is modelled

These instances have a built-in model on every code that has them. [Limits](limits.md) lists what
each model leaves out.

| Block | Instances | Notes |
|---|---|---|
| RCC | RCC | MSI, HSI16, HSE, LSI, LSE and the PLL with its PLLMUL and PLLDIV codes |
| PWR | PWR | voltage ranges 1 to 3 and VOSF |
| FLASH | FLASH | program memory, data EEPROM and option words |
| GPIO | GPIOA to GPIOH | `GPIOx_BRR` on Cat.4 to Cat.6 |
| EXTI, SYSCFG | EXTI, SYSCFG | line routing through `SYSCFG_EXTICRx`, `SYSCFG_PMC.USB_PU` |
| DMA | DMA1, DMA2 | channels with the fixed request map of RM0038 |
| USART | USART1 to USART3, UART4, UART5 | frame timing from the APB clock and BRR |
| SPI | SPI1 to SPI3 | controller and target, Motorola frames, linked to `SpiTarget` devices |
| I2C | I2C1, I2C2 | controller transmitter and receiver, linked to `I2CTarget` devices |
| TIM | TIM2 to TIM7, TIM9 to TIM11 | update, one-pulse, capture and compare |
| ADC | ADC | channels, internal channels, the analog watchdog |
| RTC | RTC | calendar, wakeup timer, alarms, backup registers |
| IWDG, CRC | IWDG, CRC | fixed CRC-32 polynomial |
| DAC | DAC | two channels, triggers, waves, DMA, the pad |
| WWDG | WWDG | window, early wake-up interrupt, `APB1RSTR.WWDGRST` |
| USB | USB, USB_SRAM | full-speed device with the packet memory |
| DBGMCU | DBGMCU | `DBGMCU_IDCODE` reads the die |

The model column of a part page marks the instances without a model as `none`. On the STM32L1
these are COMP, OPAMP, RI, LCD, AES, FSMC and SDIO. They keep plain register storage.
A firmware access logs one `model_warning` per register and raises `ModelError` under `strict`.

## Boot memory

Every code aliases its main flash at address 0, the main flash boot of RM0038 2.7 Table 6, and
VTOR resets to 0. The core boots from the vector table read through the alias, so a load, a
fetch and an exception vector at 0 reach the flash. The second bank of a Cat.6 part reads at
0x00040000, inferred. SYSCFG_MEMRMP MEM_MODE is storage and address 0 always maps the flash.
With `swap_bank` it also maps the flash, where silicon keeps the system memory aliased at 0,
RM0038 Table 6.

## Bit-banding

Every code maps the two Cortex-M3 bit-band windows from its description. The alias window at
0x22000000 maps the SRAM at 0x20000000 and the one at 0x42000000 maps the peripherals at
0x40000000. A CPU access of 1, 2 or 4 bytes reaches the bit it names, and so does a debugger
access, inferred from the Cortex-M3 bus matrix. A read returns 0 or 1 and a write sets or clears
the bit from bit 0 of the value. The peripheral models see the access at the target address, so
an alias write to `RCC_CR` reaches the RCC model like a plain write. ST's HAL sets the RCC
enables this way. RM0038 2.6 limits alias operations to Cortex-M3 accesses, and a DMA access to
an alias address is a bus error, an inferred outcome. RM0038 2.6 gives an alias write the effect
of a read-modify-write, so a write to a register with write-1-to-clear flags clears the other
set flags, inferred.

```c
*(volatile uint32_t *)(0x42000000 + ((0x40023800 - 0x40000000) * 32) + (0 * 4)) = 1;
```

This sets `RCC_CR.HSION` at 0x40023800 bit 0. The window holds no bytes, so a snapshot carries no
image of it.

## Data EEPROM

The data EEPROM maps at 0x08080000 on every code, from 2 KiB on the smallest Cat.1 codes to
16 KiB on Cat.5 and Cat.6. Firmware reads it as memory and writes it through the FLASH unit with
`FLASH_PECR.DATA` set. A byte, halfword or word write takes one programming time when
`FLASH_PECR.FTDW` is clear and the word reads 0, else two. A word store of 0 erases the word.
ST's `HAL_FLASHEx_DATAEEPROM_Program` and `HAL_FLASHEx_DATAEEPROM_Erase` run on a machine. A
never written byte reads 0, and the host reads the EEPROM through `machine.mem`.

A double word erase or write locks its bank against reads until the programming time ends, as a
half page sequence does. [Limits](limits.md) lists the timing, the lock rules and the RDP level 1
regression that erases the data EEPROM.

## Memory map

| Region | Start | Size |
|---|---|---|
| Flash | 0x08000000 | the flash size of the code |
| Data EEPROM | 0x08080000 | 2 to 16 KiB |
| SRAM | 0x20000000 | 4 to 80 KiB |
| Option bytes | 0x1FF80000 | 1 KiB |
| System memory | 0x1FF00000 | 4 or 8 KiB |

Cat.6 codes carry two flash regions, `Flash` of 192 KiB at 0x08000000 and `Flash_Bank2` of
192 KiB at 0x08040000. The 64 KiB between them is unmapped.

## Option bytes

`Machine(option_bytes=OptionBytes(...))` takes one key on the STM32L1:

| Key | Accepted | Effect |
|---|---|---|
| `swap_bank` | Cat.4, Cat.5 and Cat.6 codes | boot from the bank 2 base, and `FLASH_OBR.nBFB2` loads as 0 |

Every other field stays at its default. A field away from its default raises `ConfigError` keyed
`option_bytes.<field>`, and so does `swap_bank` on a code with one bank.

```python
from popcorn import Machine, OptionBytes
from popcorn.stm32 import mcu

machine = Machine(mcu("STM32L152RETx"), option_bytes=OptionBytes(swap_bank=True))
```

Without `option_bytes` the machine keeps the option words of the description and boots from
address 0, where the flash is aliased.

## Examples

The 12 examples of `examples/stm32l152` run on the STM32L152RETx. The `gpio`, `pwr_flash`, `rcc`
and `usart` examples also run on the Cat.1 STM32L152RBTx, and the gate runs the examples again on
further Cat.2 and Cat.3 codes. The state column of the parts page names the evidence behind each
code.
