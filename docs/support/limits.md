# Limits

A built-in model serves the registers and behaviour the firmware of the examples and the tests
use. It does not serve every field of every register. Each section below lists what its model
leaves out and the deviations from the reference manual, RM0456 for the STM32U5, that its source
documents. The class doc in the linked source has the detail.

Most models meet an unmodelled setting with one `model_warning` log record per instance, then
keep running as the section says. Under `strict` each such warning raises `ModelError` instead.
The MDF and ADF model raises `UnmodelledConfig`. Read the log records of a run to see which
settings a firmware reached.

On the STM32H5 a Machine runs the STM32H562, H563 and H573. The STM32H503, H523 and H533
descriptions build from their SVD, and attaching a Machine to them raises `ConfigError`. The
STM32H543 and H553 descriptions need ST's DIE47C rzone file to build.

## Peripherals without a model

The [part pages](generated/parts.md) list the instances with no model.

- A firmware access to a register of such a peripheral logs one `model_warning` per register.
  Under `strict` it raises `ModelError`.
- A host access over the bus, through `m.mem` or a register's `read()` and `write()`, raises
  `ModelError` and logs nothing, with or without `strict`.
- The `value` getter, the field getters and `poke()` reach the register storage without a model.
- An offset no register covers inside a modelled peripheral counts as unmodelled data too.
- A firmware or DMA access to an offset no register covers, in a peripheral with or without a
  model and held in RCC reset or not, reads 0, drops a write and logs one `model_warning` per
  offset. Under `strict` it raises `ModelError`. A native model's or plugin's debugger access
  through the services there under `strict` reads 0, drops a write and logs nothing.
- The debug blocks TPIU, ETM, CTI and the ROM tables have no model. A firmware or DMA access
  reads 0 and drops writes, with one `model_warning` per block. The ROM tables read 0, so a ROM
  table walk ends at its first entry.

## Instances a package drops

A CubeMX part file lists the IPs its package brings out. An SVD instance that the part's file
leaves out takes the IP name and version of the die reference, the part of the same die and SVD
with the most bonded GPIO pins, then the largest flash, then the first RefName. The instance then
gets that IP's patches and built-in model, as on the reference. UART5 and SAI2 on the STM32U575
and U585 C packages and SPI4, UART8 and USART10 on the 64-pin STM32H5 parts are such instances.

- This rule is inferred from the die. Its supporting reading is on a STM32U575ZITxQ, an LQFP144
  that bonds no port I pin, where `GPIOI_MODER` reads 0x0000FFFF, the reset value of the 8 pins
  of port I: an unbonded block keeps its registers.
- An instance that no part of the die and SVD lists keeps the SVD group as its IP and an empty
  version, and has no model.

## Memory map

- On the STM32U5 and STM32H5 nothing is mapped at address 0. Under the default `faults="stop"`, a
  load, store or fetch there stops the run with a bus fault.
- The ICACHE remap of external memories onto the code region is not modelled. CRR0 to CRR3 store
  what the firmware writes and map nothing.
- On the STM32L1 and STM32L4 the main flash is aliased at address 0 over the size of the Flash
  region, RM0038 2.7 and RM0351 2.6.1, and VTOR resets to 0. The second bank of an STM32L1 Cat.6
  part reads at 0x00040000 to 0x0006FFFF, inferred. A CPU, DMA or debugger load and a fetch
  through the alias reach the flash under the PCROP, bank lock and read protection rules of the
  flash address, inferred: at RDP level 1 on the STM32L4 a debugger access through the alias is
  refused like one at 0x08000000. A CPU or DMA store through the alias is refused, a bus fault for
  the CPU and a transfer error for the DMA, inferred.
- SYSCFG_MEMRMP MEM_MODE, and FB_MODE on the STM32L4, are storage. Address 0 always maps the main
  flash.
- With `swap_bank` address 0 also maps the main flash. On silicon the system memory stays aliased
  at 0 with BFB2 set, RM0351 9.2.1, and with nBFB2 0, RM0038 Table 6.

## Core faults

Source: [exceptions.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/arch/armv8m/exceptions.cpp)

Under `faults="handler"`, a fault enters the firmware's handler as the core does, with these
exceptions. They stop the run with a FAULT stop instead:

- Lockup, and an INVSTATE fault.
- Without TrustZone: INVPC, UNSTKERR, STKERR, a STKOF on exception entry, VECTTBL, and the NOCP
  of an exception entry over an FP context or of a return to an extended frame.
- With TrustZone: a STKOF of the additional state context alone, and a fault while taking a
  derived exception.

[Faults and errors](../guide/faults.md) shows both policies.

## Reset requests

- Only a lone Python `Machine` applies the reset a RESET_REQUEST stop reported at its next
  run, step or free run. A C++ driver of `machine::run_slice` or `host::run` resumes the old code
  after the stop. A member of a linked system follows `on_member_reset`.
- A reset a store requests applies after the instructions that follow the store up to the next
  branch, as for AIRCR.SYSRESETREQ.

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

QUADSPI, DFSDM1, LCD, CAN1, COMP, OPAMP, TSC, SWPMI1, SDMMC1, FMC, FIREWALL, VREFBUF and WWDG
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

## STM32L1 description

Source: [patches](https://github.com/Garcia6l20/popcorn/blob/main/python/popcorn/stm32/description/patches)

The shipped STM32L1 descriptions start from the SVDs and the CubeMX part files. Patches correct
them from RM0038 and the CMSIS device headers. These corrections are stated by RM0038 or the
headers:

- The flash size of a code comes from its CubeMX part file, and RM0038 3.2 gives the category
  maximum only.
- `RCC_APB2LPENR` resets to 0x521D, RM0038 6.3.12, and `RCC_APB1LPENR` to 0xB0E64A37, RM0038
  6.3.13, with TIM5LPEN, SPI3LPEN, UART4LPEN and UART5LPEN at 0. The patch applies both to the
  whole family where the SVDs give 0.
- `SPI_CR2` resets to 0 on every category, RM0038 28.5.2, where the STM32L152 SVD gives 0x700.
- `TIM5_ARR` resets to 0xFFFFFFFF, the 32-bit view of RM0038 Table 82. RM0038 17.4.12 shows the
  16-bit view 0xFFFF.
- `CC2S` of `TIM2_CCMR1` and `CC4S` of `TIM2_CCMR2` are two bits wide, RM0038 17.4.7 and 17.4.8,
  where the SVD gives one.
- `TIM2_OR` and `TIM3_OR` reset to 0, RM0038 17.4.19 and 17.4.20.
- `FLASH_WRPR4` resets to 0 with its field named WRP4, on Cat.5 only, RM0038 3.9.9.
- `GPIOx_BRR` reads 0, RM0038 7.4.11.
- `F_SIZE` of DEV_ID 0x436 is 0 for 384 KiB and 1 for 256 KiB, RM0038 31.1.1.
- Cat.2 parts that are not value line carry 20 `RTC_BKPxR`, RM0038 20.2. The
  `stm32l152xba.h` header lists 5.
- Cat.3 on DIE427 and DIE436 has no UART4 and UART5, so the vectors 48 and 49 and the base
  0x40004C00 stay unused, RM0038 Table 50 and the CMSIS headers.
- GPIOF and GPIOG stay in the Cat.6 description with no bonded pin, because the CubeMX part
  files of DIE437 list none.
- The CMSIS device header suffix of a category, xb, xba, xc, xca, xd, xe and xdx, is read from
  the header contents.

These are inferred, not measured on silicon:

- A read past the populated flash inside the 128 KiB window of RM0038 Table 8, the Cat.1 and
  Cat.2 table, is unmapped.
- The 64 KiB hole of Cat.6 between the two 192 KiB banks, 0x08030000 to 0x0803FFFF, is unmapped,
  from RM0038 Table 12 and the CMSIS `FLASH_BANK2_BASE`. Silicon may fault or return a value.
- `SQ24` sits in bits 29 to 25 of `ADC_SQR2`, five bits wide. RM0038 12.15.11 text gives 29
  to 26 and the SVD is used.
- The device unique ID words are fixed values, because RM0038 31.2 documents no layout of the
  wafer and lot fields. The third word sits at offset 0x14, where the STM32U5 and STM32H5 use
  0x08.
- On Cat.1 and Cat.2 `F_SIZE` sits at 0x1FF8004C, inside the factory block 0x1FF80020 to
  0x1FF800FF of RM0038 Table 8.
- `FLASH_WRPR4` also exists on Cat.6 at 0x40023C88, from the CMSIS xdx headers. RM0038 3.9.9
  gives it to Cat.5 only, and whether a Cat.6 access reads zero, faults or returns an option byte
  value is not known.
- The Cat.3 vector positions 46 to 55 follow the vector addresses and the CMSIS header. The
  Position column of RM0038 Table 50 reads 45 to 52.

## ADC

Source: [adc.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/adc.cpp), [adc_l1.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/adc_l1.cpp)

Not modelled, logged once each:

- Auto-delayed conversions, `ADC_CFGR1.AUTDLY`.
- The low frequency trigger mode, `ADC_CFGR2.LFTRIG`.
- The MDF data path, `ADC_CFGR1.DMNGT` 10.
- DMA requests gated by the AWD1 window, `AWDFILT1` with `DMNGT`.
- Left shift, gain compensation and offset of the result.
- Bulb sampling, `ADC_CFGR2.BULB`.
- The sampling time control trigger mode, `ADC_CFGR2.SMPTRIG` with `SWTRIG`.
- Triggered regular oversampling, `ADC_CFGR2.TROVS`, on the STM32U5 and STM32H5: all samples of a
  result convert back to back from the first trigger.
- Differential conversions, the `ADC_DIFSEL` bit of a converted channel: the channel converts
  single-ended.
- Reserved trigger selections in `EXTSEL`.
- The EXTI line 11 and 15 selections, `EXTSEL` 6 and 16: the STM32U5 and STM32H5 EXTI drives no
  trigger signal, so they start nothing.
- A reserved prescaler in `ADC12_CCR.PRESC` divides by 256.
- ADC4 has no model.

Every family:

- An external channel converts the analog level of its pad, the GPIO pin its description's
  `channel_pins` names, an STM32L1 bank B channel on a pad of its own that of its bank B pad.
  Channels of several ADCs on one pad read one level: a drive through `ADCx.input(n).volts` is
  the pad's board drive, and every ADC channel on that pad converts it. A description without
  `channel_pins` gives its ADC no pads: drive and input of an external channel raise
  `ConfigError` naming `channel_pins`, and the channel converts 0. Every shipped description
  carries the table.

On the STM32U5, STM32H5 and STM32L4:

- Writes to `ADC_CFGR`, `ADC_CFGR2`, `ADC_SMPRx`, `ADC_SQRx`, `ADC_OFRy`, `ADC_TRx` and
  `ADC_AWDxCR` with ADSTART or JADSTART set apply at once.

On the STM32H5, RM0481 section 28:

- The dual ADC modes, `ADC_CCR.DUAL` other than 0, are logged once. ADC1 and ADC2 convert
  independently. `ADC_CDR` is storage. `ADC_CSR` reads the `ADC_ISR` bits 10:0 of ADC1 at its
  bits 10:0 and of ADC2 at its bits 26:16.
- An enabled offset, `ADC_OFRy.OFFSET_EN` on the converted channel, is logged once and leaves
  the result unchanged.
- ADC2 channels 16 and 17, VBAT / 4 and VDDCORE, read 0.
- Channel 0 of ADC1 or ADC2 converted while ADC1 `ADC_OR.OP0` is clear is logged once, a
  model_error under strict_peripherals, and converts as 0. An STM32H573 reads an internal level
  there, not PA0.
- ADC2 `ADC_OR.OP0`, the VDDCORE channel enable, is storage.
- In the synchronous clock modes the ADC clock is HCLK divided by CKMODE, whatever ADC12EN.
  CKMODE 01 with HCLK not SYSCLK is logged once at the ADEN edge, a model_error under
  strict_peripherals, and converts at HCLK.
- Out of deep power-down, ADRDY sets 35 HCLK cycles after the ADEN edge with ADVREGEN set and 45
  with it clear, as measured on an STM32H573 with the ADC clock at HCLK. The cycle count is kept
  in HCLK cycles at every ADC clock. The tADCVREG_STUP wait of DS14121 Table 96 is the
  firmware's.
- Under OVRMOD 0 a result that finds the three-result FIFO full sets OVR and leaves the head
  only. While OVR stays set, a result lands when the FIFO is empty and is discarded otherwise.
  Measured on an STM32H573, the discard of a second result while the FIFO holds one is the
  reading consistent with that run.
- The end of a one-shot DMA transfer, DMACFG 0, clears ADSTART and stops the conversions, as
  measured on an STM32H573 with CONT 1.
- At CKMODE 00 a trigger waits for the next ADC clock edge, and tLATR is 4.5 ADC clock cycles
  from that edge at every prescaler, as measured on an STM32H573 at /1, /2, /128 and /256.
  DS14121 Table 96 gives 2 cycles. The ADC clock edges fall on multiples of the ADC clock period
  from virtual time 0.

On the STM32L1, RM0038 section 12:

- The automatic power-down, `PDI` and `PDD`, and `DELS` with `JAUTO` are logged once, a
  model_error under strict_peripherals.
- The analog watchdog compares an injected code before its offset, a code below 12 bits shifted
  to 12 bits, and a result an overrun loses, each inferred. `ADC_HTR` and `ADC_LTR` are read at
  the end of each conversion.
- With `SCAN` and `AWDIE` set, a conversion that sets `AWD` aborts the scan, RM0038 12.15.2. Its
  result is the group's last: `EOC` or `JEOC` sets, `CONT`, `DISCEN` and `JAUTO` do not go on,
  and the next start begins at the first rank, inferred.
- With `DMA` 1, `adc_dma` on DMA1 channel 1 requests each regular result until `ADC_DR` is read
  or 0 is written to `EOC`. An overrun with `DMA` 1 aborts the regular conversion and further
  regular triggers are ignored, RM0038 12.11.1. They stay ignored until `OVR` is cleared,
  `SWSTART` counting as a trigger, inferred. With `DDS` 0 the end of the channel's count while
  `DMA` is 1, at the end of a normal channel or at a circular reload, stops the requests and the
  ADC converts as with `DMA` 0, no overrun, until `DMA` is written 0, then 1, inferred from the
  RM0038 12.11.1 "avoids generating an overrun error".
- Hardware triggers start conversions from every selection of `EXTSEL` and `JEXTSEL`: the TRGO
  selections from the timer's TRGO, the TIMx_CCy selections from the timer's capture/compare
  event, and EXTI lines 11 and 15 from the line input, a pin edge counting at its own time,
  inferred. A selection whose source has no model on the part is logged once, a model_error
  under strict_peripherals, and starts nothing. tlatr is 3.5 ADC clock cycles, the datasheet
  maximum. Trigger detection stays on across an `EXTSEL` change, RM0038 12.6 blanks it
  for 2 PCLK cycles. A model_error of a selection surfaces from the `ADC_CR2` write or the `ADONS`
  event that starts the listening.
- `DELS` 010 to 111 add 1 PCLK2 cycle and 3 ADC clock cycles of synchronisation to the delay, the
  maxima RM0038 gives, inferred, at the PCLK2 rate of the conversion end. `DISCEN` with `CONT`
  converts as `DISCEN` alone and is logged once, a model_error under strict_peripherals, inferred.
  With `EOCS` 1 the `DELS` 010 to 111 delay follows each group though `EOC` sets per conversion,
  RM0038 12.9.1.
- `DELS` 001 holds the next conversion at each `EOC`: after each conversion with `EOCS` 1, after
  the group with `EOCS` 0, inferred for `EOCS` 0. A trigger or `SWSTART` during a `DELS` hold is
  ignored, RM0038 12.9 and Figure 47.
- `ADONS` sets 3.5 us after the `ADON` edge, the datasheet tSTAB maximum, and clears at once
  with `ADON`. `RCNR` and `JCNR` read 0. The ADC runs from HSI16 whatever `ADC1EN` of
  `RCC_APB2ENR` holds, which gates only its register access, see RCC.
- A `SWSTART` while a conversion runs is cleared and ignored, inferred. A `JSWSTART` or an
  injected trigger while the injected group runs is cleared and ignored, inferred.
- With `SCAN` 0 the injected group is one conversion of JSQ(4 - `JL`), the first of the `JL`
  order, also over a regular conversion, inferred. An `ADC_JSQR` write, or an `ADC_SQR1` to
  `ADC_SQR5` write, between `JDISCEN` or `DISCEN` subgroups starts the next group at its first
  conversion, inferred. The k-th conversion of the injected group lands in `ADC_JDRk` less the offset
  of `ADC_JOFRk`, inferred from the HAL rank order. Below 12 bits `JOFFSET` counts from bit 11 and
  its bits under the resolution are ignored, inferred from the LL driver note, and a left-aligned
  injected result sits one bit below the regular one at every resolution, RM0038 Figures 45 and
  46 for 12 and 6 bits, inferred for 10 and 8 bits.
- An injection drops a regular conversion in progress or due, and the same rank converts again
  after the injected group, once the delay of the regular conversion before it has passed. A
  regular trigger or `SWSTART` during the injected group starts the regular group after it. One
  edge of a signal both groups select starts both.
- `JAUTO` with `JEXTEN` not 0, `DISCEN` or `JDISCEN`, a `JSWSTART` with `JAUTO`, and `DISCEN`
  with `JDISCEN` are logged once, a model_error under strict_peripherals. With `JAUTO` the whole
  injected group follows the last regular rank and injected triggers are ignored, inferred.
- With `JDISCEN` the last subgroup sets `JEOC` alone, inferred: the RM0038 12.3.10 example also
  names `EOC`. The injected tlatr is the regular 3.5 ADC clock cycles, inferred.
- Channels 16 and 17 read their TS_CAL1 and VREFINT_CAL values from the `TSVREFE` write on,
  with no sensor tSTART, and 0 while `TSVREFE` is clear, inferred.
- A conversion keeps the end time, sampling end and resolution it started with. `RES` and `SMPx`
  written with `ADON` 1 apply from the next conversion.
- With `EOCS` 1, a result that finds `ADC_DR` unread sets `OVR` and is lost, and the group goes
  on, inferred.
- On Cat.3 to Cat.6, the bank B channels on a pad of their own, ADC_IN0b to ADC_IN3b and
  ADC_IN6b to ADC_IN12b, read their bank B input, `ADC.input(n, bank="B")`. The other bank B
  channels read their bank A pad and take no bank B drive. VCOMP, channel 26, reads 0, logged
  once.
- A package bonds a bank B channel on a pad of its own when it bonds that pad, RM0038 Table 41
  and the DS10002, DS10764, DS8890, DS10262 and DS8576 pin tables: PB2 for ADC_IN0b on every
  Cat.3 to Cat.6 package.
- The factory TS_CAL1, TS_CAL2 and VREFINT_CAL words are datasheet typical values. TS_CAL1 is
  derived from two typicals, inferred. The L100 parts carry the words though their datasheets
  give no TS_CAL, inferred.

On the STM32L47x/L48x, RM0351 section 18, ADC1, ADC2 and ADC3 run the STM32H5 model:

- The dual ADC modes, `ADC_CCR.DUAL` other than 0, are logged once, a model_error under
  strict_peripherals. ADC1, ADC2 and ADC3 convert independently. `ADC_CDR` is storage.
  `ADC_CSR` reads the `ADC_ISR` bits 10:0 of ADC1 at its bits 10:0 and of ADC2 at its bits
  26:16, RM0351 18.8.1. ADC3 has no `ADC_CSR` bits.
- `ADC_DR` holds one result. Under OVRMOD 0 a result that finds EOC set sets OVR and is
  discarded, and so is every result while OVR stays set, RM0351 18.4.26.
- An `ADC_OFRy` with OFFSET_EN subtracts OFFSET from the code of its channel, left-aligned on 12
  bits, RM0351 Table 114, the lowest enabled y first, inferred from RM0351 18.7.17, whose lowest-x
  rule would otherwise let a disabled `ADC_OFR1` at its reset channel 0 block the others, for
  `ADC_DR` and `ADC_JDRy`, and the result reads signed, RM0351 Figures 97 and 99. ROVSE and JOVSE
  ignore OFFSET_EN. OFFSET bits below the resolution, which Table 114 requires clear, are subtracted
  too and the result keeps its upper bits, inferred. The watchdogs compare the raw code.
- The one-shot DMA end, DMACFG 0, drops the conversion in progress and stops the sequence with
  ADSTART left set, RM0351 18.4.15, and freezes `ADC_DR`, RM0351 18.4.26. A result while frozen
  sets no OVR, RM0351 18.4.26 "This avoids generating an overrun error". It leaves EOC
  unchanged, sets EOS at the end of the sequence and still reaches the watchdogs, inferred. ADSTART
  going from 0 to 1 re-arms the request and ends the freeze, inferred.
- ES0250 2.8.8, ADC_AWDy_OUT reset by unguarded channels, has no counterpart: no watchdog output
  reaches a timer, and the TIM1 and TIM8 `ETR_ADCx_RMP` selections log once, see TIM.
- ADRDY sets 15 ADC clock cycles after the ADEN write, the DS10198 Table 76 tSTAB of one
  conversion cycle read as one 12-bit conversion at SMP 000, inferred. The tADCVREG_STUP wait of
  Table 76 is the firmware's.
- A calibration keeps ADCAL set for 116 ADC clock cycles, the DS10198 Table 76 tCAL, and leaves
  CALFACT_S or CALFACT_D at 0, inferred: popcorn converts with no offset error. Without a kernel
  clock it waits for one. An ADEN write within 4 ADC clock cycles after ADCAL clears is dropped,
  RM0351 18.4.9 as corrected by ES0250 2.8.4.
- A channel converted with the `GPIOx_ASCR` switch of its pad open reads 0, inferred, logged once,
  a model_error under strict_peripherals.
- ES0250 2.8.11 and 2.8.12, a wrong first result after 1 ms idle and spurious temperature
  samples, have no counterpart: every result is the converted input. ES0250 2.8.18, the clamp of
  an ADC input to VDD, has none either: inputs saturate at VDDA. ES0250 2.2.4, the VREFINT
  corruption at Stop entry, has none: Stop is not modelled.
- With ROVSE, AWD1 compares `ADC_DR[15:4]`, RM0351 18.4.30. AWD2 and AWD3 compare `ADC_DR[15:8]`,
  the STM32H5 rule, inferred: RM0351 18.4.29 and 18.4.30 give no oversampled rule for them.
- VBAT / 3 on channel 18 of ADC1 and ADC3 reads 0 with `CH18SEL`.
- The ADC2 channels 17 and 18 and the ADC3 channels 14 and 15 convert the on-chip outputs of DAC1
  channels 1 and 2, RM0351 18.4.11, and read 0 while that output reaches no on-chip peripheral.
  They need no `ADC_CCR` enable, RM0351 18.4.11 naming VREFEN, CH17SEL and CH18SEL only.
- The trigger from EXTI line 11 reaches the ADC whatever the EXTI interrupt configuration, ES0250
  2.8.13 requires the line enabled as an interrupt and its pending bit cleared.
- LDM, STM, LDRD and STRD accesses to the ADC registers complete as single accesses, ES0250 2.8.3
  and 2.8.15 list them as not supported.
- ES0250 2.8.7 and 2.8.10, an unexpected regular conversion after two injected conversions and a
  shifted slave datum, have no counterpart: the dual modes are not modelled.
- ES0250 2.8.9, injected data stored in ADC_JDR1 when JADSTP lands at the end of an injected
  conversion, has no counterpart: every injected result goes to its own ADC_JDRy.
- `EXTSEL` 8 and 10, TIM8_TRGO2 and TIM1_TRGO2, have no source and are logged once, a model_error
  under strict_peripherals. `EXTSEL` 6 takes EXTI line 11 from the line input, a pin edge
  counting at its own time.
- At CKMODE 00 a trigger waits for the next ADC clock edge, counted from virtual time 0, then
  tLATR is 1.5 ADC clock cycles, so it lies in the 1.5 to 2.5 cycles of DS10198 Table 76, inferred.
  At CKMODE 01, 10 and 11 tLATR is the Table 76 maximum, 2, 2.25 and 2.125 cycles.
- The factory TS_CAL1 and VREFINT_CAL words are the datasheet typical values of the L4 patch,
  inferred.
- The injected group converts with JQDIS 1. JADSTART with JEXTEN 0 converts the JSQ channels at
  once, with JEXTEN set each edge of the JEXTSEL event of RM0351 Table 112 converts them after the
  tLATR of a regular trigger. With JDISCEN and JEXTEN 0 each JADSTART converts the next channel and
  JADSTART clears after it, inferred: RM0351 18.4.20 describes JDISCEN with hardware triggers only.
- JOVSE oversamples the injected ranks with OVSR and OVSS, RM0351 18.4.30, and AWD1 compares
  `ADC_JDRy[15:4]`, the `ADC_DR[15:4]` rule of RM0351 18.4.30, inferred. With ROVSE and JOVSE an
  injection restarts the dropped regular rank from its first sample, RM0351 Figure 115, and JAUTO
  with both oversamples both groups, Figure 117.
- With ROVSE, ROVSM 0 and JOVSE 0 an injection keeps the samples the dropped rank completed, and the
  rank converts its remaining samples after the group, the sample in progress again, RM0351 18.4.30
  Figure 114. TROVS with ROVSE converts one sample per trigger and lands the result after 2 to the
  OVSR + 1 triggers, Figure 113, with DISCEN read as 1 and one rank per trigger. An injection
  between triggers zeroes the samples taken, and a sample it drops converts again at the end of the
  group with no trigger, RM0351 Figure 116. ADSTP between triggers zeroes them too, inferred. With
  EXTEN 0 each ADSTART converts one sample and ADSTART clears after it, inferred. The samples taken
  survive an `ADC_CFGR2`, `ADC_SQRx` or `ADC_SMPRx` write while ADSTART reads 0 between them, and
  the result then sums samples taken under both settings, inferred.
- TROVS with CONT is the DISCEN and CONT pair RM0351 18.4.20 forbids, as RM0351 18.4.30 reads DISCEN
  as 1 under TROVS. It is logged once, a model_error under strict_peripherals, and converts as
  DISCEN alone.
- These oversampling settings are logged once, a model_error under strict_peripherals: ROVSE and
  JOVSE with ROVSM 0 and JAUTO 0 convert in the resumed mode, as the RM0351 18.4.30 text says ROVSM
  is ignored there, and are logged because Table 119 marks the setting not supported, and TROVS with
  ROVSM 0, and TROVS with JOVSE outside JAUTO, not supported by Table 119, convert one sample per
  trigger in the resumed mode. TROVS with JAUTO is logged as not modelled. A strict model_error at a
  JADSTART write leaves `ADC_CR` as before the write.
- With JQDIS 0 a CPU or DMA `ADC_JSQR` write goes through the queue of context with one stage at JQM
  0, ES0250 2.8.1, where RM0351 18.4.21 describes two. A write before the waiting context is
  consumed sets JQOVF, an interrupt with JQOVFIE, and is ignored. `ADC_JSQR` reads the active
  context. JEOS consumes it, and with JDISCEN the first trigger does, RM0351 18.4.21 note: a write
  after that waits and becomes the context at JEOS. Otherwise a write after the consumption applies
  at once, RM0351 Figure 84, and so does the first write after JADSTP or ADDIS drop the wait,
  Figures 86, 88 and 90, or after ADCRST, inferred. A set or reset of JQDIS flushes the queue and
  clears `ADC_JSQR`, RM0351 18.7.4. A debugger write stores the word with no queue effect. Only
  hardware triggers start a sequence with the queue on, RM0351 18.4.21: JADSTART with JEXTEN 0 is
  ignored and logged once, a model_error under strict_peripherals.
- With JQM 1 the queue holds two contexts, RM0351 18.4.21. A write to the empty queue applies at
  once, a later one waits, and a third sets JQOVF. JEOS applies the next context or empties the
  queue, which then reads `ADC_JSQR` 0, RM0351 Figures 85, 89 and 91, and ignores the injected
  triggers until a write. JADSTP, ADDIS and a JQDIS change empty it.
- ES0250 2.8.5 and 2.8.6, JQM 0: a new context converts at its own trigger, and a longer context
  written just after the previous one completes converts its own ranks. Silicon may start the
  new context without its trigger, or merge the two contexts.
- With JAUTO the injected group converts after the last regular rank, RM0351 18.4.19, with no
  delay, inferred, and with CONT the regular sequence starts again at JEOS. With CONT 0 and
  EXTEN 0 ADSTART clears at EOS, as the RM0351 18.7.3 ADSTART bit says, and JADSTART reads 0 while
  the auto-injected group converts, inferred: the RM0351 18.4.15 note says ADSTART and JADSTART
  "provide information on whether any ADC operation is currently ongoing". ADSTP stops both
  groups, RM0351 18.4.17. The one-shot DMA transfer end stops the sequence and drops the
  conversion in progress, RM0351 18.4.19 and 18.4.26, and clears ADSTART, inferred.
- JADSTART or JADSTP with JAUTO is ignored, JAUTO with JEXTEN set ignores the injected triggers,
  JAUTO with DISCEN or JDISCEN converts as JAUTO alone, and JAUTO with ROVSE 1 and JOVSE 0
  oversamples the regular ranks only, RM0351 18.4.30. Each is logged once, a model_error under
  strict_peripherals, RM0351 18.4.19 and 18.7.4.
- An injected start or trigger during a regular conversion drops it, and the dropped rank converts
  again after the group, RM0351 18.4.19, starting at the group's end, inferred. tLATRINJ is the
  next ADC clock edge then 2.5 cycles at CKMODE 00, inside the 2.5 to 3.5 cycles of DS10198 Table
  76, inferred. At CKMODE 01, 10 and 11 it is the Table 76 maximum, 3, 3.25 and 3.125 cycles.
- A regular start with EXTEN 0 or a regular trigger during the injected group starts the regular
  sequence at the group's end, RM0351 18.4.19, at once with no tLATR, inferred. ADSTART with
  EXTEN set arms the trigger. ADSTP during the group cancels the dropped rank and the latched
  start, and the injected group runs on. JADSTP during the group resumes the dropped rank at once,
  inferred.
- `JEXTSEL` 8 and 10, TIM1_TRGO2 and TIM8_TRGO2, have no source and are logged once, a
  model_error under strict_peripherals. `JEXTSEL` 6 takes EXTI line 15 from the line input.
- An `ADC_JSQR` write with JADSTART 1 and JQDIS 1 applies to the next injected sequence and is
  logged once, a model_error under strict_peripherals, RM0351 18.7.16 as corrected by ES0250 2.8.2.

## CRC

Source: [crc.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/crc.cpp)

- On the STM32U5, STM32H5 and STM32L4 a `CRC_DR` write that is not right-aligned is ignored and
  logged, RM0456 24.4, RM0351 15.4.
- An even polynomial, which RM0456 24.3.3 and RM0351 15.3.3 do not support, is computed anyway and
  logged.
- A write narrower than 32 bits to any register but `CRC_DR`, and on the STM32L1 and STM32L4
  `CRC_IDR`, is ignored and logged. Such a read is logged.
- The STM32L1 CRC, RM0038 4, computes CRC-32 over the fixed polynomial 0x04C11DB7 from 0xFFFFFFFF,
  with no INIT, POL or reversal. A byte or half-word CRC_DR write feeds the word its byte lanes
  repeat, 0xB0B0B0B0 for the byte 0xB0, and logs once. For a DMA write RM0038 11.3.4 states the
  repeated lanes. For a CPU store the repeated lanes, and the unit ignoring the access size, are
  inferred. A DR write completes with no stall, where RM0038 4.3 stalls it for the 4 HCLK of the
  computation. A narrow read of CRC_DR or CRC_CR and a narrow write to CRC_CR log once, as RM0038
  4.4 asks for words. RM0038 4.4 says "The CRC registers have to be accessed by words (32 bits)",
  CRC_IDR included. The model takes byte and half-word CRC_IDR accesses with no log, as ST's CMSIS
  header declares IDR a byte and LL_CRC_Write_IDR stores one. Under `strict` each logged access
  raises `ModelError`.
- The STM32L4 CRC, RM0351 15, is the programmable unit of the STM32U5 with an 8-bit CRC_IDR, RM0351
  15.4.2. RM0351 15.3.3 and 15.4 allow only 32-bit accesses to registers other than CRC_DR, "For
  the other registers only 32-bit accesses are allowed". The model takes byte and half-word
  CRC_IDR accesses with no log, as ST's CMSIS header declares IDR a byte and LL_CRC_Write_IDR
  stores one.

## CRS

Source: [crs.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/crs.cpp)

- A SYNC event is a write of 1 to `SWSYNC`, or a USB start of frame from the OTG_FS (STM32U5)
  or the USB (STM32H5) while `SYNCSRC` is 2, divided by 2^`SYNCDIV`. Only `SWSYNC` and the
  USB SOF give a SYNC, and only while `CEN` is set. Setting `CEN` with `SYNCSRC` 0, 1 or 3 logs
  one warning per model life. `SYNCPOL` is storage.
- The frequency error counter runs at exactly 48 MHz. The first SYNC after `CEN` is set loads
  `RELOAD` and sets no flag. Each later SYNC latches `FECAP` and `FEDIR` and sets `SYNCOKF`,
  `SYNCWARNF`, or `SYNCERR` with `ERRF`, by the thresholds of RM0456 12.4.5, then reloads.
  `ESYNCF` is set `RELOAD` + 1 cycles after a SYNC, so the `RELOAD` of RM0456 12.4.6 gives
  `FECAP` 0. When no SYNC comes, `SYNCMISS` and `ERRF` follow 128 x `FELIM` cycles later and
  the counter stops, and the next SYNC latches 128 x `FELIM`. With `SYNCSRC` 2, `RELOAD` 47999
  matches `SYNCDIV` 0 only.
- `TRIM` keeps what the firmware wrote, with or without `AUTOTRIMEN`, and `TRIMOVF` stays 0.

## DAC

Source: [dac.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/dac.cpp)

STM32L4 DAC1, RM0351 section 19, two 12-bit channels on the APB1 clock.

- A holding register write stores DHR1 and DHR2 by the shifts of RM0351 19.4.3. Every holding
  register reads its view of DHR1 and DHR2, inferred.
- With TENx 0, DHRx loads into `DAC_DORx` one APB1 cycle after the write, RM0351 19.4.4. The
  cycle counts from the access time at the APB1 rate of the write, inferred. With TENx 1 and TSELx
  111, each SWTRIGx write loads `DAC_DORx` one APB1 cycle later, RM0351 19.4.6. SWTRIGx follows
  the `w` access of RM0351 19.7.2 and reads 0, inferred against the 19.4.6 text of a bit reset by
  hardware at the load. Written with TENx 0 or another TSELx, it loads nothing, inferred. Loads
  and triggers run with ENx 0, RM0351 19.4.2 note.
- With TENx 1 and TSELx below 111, each rising edge of the RM0351 Table 128 source loads
  `DAC_DORx` three APB1 cycles after the edge, RM0351 19.4.4 and 19.4.6: `tim6_trgo`,
  `tim8_trgo`, `tim7_trgo`, `tim5_trgo`, `tim2_trgo`, `tim4_trgo` and `exti9`. A falling edge
  loads nothing. The three cycles count from the exact instant the source gives the edge. Each
  load takes DHRx as it is at the load, inferred.
- The model keeps three pending loads per channel, in order. A fourth before the first load moves
  the third to its own due. Three cover the APB1 timer sources TIM2, TIM4, TIM5, TIM6 and TIM7:
  an update needs ARR 1 or more, RM0351 33.4.8, so it comes at most once per APB1 cycle. TIM8 on
  APB2 and EXTI9 can give edges closer than that, and then the fourth-edge rule applies.
- Popcorn charges no APB wait states, so writes closer than one APB1 cycle happen only in the
  model. In that window a load takes DHRx as it is at the load, and a write while a load is
  pending takes the next of the three slots.
- While ENx is 1, a `DAC_CR` write keeps the old TSELx, RM0351 19.4.6 note, the old MAMPx, RM0351
  19.4.9, and the old CENx, RM0351 19.7.1. `DAC_MCR` keeps the old MODEx while ENx or CENx is 1,
  RM0351 19.7.16. ENx set while CENx is 1, which RM0351 19.4.11 does not allow, and WAVEx 11,
  reserved by RM0351 19.7.1, keep the write. Each logs one `model_warning` per rule. Under
  strict_peripherals it raises `ModelError` and leaves the register as before the write.
- With CENx 1, CAL_FLAGx reads 1 once OTRIMx is at or above the factory trim and MODEx enables
  the buffer, RM0351 19.4.11. It sets at the `DAC_CCR` write, with no tTRIM delay. With a buffer-off
  MODEx it reads 0, and with CENx 0 it keeps its value, inferred. The factory trim is 16 on both
  channels, inferred, as RM0351 19.7.15 gives 0x00XX 00XX. `DAC_CCR` resets to it.
- With DMAENx 1, a load from a hardware trigger raises `dac1_ch<x>_dma` at the load instant,
  RM0351 19.4.7, the instant inferred. A load from a holding register write or SWTRIGx raises none.
  The request drops when a DMA transfer serving it acknowledges it, or when DMAENx is cleared,
  inferred. A hardware load while the request is high raises no new request and sets DMAUDRx,
  RM0351 19.4.7, decided at the load, inferred, and `DAC_DORx` still loads DHRx.
- DMAUDRx with DMAUDRIEx holds the DAC's share of `TIM6_DAC_IRQn`, position 54, which TIM6 also
  drives.
- ES0250 2.9.1, the DMA underrun flag missed for an internal trigger on the cycle of the DMA
  acknowledge, applies only with software and hardware triggers mixed. Popcorn sets DMAUDRx by the
  rule above.
- WAVEx 01 adds the LFSR of RM0351 Figure 141, masked to the MAMPx + 1 low bits, at each SWTRIGx
  or hardware load, then steps it, RM0351 19.4.8 and 19.4.12, from the preload 0xAAA. WAVEx 1x
  adds the triangle counter, then steps it up to 2^(MAMPx + 1) - 1 and back down to 0, RM0351
  19.4.9, 19.4.12 and Figure 144. RM0351 19.4.9 reads the increment before the add, and the model
  follows 19.4.12 and Figure 144. A MAMPx above 1011 acts as 1011. The sum saturates at 0xFFF,
  the reading of "without overflow", inferred. WAVEx 00 resets both generators.
- The LFSR runs the 4095 states from 0xAAA and never reaches 0, so the antilock-up injection of
  RM0351 19.4.8 never applies.
- The RM0351 19.4.8 and 19.4.9 notes require TENx for a wave. A holding register write with TENx 0
  and WAVEx set logs one `model_warning` and loads DHRx with no wave, the load inferred. Under
  strict_peripherals it raises `ModelError` and leaves the holding registers as before the write.
  A `DAC_CR` write that sets WAVEx with TENx 0 is accepted, as `LL_DAC_Init` writes WAVEx before
  `LL_DAC_EnableTrigger` sets TENx.
- Each accepted `DAC_SHSRx` write sets BWSTx, RM0351 19.7.14, and so does a `DAC_MCR` write that
  sets MODEx 1xx, the sample and hold mode, the MCR write as the instant inferred. BWSTx clears 3
  LSI periods later, RM0351 19.7.14 "about 3 LSI periods", the 3 inferred. While the LSI is off
  BWSTx stays set, a count already running included, and it clears 3 LSI periods after the LSI
  starts.
- A `DAC_SHSRx` write while ENx and BWSTx are both 1 is ignored, RM0351 19.7.17. A THOLDx or TREFRESHx write
  while ENx or CENx is 1 is ignored, RM0351 19.7.19 and 19.7.20. MODEx 1xx set with the LSI off is
  not allowed, RM0351 19.4.10. Each logs one `model_warning` per rule. Under strict_peripherals it
  raises `ModelError` and leaves the register as before the write. The LSI stopping while a MODEx
  is 1xx logs the same warning once, under strict_peripherals too, since the stop comes from an RCC
  write whose clock change every other listener also sees.
- A sample and hold channel loads `DAC_DORx` as a normal one does, and runs the phases of RM0351
  19.4.10 in LSI periods: a sample phase of TSAMPLEx + 1, RM0351 19.7.17, then hold phases of
  THOLDx and refresh phases of TREFRESHx in turn, the alternation inferred. The sample phase starts
  at ENx set, at each DHRx change, RM0351 19.4.10, and at each `DAC_DORx` load, inferred. A write
  of the value DHRx already holds is no change, inferred. Each phase length counts from the end of
  the one before. A phase of length 0 is skipped, and with THOLDx and TREFRESHx both 0 the channel
  stays in refresh with no further phase. While the LSI is off the phase in progress holds, and it
  restarts at its full length when the LSI starts, inferred. ENx 0 stops the phases.
- The DAC stays enabled through a Stop 2 entry with no diagnostic, though RM0351 19.5 Table 131
  asks for it to be disabled first. The PWR model logs the low-power mode and models no Stop entry
  for a peripheral to see.
- With ENx 1 a channel drives its pad, PA4 or PA5, RM0351 19.3, when MODEx connects the pad,
  000, 001, 010, 100, 101 or 110, RM0351 Table 130, at `DAC_DORx` x VDDA / 4096, rounded up to
  the next microvolt so that an ADC at the same VDDA converts `DAC_DORx` back, VDDA from
  `SupplyOptions` taken as VREF+. The level follows each `DAC_DORx` load at once, with no
  tSETTLING or tWAKEUP, and the buffer state changes no level, inferred.
- A sample and hold channel drives the `DAC_DORx` value held at the end of each sample and refresh
  phase, through the hold phase, in which RM0351 19.4.10 tri-states the output and the hold
  capacitor keeps the voltage, inferred. From ENx set to the end of its first sample phase it
  leaves the pad, inferred. The RM0351 Table 129 settling formulas do not enter the level.
- MODEx 011 and 111, ENx 0, DAC1RST and a machine reset release the pad. The ADC channels on the
  pad convert the DAC level, and it wins over a board drive of the pin with one warning per pin.
- MODEx 001, 011, 101, 110 and 111 connect the channel on chip, RM0351 Table 130: ADC2 channels 17
  and 18 and ADC3 channels 14 and 15 convert the same level, the held one in sample and hold
  mode, MODEx 111 holding it on the internal CLint. MODEx 000, 010, 100, ENx 0, DAC1RST and a
  machine reset disconnect it, and those channels then read 0.
- In Stop 0 and Stop 1 a channel keeps its level and a sample and hold channel runs its phases, as
  RM0351 Table 131 states for the sample and hold mode, since popcorn models no Stop entry.

## DBGMCU

Source: [dbgmcu.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/dbgmcu.cpp)

- A freeze bit written while the core is held takes effect at the next hold or release. On
  silicon it takes effect at once.
- STM32L4: the U5 and L1 model, RM0351 48.16. `DBGMCU_IDCODE` reads 0x10076415, `DBGMCU_CR` is
  storage, and the freeze fields carry the RM0351 names, `DBG_TIM2_STOP` and others. A power-on
  reset clears the registers, a system reset keeps them, 48.16.3 to 48.16.6.
- STM32L1: `DBGMCU_CR` is storage, as on the STM32U5. `DBGMCU_IDCODE` reads the description's
  value of the category. REV_ID is 0x1000 on every category, the revision A of RM0038 30.6.1,
  and the choice of one revision is inferred.

## EXTI

Source: [exti.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/exti.cpp), [exti_classic.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/exti_classic.cpp)

- STM32L1: `EXTI_EMR` is storage, and an EXTI event wakes no WFE.
- STM32L1: lines 16, 17, 18, 20 and 23 set their PR bit only, from `EXTI_SWIER`. The
  description gives `PVD_IRQn`, `RTC_Alarm_IRQn`, `USB_FS_WKUP_IRQn`, `RTC_WKUP_IRQn` and
  `COMP_ACQ_IRQn` to PWR, RTC, USB and COMP. Lines 16 to 23 take their edges from `EXTI_SWIER`
  only, the PVD, the RTC, the USB and the comparators drive none.
- STM32L1: lines 0 to 15 drive the trigger signals `exti0` to `exti15` the ADC selects, the input
  of the line before its edge detection: an edge at each change of the selected pin, at the pin
  edge's own time, and at a `SYSCFG_EXTICRn` write that moves the line to a pin of another level,
  whatever `EXTI_IMR`, `RTSR` and `FTSR` hold, inferred.
- STM32L1: line 23 exists on Cat.3 to Cat.6, so `EXTI_IMR` takes 0x00FFFFFF there and 0x007FFFFF on
  Cat.1 and Cat.2, inferred from RM0038 chapter 10.
- STM32L1, inferred rules: an edge or a SWIER write sets PR only while the `EXTI_IMR` bit is set,
  the NVIC line follows PR whatever IMR holds, a SWIER write of 0 clears the SWIER bit, a write
  of 1 to a PR bit clears the SWIER bit of the line whether PR was set or not, a
  `SYSCFG_EXTICRn` write that moves a line to a pin of another level is an edge on the line, and
  a selector code RM0038 8.5.3 to 8.5.6 does not list selects no pin.
- STM32L4: lines 0 to 31 sit in `EXTI_IMR1` to `EXTI_PR1`, lines 32 to 39 in `EXTI_IMR2` to
  `EXTI_PR2`, RM0351 14.5. Both banks follow the STM32L1 rules above, the inferred ones inferred
  on the STM32L4 too.
- STM32L4: the direct lines 17, 23 to 34 and 39, RM0351 Table 62, have an `EXTI_IMRx` and an
  `EXTI_EMRx` bit, storage with the reset value. A direct line sets no PR bit and raises no NVIC
  line, as RM0351 14.3 gives while the system runs. Its source raises its own NVIC line. A deep-sleep
  WFI idles as a plain WFI, so a direct line wakes nothing.
- STM32L4: lines 16 and 35 to 38, the PVD and PVM1 to PVM4, raise `PVD_PVM_IRQn`, lines 18 to
  22 `RTC_Alarm_IRQn`, `TAMP_STAMP_IRQn`, `RTC_WKUP_IRQn` and `COMP_IRQn`. These lines take their
  edges from `EXTI_SWIERx` only.
- STM32L4: `EXTI_EMR1` and `EXTI_EMR2` are storage, and an EXTI event wakes no WFE.

## GPIO

Source: [gpio.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/gpio.cpp)

- STM32L1: `GPIOx_BRR` exists on Cat.4, Cat.5 and Cat.6, RM0038 7.4.11. On Cat.1 to Cat.3 a
  firmware store to offset 0x28 is an unmodelled access and changes no pin. The absence on Cat.3
  is inferred from RM0038 7.4.11 and its revision history.
- STM32L1: `GPIOx_OSPEEDR` is storage, as on the STM32U5 and the STM32H5.
- Every family: the analog level of a pin, which an ADC channel on its pad converts, is the chip
  slot a peripheral output drives, else the board slot, else 0, in every pin mode. A push-pull
  output driving high reads 0 there unless a slot holds VDD. With both slots set the chip slot
  wins and the first overlap on a pin logs one `model_warning` per machine life, also under
  `strict_peripherals`. The chip slot is kept through a machine reset and its driver's reset
  releases it, inferred: the reference manuals describe no pad state of their own.
- STM32L4: `GPIOx_ASCR`, RM0351 8.5.12, stores the switch between each pin and its ADC input,
  which the GPIO model gives its peers. `GPIOx_OSPEEDR` is storage.
- STM32L4: PG2 to PG15 are powered by VDDIO2, RM0351 5.4.2. A CPU or DMA write that changes a
  bonded one of these pins while `PWR_CR2.IOSV` is 0 logs one `model_warning` per machine life,
  and under `strict_peripherals` raises `ModelError` with the register kept. Without strict the
  register takes the write and the pin behaves as a powered one, where RM0351 5.1.2 isolates the
  pins until IOSV is set. What the isolated register reads and drives is inferred.
- STM32L4: port H carries PH0 and PH1, its `GPIOH_MODER` resets to 0x0000000F, RM0351 8.5.1,
  inferred against Table 42.
- STM32L4 errata, no register effect: ES0250 2.2.6, the full JTAG configuration without NJTRST
  pin, ES0250 2.2.16, unbonded WLCSP I/Os stay configurable by software, as the register model
  does, ES0250 2.2.17, an I/O used as output with VDDA or VDDUSB above VDD + 0.6 V, and ES0250
  2.2.20, PH0 and PH1 driven by the GPIOH registers while HSE runs, which can stop the HSE
  oscillator when they leave analog mode.

## FLASH

Source: [flash.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/flash.cpp), [flash_l1.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/flash_l1.cpp)

Not modelled:

- Single ECC errors. ECCC never sets and ECCIE has no effect.

Differs from RM0456:

- OPTSTRT programs the FLASH_WRPxyR registers only and completes at once, with no BSY. The
  protection follows OBL_LAUNCH, a power-on or a brown-out reset. FLASH_OPTR, the boot addresses
  and the SECWM registers keep the machine's option bytes, and a write to one of them logs a
  warning once at the next OPTSTRT.
- A store while BSY is set completes at once and is applied when BSY clears. Silicon stalls the
  bus until then.
- Host and debugger reads, instruction fetches and vector table reads never detect an ECC error.
- An unprivileged fetch of a privileged page is a BusFault. RM0456 Table 73 gives zeros.
- A Secure bank erase over a bank with mixed pages is refused whole, by Table 70, not by 3.5.5.
- Without TrustZone the FLASH_PRIVxBBRy bits are stored and gate no access. RM0456 3.6.3 keeps
  the privileged protection with TZEN = 0.

Not modelled on the STM32H5:

- OBK key storage, the high-cycle data area, OTP and read-only area programming. Their
  registers are storage.
- FLASH_OPSR and FLASH_OTPBLR_CUR are storage. OPSR never reports an interrupted operation and
  OTPBLR_CUR locks no OTP block.
- The effect of FLASH_ACR.WRHIGHFREQ on programming: a program or erase takes its datasheet time
  whatever the delay. A WRHIGHFREQ below RM0481 Table 48 for the current HCLK and voltage range
  warns once per HCLK, range and value at an erase START or a store with PG set, and raises a
  model error under `strict`. LATENCY is checked against the same table.

The HDP area of each bank closes by the SBS HDPL level of RM0481 Table 63, with TrustZone on
and off, RM0481 7.6.2 and Table 72 note 1. A closed sector reads zero to a load, a fetch from it
is a BusFault, and a program or erase of it sets WRPERR, Table 64. FLASH_HDPEXTR extends the area
at HDPL 3. Without TrustZone the rule needs machine option bytes, which set the HDP areas.

Differs from RM0481:

- OPTSTRT programs FLASH_WRP1R_PRG and FLASH_WRP2R_PRG into the _CUR registers at once, with
  no BSY. The other _PRG registers are storage, except FLASH_BOOTR_PRG, FLASH_SECWM1R_PRG and
  FLASH_SECWM2R_PRG, which read 0 and ignore writes while TrustZone is off. A write to a storage
  register logs a warning once at the next OPTSTRT.
- Without machine option bytes the OPTSR, OPTSR2, NSBOOTR, SECWM and HDP _CUR registers read
  the delivery values of RM0481 Table 55, FLASH_SECBOOTR_CUR reads 0 on the bus while TrustZone
  is off, and the WRP _CUR registers read 0xFFFFFFFF. With them every reset loads the encoded
  values. FLASH_NSEPOCHR_CUR, FLASH_SECEPOCHR_CUR and the _PRG registers reset to 0.
- On the STM32H562, H563 and H573 FLASH_OPTSR_CUR reads the delivery value 0x2D30EDF8 read from
  silicon, which differs from RM0481 Table 55 in bit 5. The STM32H523, H533, H543 and H553 read
  the Table 55 value.
- A program over a word that is not erased writes the new data as given.
- Without TrustZone the FLASH_PRIVBBxRy bits are stored and gate no access. RM0481 3.7.2 keeps
  the privileged protection with TZEN = 0xC3.

On the STM32L1, RM0038 section 3:

- A program memory word write, page erase or half page write holds BSY for one tprog of 3.28 ms, the
  typical of the STM32L15x datasheets and inferred for the 2.1 MHz MSI clock, and the memory
  reads its new content from the last store of the operation on. During a word write or a page
  erase, code and data reads of the bank complete and the core keeps running from it. RM0038
  3.4.3 stalls code run from the bank for one tprog.
- From the first store of a half page sequence to the end of its tprog, a CPU or DMA load of the
  bank written, its program memory and its half of the data EEPROM, is a bus error and a fetch from
  it a BusFault, RM0038 3.4.3 note 2 and Table 15 note 3. The bus error of a load from SRAM code
  and of a DMA read, and the start of the lock at the first store, are inferred from those notes.
  A data EEPROM double word erase or write locks its bank the same way, inferred from RM0038
  3.4.3 note 2 and 3.7.3. A parallel page erase or parallel half page locks both banks the same
  way, from the first store, inferred from RM0038 3.4.2 and 3.4.3, which run them from SRAM only.
  Host and debugger reads complete. A vector read from a locked bank is a bus error too, so an
  exception entry with VTOR in that bank escalates to HardFault with HFSR.VECTTBL, inferred from
  RM0038 Table 15 note 3. Firmware runs these operations from SRAM with VTOR in SRAM or in the other
  bank.
- A store while BSY is set is decided against FLASH_PECR at its own time and writes at once. An
  operation it completes adds its tprogs to BSY. EOP sets once at the end of each operation.
  Silicon stalls the store until the operation before it ends. While a half page or a data
  EEPROM double word of one bank is BSY, a program memory or data EEPROM store to the other bank
  sets WRPERR and writes nothing, RM0038 3.4.4 and 3.7.3, inferred. A Cat.5 data EEPROM store to
  bank 2 during a bank 1 half page does the same.
- A program memory word written over a word that is not erased holds the word as given, inferred
  from RM0038 3.4.3, and Table 15 covers the data EEPROM only. A page erase writes 0, and a
  never-programmed word reads 0, the erased value.
- A store with FLASH_PECR.ERASE or FPRG set and PROG clear, or with both set, programs nothing,
  logs one `model_warning` and raises `ModelError` under `strict`.
- A data EEPROM byte, halfword or word write takes one tprog when FTDW is clear and the word reads
  0, else two, RM0038 Table 15. A word store of 0 is a word erase of one tprog. With ERASE or FPRG
  set, DATA set and PROG clear, two word stores from an 8-byte boundary erase or program a double
  word in one tprog. A never written data EEPROM byte reads 0.
- The option words of the information block, offsets 0x00 to 0x1F and 0x80 to 0x9F on Cat.4 to
  Cat.6, take a word store with PELOCK and OPTLOCK clear, timed as the data EEPROM. A power-on,
  brown-out or option load reset loads FLASH_OBR and FLASH_WRPRx from them, FLASH_PECR.OBL_LAUNCH
  requests that reset. A write protected sector, one FLASH_WRPRx bit per 4 KB counted over the
  Flash regions in address order, refuses a program or erase with WRPERR. RDP level 2 refuses
  every option store with WRPERR. At level 1, a store of RDP 0xAA erases the Flash regions, the
  data EEPROM, the option words and the RTC backup registers to 0, then programs the word, in
  three tprog. A part starts with the virgin option words: FLASH_OBR 0x00F800AA, FLASH_WRPRx 0.
- At RDP level 1 every read, program and erase completes, the user flash boot of RM0038 3.7.1.
  BOR_LEV, IWDG_SW, nRST_STOP, nRST_STDBY and nBFB2 read in FLASH_OBR as loaded, and the BOR
  threshold, the watchdog start, Stop, Standby and the boot address follow the machine's own
  configuration.
- PCROP on STM32L151, L152 and L162 Cat.2 and Cat.3 on DIE427, RM0038 3.7.4: SPRMOD loads with RDP
  against nSPRMOD and nRDP. With SPRMOD set a FLASH_WRPRx bit at 0 protects its sector against
  program and erase, WRPERR, and against a CPU, DMA or filtered debugger data read, which reads 0
  and sets FLASH_SR.RDERR. A fetch from it runs. Host and unfiltered debugger reads return the
  content. An option store programs with SPRMOD and every protected WRPx bit kept. The RDP level
  1 to 0 store programs the SPRMOD bit it carries, and its mass erase clears the WRPx words. The
  value a PCROP read returns and the absence of a fault are inferred. So are the option store rule
  that keeps every protected WRPx bit, the words read back before the next reload, and the load of
  SPRMOD 1, OPTVERR and WRPR1 and WRPR2 at 0 after an RDP 1 to 0 store and a power-on, RM0038
  3.7.4. On every other category and
  part FLASH_WRPRx bits at 1 are write protection.
- `Machine(option_bytes=)` selects the boot bank: address 0, the flash alias, or with `swap_bank`
  on Cat.4 to Cat.6 the bank 2 base, whatever the first word of bank 2 holds. RM0038 Table 19
  boots the system bootloader, which jumps to bank 2 when its first word is an SRAM address.
  FLASH_OBR.nBFB2 then loads as the inverse of `swap_bank` at every option load, whatever the
  USER word holds. Any other field away from its default is refused as `option_bytes.<field>`.
- OPTVERR and OPTVERRUSR set again at every reset and FLITFRST pulse after an option load that
  set them, RM0038 3.9.7. An option store programs whatever OPTVERR and OPTVERRUSR hold.
- A program in voltage range 3 completes, logs one `model_warning` and raises `ModelError` under
  `strict`. RM0038 3.6 allows programming in ranges 1 and 2 only. That the program completes
  is inferred.
- FLASH_ACR.RUN_PD powers nothing down: the flash stays readable and fetchable, and
  FLASH_SR.READY reads the inverse of RUN_PD. SLEEP_PD and PRFTEN are storage.
- An RCC FLITFRST pulse resets the FLASH registers only while FLASH_ACR.RUN_PD is set, inferred
  from RM0038 6.3.5, and keeps FLASH_OBR and FLASH_WRPRx at their loaded values. While
  FLITFRST is held, FLASH register writes are ignored whatever RUN_PD holds.
- A supply cut during a program leaves the word as the store wrote it.
- Inferred rules: a wrong key, a third key write after the unlock, or a write to FLASH_PRGKEYR or
  FLASH_OPTKEYR while PELOCK is set is a bus error that sets the lock it targets until the next
  reset, a rejected FLASH_PEKEYR write also sets PRGLOCK and OPTLOCK, a wrong FLASH_PDKEYR key
  restarts the sequence, RUN_PD takes only the first FLASH_ACR write after the PDKEYR pair, a
  FLASH_ACR write that changes ACC64 keeps LATENCY and PRFTEN, so ACC64 clears while LATENCY or
  PRFTEN is 1, a byte or halfword store to program memory sets SIZERR and an unaligned word store
  PGAERR, a store to a page start with ERASE and PROG set erases the page whatever value it writes,
  an erase store away from a page start sets PGAERR, a half page store that skips a word inside the
  half page sets PGAERR, BSY stays 0 while the first 31 half page words load, a parallel operation
  takes a bank 1 page or half page first and a bank 2 one second and sets PGAERR for any other
  order, a FLASH_PECR write that ends a half page sequence early, setting PELOCK included, sets
  PGAERR, a byte or halfword store inside a sequence sets SIZERR and drops it, FLASH_SR.READY reads
  1 after a reset, where RM0038 3.9.7 gives 0, the virgin option words hold 0xFF in the upper byte
  of the RDP and USER words and complement 0xFFFF of WRPx 0, an option word whose complement is
  wrong loads 0, so after an RDP 1 to 0 regression FLASH_OBR reads USER 0 with OPTVERR and
  OPTVERRUSR set, a write of 1 to OPTVERR or OPTVERRUSR clears it until the next reset, an option
  store with OPTVERR or OPTVERRUSR set programs, a data EEPROM store that spans two words counts the
  tprogs of each word, RM0038 Table 20 note 5, a store to the factory information or at RDP level 2
  sets WRPERR, an unaligned or a byte or halfword option store sets PGAERR or SIZERR, a byte or
  halfword store of 0 to the Cat.1 data EEPROM programs nothing and logs one `model_warning`,
  `ModelError` under `strict`.

On the STM32L47x/L48x, RM0351 section 3:

- One register set, FLASH_KEYR, FLASH_CR and FLASH_SR, and no TrustZone. KEY1 then KEY2 clear
  LOCK, any other key write is a bus error and keeps FLASH_CR locked until the next reset.
  FLASH_OPTKEYR clears OPTLOCK with LOCK clear. A wrong option key is a bus error that keeps
  OPTLOCK set until the next reset, inferred from the U5 rule: RM0351 3.7.6 states the lock only.
- A double word programs when its second word is stored, BSY set for
  `flash_options.quad_word_program`, 81.69 us by default, the DS10198 Table 63 tprog. A double
  word that does not read 0xFFFFFFFF sets PROGERR, except when the data is zero: zeros program
  over any double word. A page erase holds BSY for 22.02 ms, a bank or two-bank mass erase for
  22.13 ms, the DS10198 typical values. A PNB at or above the pages of a bank, 64 on a 256-Kbyte
  part, erases nothing and logs one warning, a `model_error` under `strict_peripherals`.
- A read of the bank under program or erase returns its stored data at once, with no bus stall.
  With `FLASH_ACR.DCEN` set such a read returns the stored data too, a popcorn rule for ES0250
  2.2.14.
- `PRFTEN`, `ICEN`, `DCEN`, `ICRST`, `DCRST` and `SLEEP_PD` are storage, the flash reads at once.
  `RUN_PD` takes a write only after PDKEY1 then PDKEY2 in `FLASH_PDKEYR`, RM0351 3.7.1. Inferred:
  the pair unlocks only the next `FLASH_ACR` write, and any other `FLASH_PDKEYR` value restarts
  the sequence. A CPU or DMA write after the pair that sets `RUN_PD` logs one warning per machine
  life and stores it, and under `strict_peripherals` it is a `model_error` with `FLASH_ACR` kept.
  The flash stays readable, so ES0250 2.2.15 does not arise.
- `LATENCY` is checked against RM0351 Table 11 for HCLK in the voltage range PWR applies: a lower
  value, or an HCLK above 80 MHz in range 1 or 26 MHz in range 2, logs one warning per HCLK, range
  and value, a `model_error` under `strict_peripherals`. `LATENCY` 5 to 7 counts as that many wait
  states.
- Fast programming, RM0351 3.3.7: with `FSTPG` set each double word of a row of 32 programs for
  1/32 of the DS10198 Table 63 fast row time, 1.91 ms, so 59.69 us, where RM0351 3.3.7 gives about
  20 us between double words. BSY is set from the first double word to the 32nd and EOP after the
  32nd. `flash_options` sets no fast row time. A double word not complete when the one before it
  ends sets MISERR and FASTERR. PGAERR, SIZERR and WRPERR of a store with `FSTPG` set come with
  FASTERR, inferred for the first store of a row. `FSTPG` set for 7 ms sets FASTERR, counted
  from each `FSTPG` 0 to 1, and a store after it sets PGSERR, inferred.
- A CPU or DMA load of the bank under a fast row ends the row with no flag: the double word in
  flight and the rest of the row stay unprogrammed, inferred, and one warning per machine life
  is logged, a `model_error` under `strict_peripherals`. Code fetched from that bank runs and the
  row goes on, where RM0351 3.3.7 aborts it.
- A fast row on a bank with no `MER1`, `MER2` or two-bank mass erase since the last reset, or
  with HCLK below 8 MHz, logs one warning per machine life for each rule, a `model_error` under
  `strict_peripherals`, and programs. RM0351 3.3.7 sets PGSERR without the mass erase, which the
  model does not set. Counting the mass erase from the last reset is inferred.
- A fast row programs over any content, clearing only the bits it programs, with no PROGERR,
  inferred.
- Option bytes, RM0351 3.4: `FLASH_OPTR`, `FLASH_PCROPxSR`, `FLASH_PCROPxER`, `FLASH_WRPxAR`
  and `FLASH_WRPxBR` read their ST production values, `FLASH_OPTR` 0xFFEFF8AA. Writes are stored
  while `OPTLOCK` is clear and ignored while it is set, inferred, and reserved bits keep their
  production value. `OPTSTRT` programs the nine registers at once, with no BSY, inferred. A
  power-on, brown-out or option byte loader reset loads them, `OBL_LAUNCH` requests the last, and
  every other reset puts the loaded words back. The construction and every `OPTSTRT` write the
  programmed words into the option byte area of RM0351 3.4.1 Table 13, bank 1 from 0x1FFF7800,
  bank 2 from
  0x1FFFF808, each double word the word in its low half and its complement in its high half. The
  Unused fields hold the register's reserved bits and their complement, and the double word at
  0x1FFFF800 stays erased, inferred. The loaded `WRPxy_STRT` to `WRPxy_END` pages of bank x refuse a store,
  a page erase and a bank erase with WRPERR. `RDP`, the PCROP words and `DUALBANK` act as below. The other option
  fields take no effect. `OPTVERR` stays 0.
- Read protection, RM0351 3.5.1: the loaded `RDP` gives level 0 for 0xAA, level 2 for 0xCC and
  level 1 for any other value, from `FLASH_OPTR` alone, whatever the complement in the option
  byte area holds. A debug port access is an access through `m.mem.secure` or
  `m.mem.nonsecure`. At level 1 such an access to the flash main memory, the OTP area or SRAM2
  is a bus error, a `BusAccessError` in Python, inferred for the OTP area and SRAM2 from RM0351
  Table 15. The option bytes, the system memory, the other memory and the peripherals stay
  open. At level 2 every such access is a bus error. `m.mem` and the CPU keep every access at every
  level: the model runs the boot from flash user mode. The RTC backup registers stay open to the
  debug port at level 1.
- At level 2 `OPTSTRT` sets `WRPERR`, programs nothing and puts the programmed words back in the
  option registers, inferred.
- `OPTSTRT` with the loaded level 1 and `RDP` 0xAA in `FLASH_OPTR` erases the flash at once, with
  no BSY time, inferred. With `PCROP_RDP` set both banks are erased and the PCROP words program
  their production values. With `PCROP_RDP` clear, a bank with a PCROP area, `PCROPx_END` not
  below `PCROPx_STRT` (inferred), keeps the pages that hold part of the area and the PCROP words
  keep their programmed values. The other bank is erased, inferred. The OTP area and the
  system memory are kept. The other option words program as written. The SYSCFG model writes 0
  to the whole SRAM2 at once, inferred, with no SRAM2BSY. The RTC model writes 0 to every RTC
  backup register, RM0351 3.5.1. The new level takes effect at the next option byte load.
- PCROP, RM0351 3.5.2: the loaded `PCROPx_STRT` and `PCROPx_END` of each bank give an area
  from STRT x 8 to (END + 1) x 8 in the bank. A CPU, DMA or debug port load from it reads 0 and
  sets `RDERR`, 0 is inferred, and `RDERRIE` raises `FLASH_IRQn`, 3.6 Table 16. A debug port
  store to it is dropped. A fetch from it runs. `m.mem` reads its content. A program of a
  double word in it, an erase of a page holding part of it and the erase of its bank or of
  both banks set `WRPERR`, the bank erase rule inferred. An `OPTSTRT` that clears or shrinks a
  bank's area keeps that bank's two PCROP words and programs the rest, an `OPTSTRT` that grows
  it programs it.
- OTP, RM0351 3.3.1: a double word of 0x1FFF7000 to 0x1FFF73FF programs once with `PG`. A program
  of a double word with any bit at 0 sets `PROGERR`, also with zeros, inferred flag. A store to
  the rest of the information block, the option bytes and the signature, and a `FSTPG` store to
  the OTP area, set `WRPERR`, inferred.
- `DUALBANK`, RM0351 3.7.8: on a 256 KB or 512 KB part a loaded `DUALBANK` 0 makes the flash one
  bank, pages 0 up to the flash size, `MER1` erases all of it. `BKER` 1 and `MER2` alone erase
  nothing, log one warning per machine life and raise `model_error` under
  `strict_peripherals`, inferred. The bank 2 WRP and PCROP words protect nothing then. On a 1 MB
  part the flash is always two banks.
- `Machine(option_bytes=)` programs and loads `FLASH_OPTR` at construction, the option byte area
  with it: `swap_bank` sets `BFB2`, `dualbank=False` clears `DUALBANK` on a 256 KB or 512 KB part. A
  BFB2 boot runs bank 2 code at its own address: the core boots from the bank 2 base, 0x08020000 on
  a 256 KB part, and the banks keep their addresses. Without `swap_bank` the core boots from address
  0, the flash alias. An image linked at 0x08000000 for a bank-swap boot runs only from bank 1. The
  core boots the bank 2 base whatever bank 2 holds. On silicon, AN2606 Rev 71 79.1.2 Figure 102 and
  79.2.2 Figure 104, the system bootloader boots bank 2 only when its first word is an internal SRAM
  address, else bank 1 when bank 1's first word is one, else bank 1 at RDP level 2, else it runs its
  own command loop. `RDP` and the other user option bits keep their production values, firmware
  programs them with `OPTSTRT`. A `BFB2` that firmware programs is loaded only. Any other field away
  from its default, `dualbank=False` on a 1 MB part and `swap_bank` with `dualbank=False` are
  refused as `option_bytes.<field>`.
- ES0250 2.2.2: a PCROP area within one page is kept with its page at the regression, as
  RM0351 3.5.1 states, where the erratum leaves the bank unerased and the area unprotected.
- ES0250 2.2.23: `OBL_LAUNCH` with MSI at 8 MHz or more logs one warning per machine life, a
  `model_error` under `strict_peripherals` with `FLASH_CR` kept, and loads the option bytes.
- ECC, RM0351 3.3.2: a power cut tears the double word in flight, its first 4 data bytes ANDed
  into the old ones and its last 4 kept. A CPU or DMA load of it sets `ECCD`, `BK_ECC` and
  `ADDR_ECC`, the offset in its bank, and takes the NMI. Clearing `ECCD` keeps `ADDR_ECC` and
  `BK_ECC`. `ECCC` and `SYSF_ECC` stay 0, and `ECCIE` is storage. A reset clears `FLASH_ECCR`.
- ES0250 2.2.9: on silicon a read of a location a reset or power-down hit during its program or
  erase may corrupt `FLASH_ECCR`. The model records that read's `ECCD`, `BK_ECC` and `ADDR_ECC`
  exactly, and only a power cut tears a location: a reset of another kind drops the operation.

## DMA

Source: [dma_classic.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/dma_classic.cpp)

- STM32L1 DMA1 and DMA2, RM0038 section 11. One transfer runs at a time per controller, 4 HCLK
  cycles after the previous one, inferred. RM0038 gives no transfer time.
- `GIFx` reads as the OR of `TEIFx`, `HTIFx` and `TCIFx`, inferred.
- `HTIFx` sets once per pass, when the items left are at most half the count latched at the
  enable. For an odd count the rounding is inferred.
- `CPARx` and `CMARx` take writes while the channel runs, and the running transfer keeps the
  addresses it latched at the enable until the next circular reload reads them again. `CCRx`
  fields take writes while the channel runs and apply from the next item, inferred. `CNDTRx`
  ignores writes while `EN` is set, RM0038 11.4.4.
- A transfer error leaves the count and the current addresses at the failed item, inferred. A
  circular channel disabled mid-copy and enabled again with no `CNDTRx` write restarts from the
  count left at the disable, inferred.
- A reserved `PSIZE` or `MSIZE` of 3 transfers words, inferred from RM0038 11.4.3, and `CIRC`
  with `MEM2MEM` runs the copy once, inferred from RM0038 11.3.3. Each logs one `model_warning`.
- A byte or halfword DMA write to an APB1 or APB2 address is a word write at the address
  aligned down to 4, with the item repeated on every lane, RM0038 11.3.4. A CPU byte or
  halfword store to the same address writes its own lanes only, inferred.
- The requests of a channel are ORed, RM0038 11.3.7. Each item acknowledges every request of
  the channel that is high, and the end of a non-circular transfer tells every request of the
  channel, inferred.
- A peripheral-requested transfer that leaves one item tells every request of the channel, once
  per pass of a circular channel. This is the EOT_1 signal the I2C reads with CR2.LAST, RM0038
  26.3.7. A memory-to-memory copy tells none.
- The channels run whatever `RCC_AHBENR.DMAxEN` holds, which gates only their register access,
  see RCC.
- STM32L4 DMA1 and DMA2, RM0351 section 11, follow the STM32L1 rules above, the inferred ones
  inferred on the STM32L4 too, with the APB windows of RM0351 Table 1 and `RCC_AHB1ENR`.
- STM32L4: `DMA_CSELR` routes each request of RM0351 Tables 46 and 47 to its channel while the
  channel's `CxS` holds the request's code. A request with another code reaches nothing. A
  signal the tables list on two channels, such as `SAI1_A` on DMA2 channels 1 and 6, reaches
  both. The I2C4, DCMI and HASH requests of the STM32L49x/L4Ax have no row.
- STM32L4: a firmware byte or halfword write to a DMA register logs one `model_warning` per
  controller and, under `strict`, raises `ModelError` before the store, after RM0351 11.6 "The DMA
  registers have to be accessed by words (32-bit)". Without `strict` it lands. ES0250 2.4.2
  states the same as a description correction.
- STM32L4: ES0250 2.4.1. A transfer error sets `TEIFx` and clears `EN` whatever `CGIFx` the
  firmware writes in the same cycle.
- STM32L4: ES0250 2.8.14, 2.8.16 and 2.8.17. The ADC2 and ADC3 requests on DMA2 channels 4 and 5
  need no request 0 on channels 2 and 3, and DMA2 channels 4 and 5 hand their acknowledge and end
  of transfer to their own requests only, while DMA1 serves ADC2 or ADC3.

## GPDMA and LPDMA

Source: [gpdma.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/gpdma.cpp)

- The link step mode is not modelled. A channel that uses it runs to completion.
- Triggers are not modelled.
- `CxTR1` takes writes while its channel is enabled. RM0456 17.8.10 makes it read-only then.

## GTZC

Source: [gtzc.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/gtzc.cpp)

- The GTZC models serve only with TrustZone enabled, `OptionBytes(tzen=True)`.
- No debugger access raises a TZIC event, so a host view leaves the TZIC flags and GTZC_IRQn as
  they are.
- STM32H573: `HDMICECSEC` and `HDMICECIE` name no peripheral, the SVD has no HDMI-CEC.
- A delay block takes the SEC bit of the peripheral it serves: DLYBSD1 and DLYBSD2 those of SDMMC1
  and SDMMC2, DLYBOS1 that of OCTOSPI1 and DLYBOS2 that of OCTOSPI2 on the STM32U5, DLYBOS1 that
  of the OCTOSPI on the STM32H573. RM0456 Table 5 and RM0481 Table 3 list no delay block, and
  RM0456 Table 297 and RM0481 Table 275 give the same pairs.

## HASH

Source: [hash.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/hash.cpp)

- An HMAC key longer than the block with LKEY 0 is hashed as with LKEY 1, and logged. The block is
  64 bytes for SHA-1, SHA2-224 and SHA2-256, 128 bytes for the SHA-512 family and the sponge rate
  for SHA-3.
- The H5 HASH keeps SHA-3 and SHAKE on the STM32H543, H553, H5E and H5F only, RM0481 Table 397
  and RM0517 46.2. On the STM32H52x, H53x, H56x and H57x, ALGO keeps 0000, 0010, 0011 and 1100
  to 1111. A CR write of another code, with or without INIT, stores ALGO 0000 and the hash runs
  SHA-1, as read back on an STM32H573 for 0001 and 0100 to 1011. The H543, H553, H5E and H5F
  store 0000 for 0001, 1010 and 1011 by the same rule, which is the H573 reading applied there,
  inferred. The write logs once and raises a model_error under strict_peripherals before CR
  changes.
- The SHA-3 and SHAKE rules follow RM0481 and RM0517 and are served on the STM32H543, H553, H5E
  and H5F rows. Building a description for the H543 and H553 needs ST's DIE47C rzone file, and
  for the H5E and H5F the DIE47A file. The H5E and H5F parts need a CMSIS SVD, which ST's pack
  does not hold. The SHA-3 variant refuses a description whose SR.NBWE is narrower than 6 bits.
- The STM32H503 HASH is the U5 model without MD5: ALGO[1:0] and NBWE[4:0] as RM0492 25.6 give
  them, with SHA-1, SHA-224 and SHA-256. ALGO 01 is reserved there. A CR write with it stores
  ALGO 00 and the hash runs SHA-1, logged once and raised under strict_peripherals, inferred
  from the H573 reading. A board check of ALGO 01 on an H503 would settle it.
- The H5 BUSY time of one block, from its launching DIN write, is 67 HCLK cycles under SHA-1 and
  SHA2-512 and 51 under SHA2-256, as measured on an STM32H573 at 32 MHz. RM0481 Table 401 gives
  82, 98 and 66. NBWE and NBWP take their new values 1 cycle after BUSY clears and DINIS 1 cycle
  later, as measured. While BUSY, NBWP counts the launched block's words with the pending ones.
  SHA2-384, SHA2-512/224 and SHA2-512/256 take the SHA2-512 times and SHA2-224 the SHA2-256
  times, inferred from their families. The U5 keeps RM0456 Table 486: 82 under SHA-1 and 66 under
  MD5, SHA2-224 and SHA2-256.
- An H5 final digest keeps BUSY its compressions times the block time plus 17 HCLK cycles under
  SHA-1 and SHA2-256 and 33 under SHA2-512. DCIS, NBWE and the digest registers follow 1 cycle
  after BUSY clears and DINIS 1 cycle later. An STM32H573 measured from the DCAL store BUSY 0 at
  84, 68 and 100 cycles for one compression and 150, 118 and 166 for two, 152, 120 and 168 when
  the padding spills into a second block, under SHA-1, SHA2-256 and SHA2-512. The rule gives each
  within 1 cycle. The digest registers read 0 in the one cycle between BUSY 0 and DCIS, and after
  INIT. A DIN write in between completes the digest first. SHA-3 and SHAKE digests take their
  block times, DCIS with BUSY 0. The U5 sets DCIS with BUSY 0 after one block time per padded
  block.
- The H5 HASH reads SHA-3 and SHAKE DIN words as the SHA-2 core does: the first message byte of a
  word, after the DATATYPE swap, is its most significant byte, and the digest bytes fill HR0 and
  up from bit 31. RM0481 40.4.4 does not spell this out for SHA-3. DATATYPE 00, 01 and 11 follow
  the same mapping.
- The H5 BUSY time of SHAKE128 and SHAKE256, 66 and 58 cycles, follows the SHA-3 rule of
  RM0481 Table 401, which has no SHAKE row. A DCAL that squeezes the next rate takes the same time.
  A DCAL written while BUSY of a SHAKE digest squeezes the next rate at once, logs once, and the
  rate of the first DCAL is never shown. NBWP and CR.NBW read 0 after a SHAKE digest, as after a
  SHA-3 digest.
- A DIN word after a SHAKE digest with no INIT starts a new message and logs once. MODE 1 with
  SHAKE logs once at INIT and MODE 1 is ignored.
- The H5 CSR registers use popcorn's layout too: CSR4 up hold the Keccak lanes or the SHA-512
  chaining value and pending block, the inner HMAC digest sits at CSR55, CSR72 or CSR52 by
  algorithm. A save of CSR0 to CSR71 covers every SHA-3 context but the SHA3-384 and SHA3-512
  HMAC, as RM0481 40.4.8 says.
- DMA input works through GPDMA request 89. A machine with no DMA controller that serves
  `hash_in_dma` logs DMAE 1 once and requests nothing.
- The CSR registers hold popcorn's own serialisation of the state, not the silicon layout. CSR0
  reads 0x48530001, out of reset too, where RM0456 gives 0x00000002.
- A CSR0 write other than popcorn's tag, or a CSR1 write that does not match the state latched at
  INIT, is refused and logged.
- A DIN write while BUSY does not stall the bus. The CPU runs on.

## I2C

Source: [i2c.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/i2c.cpp), [i2c_v1.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/i2c_v1.cpp)

- The I2C runs as a 7-bit controller and as a 7-bit target on OAR1, OAR2 with its mask and the
  general call, with clock stretching, NOSTRETCH, SBC and the SMBus timeouts of TIMEOUTR. 10-bit
  addressing, the SMBus host and device addresses, ALERT, PEC, the autonomous trigger, SBC or
  NOSTRETCH in controller mode and SBC with NOSTRETCH in target mode warn once as unmodelled,
  `model_error` under strict peripherals at the next controller START or target address phase.
  A write of I2C_AUTOCR only warns, since a modelled target setup with SBC or NOSTRETCH writes
  it too.
- As a target, the SCL low timeout counts from the end of each address or data phase of a
  transfer to the start of the next phase or STOP the controller queued, the I2C's own stretch
  included. Across a System link that gap holds the remote controller's wait of 2L + p + s per
  phase and its own waits. A target counts it only while the last address phase of the transfer
  addressed it, for a board controller and a remote one alike. A target the controller does not
  address counts no SCL low during another target's transfer, a controller stall included, where
  silicon measures SCL on the line whatever the address, RM0456 65.4.12. An RCC reset or a PE
  clear of the remote controller's I2C releases the bus at its time plus L and ends the count,
  with no STOP, and an RCC reset of the target keeps a release it holds. The bus idle timeout
  counts while PE is set and BUSY is 0, from the STOP or the PE set. Timeout lengths follow the
  kernel clock rate the model reads at its last event or register access.
- Inferred, unmeasured on a board: TEXTEN sums the I2C's own stretches from START to STOP, a
  controller's waits included, each from the end of the target's stretch before it, and the sum
  restarts after a timeout and at each TEXTEN set. A target timeout releases
  its stretch as PE = 0 does and leaves the transfer until the next address match. A controller
  timeout during a target's stretch sends its STOP once the target releases SCL.
- A target stretches before a written byte's acknowledge after the byte instead: the acknowledge
  and the release come at the end of the byte or at the RXDR read, up to one SCL period and the
  acknowledge bit later than on silicon.
- A STOP the controller queues during a target's stretch runs after the stretched byte's answer.
  A STOP due inside a running byte drops that byte.
- A target transmitter's TCR or empty-TXDR stretch holds only the next read byte. A STOP the
  controller queues runs past it, and TCR stays set until a non-zero NBYTES write.
- Inferred, unmeasured on a board: with NOSTRETCH and TXE 1 at the address acknowledge, TXIS sets
  only when the firmware writes it. TXIS set before a NACK or a STOP stays until a TXDR write.
  With SBC and RELOAD clear, bytes past NBYTES are acknowledged as CR2.NACK says, with no TCR. An
  SBC transmitter with RELOAD sets TCR after the acknowledge of the NBYTES-th byte. A NOSTRETCH
  receive overrun leaves CR2.NACK as it is.
- PE set during a bus hold reads BUSY 1 at once. Silicon waits for the next START.
- A transfer that meets a hold waits. On silicon it would lose arbitration. A target's ARLO reply
  models that case.
- An address matched in target mode clears a pending CR2.START on the STM32U5 and STM32H5,
  RM0456 65.9.2 and RM0481 53.9.2. On the STM32L4 the match leaves START set and a write of
  ICR.ADDRCF clears it, RM0351 39.9.2 and 39.9.8 as corrected by ES0250 2.20.7.
- STM32L4 I2C1 to I2C3, RM0351 section 39, run the I2C model without I2C_AUTOCR and without
  CR1.ADDRACLR and STOPFACLR: ADDR and STOPF set at every match and STOP. The kernel clock is the
  instance's `RCC_CCIPR` I2CxSEL selection. The Fast-mode Plus drive bits I2Cx_FMP of SYSCFG_CFGR1
  are stored, the bus timing follows TIMINGR alone.
- STM32L4 ES0250 2.20.2, the Stop-mode behaviour with WUPEN 0, corrects RM0351 for Stop mode, which
  the model leaves out. ES0250 2.20.1 (10-bit controller restart), 2.20.3 (SDA setup shorter than a
  kernel period), 2.20.4 (spurious BERR), 2.20.5 and 2.20.13 (last byte lost in reload mode), 2.20.6
  (spurious controller transfer after a match with START pending), 2.20.8 (OVR not set on underrun
  with NOSTRETCH), 2.20.9 (stall after the first byte), 2.20.10 (SDA held low at an SMBus timeout),
  2.20.11 (Fast-mode Plus not on every pin) and 2.20.12 (I2C3 analog filter needs PC0 and PC1) are
  silicon effects the model leaves out.
- STM32L1 I2C1 and I2C2, RM0038 section 26, run as a 7-bit controller transmitter and receiver
  on board targets. The SCL period is Thigh + Tlow from CCR and PCLK1, RM0038 26.6.8, without the
  rise time and the noise filter delay: a START takes 1 period, a byte 9, a STOP 1. FREQ, CCR,
  TRISE and the PCLK1 rate are checked at the first START after PE is set with PCLK1 running, and
  each rule a configuration breaks logs one `model_warning`. TRISE passes at or above the rise
  time.
- SMBus, PEC, ALERT and ARP log one `model_warning`, raise `model_error` under
  `strict_peripherals` at the START that uses them, and the bus stays idle for them.
- DMA, RM0038 26.3.7: with CR2.DMAEN 1 the TX request follows TxE and the RX request follows
  RxNE, in controller and target mode and whatever ITBUFEN holds. A DMA access to DR acts as a CPU
  access. With CR2.LAST 1 at the EOT_1 of the channel serving the RX request, the controller
  receiver NACKs the next byte. Inferred: LAST counts as it is at EOT_1, the NACKed byte is the
  first whose acknowledge the I2C decides after EOT_1, the end of a START drops a pending EOT_1,
  an EOT_1 with DMAEN 0 changes nothing, and LAST has no effect in target mode. The end of the DMA transfer sends no STOP: the firmware
  sets it.
- A `board.i2c_controllers` controller, alone on the instance, queues phases through
  `i2c_controller_port` with the placement, unit order, STOP, drop, reset and snapshot rules of
  the STM32U5 I2C with a board controller.
- Target receiver and transmitter, RM0038 26.3.2: a 7-bit match of OAR1, of OAR2 with ENDUAL 1, or
  the general call with ENGC 1, with CR1.ACK 1, sets ADDR with TRA as the R/W bit, DUALF for OAR2
  and GENCALL for the general call, and holds SCL until the SR1 then SR2 read. OAR2 matches with
  ADDMODE 0 only, the general call in either ADDMODE. Address 0 never matches OAR1 nor OAR2. A STOP,
  a repeated START and the disable clear DUALF and GENCALL. Written bytes land in DR with RxNE,
  CR1.ACK read at the end of the byte's phase, the acknowledge period included. A byte ending with
  RxNE 1 sets BTF and holds SCL until the DR read. A read byte sends the shift register, which the
  first DR write after the ADDR clear loads with TxE kept 1, and waits for that write. The
  acknowledge of a read byte moves a full DR on and sets TxE, and with DR empty BTF sets at the
  byte's end, RM0038 26.3.2. The controller's NACK sets AF, and a DR write after it clears TxE and
  sends nothing. BUSY follows the port's START and STOP with PE 0 too. A STOP sets STOPF after an
  acknowledged address or byte, none after a NACKed byte. Which acknowledge counts for that rule,
  the last one sent or CR1.ACK at the STOP, is inferred as the last one sent. PE 0 written
  between a match of an address of the I2C and the port's STOP disables it at that STOP, RM0038
  26.6.1: until then it answers,
  stretches and takes DR accesses as with PE 1. Inferred: OAR1 matches before OAR2 for one address,
  PE 0 with no address matched since the last STOP disables the I2C at once, a match with CR1.ACK 0
  answers NACK at the address end with no ADDR, address 0 with R/W 1 matches nothing, an SR1 read
  before STOPF sets arms its clear by a CR1 write, the first read byte after the ADDR clear sets no
  BTF, the communication PE 0 waits for runs from the match to the port's STOP, repeated STARTs and
  their address phases included, a repeated START inside it matches the I2C's addresses, the STOP
  that ends it sets no STOPF, and a PE 1 write before that STOP keeps the I2C enabled. A firmware START
  waits for the port's STOP. A 10-bit header with ADDMODE 1 logs one `model_warning`, raises
  `model_error` under `strict_peripherals` at the address phase, and answers NACK.
- Target with NOSTRETCH 1, RM0038 26.3.4 and 26.3.5: an address match answers at its end with ADDR
  set and no SCL hold. A written byte that ends with RxNE 1 is lost and sets OVR, and a read byte
  that starts with the shift register empty sends the DR byte again and sets OVR. BTF never sets.
  A read byte NACKed before the ADDR clear ends the transmission, RM0038 26.3.4 AF. Inferred: the
  lost byte is acknowledged as CR1.ACK says, a read byte before the ADDR clear sends DR as it is,
  TxE sets only with ADDR clear, and the ADDR clear after that NACK sets no TxE.
- Errors, RM0038 26.3.4: a target's `bus_error` sets BERR and its `overrun` sets OVR at the end of
  the byte, which completes as its acknowledge says. On silicon OVR sets in slave mode with
  NOSTRETCH only, the injection exercises firmware handlers. With several targets the byte takes
  the error of the first in attachment order. An `arbitration_lost` sets ARLO, clears MSL and
  TRA, RM0038 26.6.7, ignores the reply's acknowledge and stretch, drops a read byte and sends no
  DR byte the firmware wrote, RM0038 26.3.4 ARLO. Inferred: TxE clears and BTF follows a byte
  still held, as at a START end. Inferred: BUSY stays 1 for one SCL period, then the winner's STOP
  reaches the targets, clears BUSY and CR1.STOP and sends a START CR1.START holds. PE 0 in that
  window leaves BUSY 1 until that STOP. In target mode a port STOP inside a running data byte of
  the I2C's acknowledged address, or inside a running address phase while the I2C is enabled,
  sets BERR and drops the byte, RM0038 26.3.4 BERR. Inferred: a STOP inside another address's
  byte or before a queued phase starts sets no BERR.
- The controller receiver decides a byte's acknowledge at the end of its eighth period, before
  the acknowledge pulse, from CR1.ACK, an ACK write inside the eighth period counting, as RM0038
  Figure 213 note 4 states. An ACK write during the acknowledge pulse counts for the next byte,
  inferred. With POS 1 it takes ACK as it was at the previous byte's decision or at the end of the
  address, inferred for an ACK write during a byte. A byte clocked after a NACKed byte reads 0xFF and
  reaches no target.
- A DR write in reception clears RxNE, and with BTF set the held byte replaces the written value
  in DR, inferred. A DR write with TRA 1 clears RxNE, BTF and a byte held from a reception, and
  the transmitter sends it, inferred.
- PE 0 during a controller transfer, which RM0038 26.6.1 forbids, ends it at once and logs one
  `model_warning`. The SB clear by an SR1 read then a DR write follows RM0038 26.6.6 SB and Figure
  212 EV5. Inferred: a DR write with SB set and no SR1 read since the last SB or ADDR clear sends
  nothing, and the DR write that clears SB takes the SR1 read, so the ADDR clear needs an SR1 read
  after it. A DR write while ADDR is set, during the address phase or after a data NACK sends
  nothing, inferred. An SR2 read between the SR1 read and the DR write that clears SB leaves
  that clear armed, inferred: RM0038 26.6.7 names the ADDR case only. A CR1.STOP set while ADDR
  is set sends its STOP after the ADDR clear, inferred where RM0038 26.6.1, Figure 212 and 26.3.3
  conflict. A byte written with TxE 1 and NACKed leaves TxE 1, inferred from RM0038 26.6.6 TxE.
- An STM32L1 I2C target answers at the phase's end. A target that leaves its answer pending is
  taken as NACK or as a byte of 0xFF and fails the run with `config_error`, and
  `i2c_target_port::complete()` and `complete_read()` throw `config_error`. The STM32L1 I2C is a
  link end of no linked system and takes no host bus hold.

## IWDG

Source: [iwdg.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/iwdg.cpp)

- A new prescaler restarts the division sequence when PVU falls, an error below one period of
  the old prescaled clock.
- A refresh reloads at the key write, not one period later.
- EWIC acknowledges without the 0x5555 key, as `LL_IWDG_ClearFlag_EWIF` expects.
- The IWDG starts at the first `KR` 0xCCCC whatever the hardware watchdog option holds:
  FLASH_OPTR.IWDG_SW on the STM32U5, FLASH_OPTSR_CUR.IWDG_SW on the STM32H5, FLASH_OBR.IWDG_SW on
  the STM32L1. RM0456 61.4.3 and RM0038 21.3.1 start it at power-on with IWDG_SW 0.
- The STM32L1 IWDG, RM0038 21, has KR, PR, RLR and SR with PVU and RVU. A refresh reloads at any
  counter value. PR 110 and 111 both divide by 256. PVU and RVU fall 5 LSI cycles after the
  write, the "up to 5" of RM0038 21.4.4 taken as 5, inferred, 6 on the STM32U5 and STM32H5.
- An update flag falls only while the LSI runs: a PR, RLR, WINR or EWCR write before the LSI
  starts keeps its flag set until the LSI is ready, inferred.
- The STM32L4 IWDG, RM0351 36, has KR, PR, RLR, SR with PVU, RVU and WVU, and WINR. PR 110 and 111
  both divide by 256. RM0351 36.4 says an update "takes up to five LSI/prescaled clock cycles". The
  model clears PVU, RVU and WVU 5 LSI cycles after the write, whatever the prescaler, inferred. With
  FLASH_OPTR.IWDG_SW 0 the IWDG starts at every reset without a key, RM0351 36.3.3. RM0351 names
  power-on only, the start after the other resets is inferred. FLASH_OPTR.IWDG_STOP and IWDG_STDBY
  are stored: the model runs in Run mode only.

## LPTIM

Source: [lptim.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/lptim.cpp)

STM32L4 LPTIM1 and LPTIM2, RM0351 section 34, count the kernel clock of RCC_CCIPR LPTIM1SEL or
LPTIM2SEL through the prescaler, started by CNTSTRT or SNGSTRT.

- The counter starts at the later of the CNTSTRT or SNGSTRT write plus 3 kernel clocks, RM0351
  34.4.6, and the ENABLE write plus two prescaled clocks, inferred from RM0351 34.4.12. A start
  write made with ENABLE 0 is discarded, also when the same write sets ENABLE, inferred.
  CNTSTRT and SNGSTRT read 0 at once, inferred.
- CMPM and ARRM set one kernel clock after the counter reaches CMP or ARR, ES0250 2.18.3, also
  with PRESC above 0, inferred. CMP above ARR never matches, and a counter above a lowered ARR
  counts to 0xFFFF before it wraps, inferred.
- CMPOK and ARROK set 3 kernel clocks after the CMP or ARR write, and the value applies then,
  inferred: RM0351 34.4.10 states a latency with no number. Counter ticks up to the OK compare
  with the old values, and a match of such a tick still sets its flag at its due after the OK,
  inferred. An OK at the one-shot wrap keeps the stop at the wrap, inferred. Terms due at the
  same time apply in the order CMPOK, ARROK, CMPM, ARRM, wrap, mode switch, inferred.
- With PRELOAD 1 and the counter started, a CMP or ARR value takes effect at the first wrap at or
  after its OK flag, RM0351 34.4.10, the wrap being the tick after ARR, inferred. The counter
  counts as started from the CNTSTRT or SNGSTRT write, its start latency included, inferred. A
  later write waits for its own OK and wrap. ENABLE written to 0 and the one-shot stop apply such
  a value at once, inferred.
- SNGSTRT written while the counter runs in continuous mode makes it stop at a later wrap, and
  CNTSTRT written in one-shot mode makes it go on counting, RM0351 34.4.7. The new mode reaches
  the counter 3 kernel clocks after the write, inferred, so a wrap before then keeps the old mode,
  and a CNTSTRT reaching a counter that the one-shot wrap stopped starts it. The same mode written
  again while the counter runs is discarded, inferred. A second CNTSTRT or SNGSTRT before the
  first reaches the counter replaces it, logs one `model_warning` and raises `ModelError` under
  `strict`, inferred.
- ENABLE written to 0 stops the counter at 0 and keeps the flags and a pending CMP or ARR write,
  inferred.
- An IER, CFGR or OR write with ENABLE 1, a CMP or ARR write with ENABLE 0, and a second CMP or
  ARR write before its OK flag log one `model_warning` each and raise `ModelError` under
  `strict`. The register keeps its value, except the second write, whose value replaces the
  pending one, inferred.
- A start with OR_0 or OR_1 set, TRIGSEL not 0 with TRIGEN not 00, PRESC above 0 with
  COUNTMODE 1 or ENC 1, which RM0351 34.4.11 and 34.4.13 forbid, ENC 1 with CKSEL 1, or with
  CKSEL 1, COUNTMODE 1 or ENC 1 CKPOL 11, CKPOL 10 with CKSEL 1 or a CKFLT above 0, logs one
  `model_warning` each and raises `ModelError` under `strict`. The counter then counts the
  kernel clock, or input 1 edges as configured, and with TRIGSEL not 0 no trigger starts it.
- With CKSEL 1 or COUNTMODE 1 the counter counts the active edges CKPOL selects on the
  `lptim1_in1` and `lptim2_in1` pin signals, PB5 or PC0 AF1 and PB1 or PC0 AF14 on the L476,
  RM0351 34.4.11 and 34.7.4. With CKSEL 1 the input 1 edges clock the kernel logic, RM0351
  34.4.3 and 34.4.11, so CMPOK, ARROK, the start, a mode switch and a trigger count edges and no
  internal clock: the ENABLE latency 2 edges, the OK and start latencies 3 edges and a trigger
  start 2 counter clocks, inferred counts that give the five lost edges of RM0351 34.4.11 after
  an ENABLE and a CNTSTRT for every PRESC. PRESC divides the edges before the counter, RM0351 34.4.5, and
  CMPM and ARRM set at the edge, inferred. With COUNTMODE 1 every active edge counts and the flags
  set one kernel clock after it, the kernel clock sampling taken as the edge itself, inferred, and
  an edge with the kernel clock stopped is dropped, inferred. An edge during a debug hold is
  dropped, and `lptim1_out` follows CNT at each edge, inferred.
- With ENC 1, LPTIM1 only, RM0351 34.3 Table 216, the counter counts the `lptim1_in1` and
  `lptim1_in2` edges CKPOL selects, PB5 or PC0 and PB7 or PC2 AF1 on the L476, up or down by the
  level of the other input, RM0351 34.4.13 Table 220, between 0 and ARR: counting down from 0
  gives ARR. A change of direction sets DOWN or UP, RM0351 34.7.1, at the edge, and the count
  starts upwards, inferred. CMPM and ARRM set one kernel clock after the edge that reaches CMP or
  ARR in either direction, inferred. An edge during a debug hold or with the kernel clock
  stopped is dropped, inferred. LPTIMxRST takes the input pins' levels. With PRELOAD 1 a pending
  CMP or ARR applies at a down wrap from 0 as at an up wrap, and that wrap loads the new ARR,
  inferred: RM0351 34.4.10 names only the end of the period. SNGSTRT with ENC 1 logs one
  `model_warning` and raises `ModelError` under `strict`, RM0351 34.4.13 asking for continuous
  mode, and the one-shot stop then applies at an up wrap.
- With TRIGEN not 00 and TRIGSEL 0, the `lptim1_etr` and `lptim2_etr` pin signals, PB6 or PC3 AF1
  and PA5 or PC3 AF14 on the L476, start the counter on the edge TRIGEN selects, RM0351 34.4.6 and
  34.7.4. CNTSTRT or SNGSTRT arm the start and choose the mode. EXTTRIG sets at the edge, and the
  counter starts two prescaled clocks after it, RM0351 34.4.6. With TRGFLT 01, 10 or 11 a level
  change counts once the pin has held it for 2, 4 or 8 kernel clocks, and a shorter pulse is
  rejected, RM0351 34.4.4 and 34.7.4: EXTTRIG and the start move by that many clocks, the
  sampling phase taken as the edge itself, inferred. An edge during a debug hold or with the
  kernel clock stopped starts nothing, and the level it leaves becomes the filtered level, since
  the filter samples on the kernel clock, inferred. LPTIMxRST takes the pin's level as the
  filtered level, inferred. A trigger while the
  counter runs is ignored and sets no EXTTRIG, RM0351 34.4.6 and 34.7.1, unless TIMOUT is 1: it
  then sets EXTTRIG and restarts the count from 0 two prescaled clocks later, RM0351 34.4.8, the
  latency inferred. In one-shot mode each trigger after the stop starts a new run, RM0351 34.4.7,
  and with WAVE 1 only the first trigger starts one. A trigger before CNTSTRT or SNGSTRT starts
  nothing and sets no EXTTRIG, inferred. A trigger at the time of another term applies after it,
  inferred.
- CNT reads the exact count, so the double read of RM0351 34.7.8 always agrees.
- A kernel rate change, such as an LPTIM1SEL write or a PPRE1 change under PCLK1, keeps CNT,
  the kernel clocks already spent in the current tick or the start latency, and the kernel
  clocks left before each pending CMPOK, ARROK, CMPM, ARRM, mode switch and one-shot stop,
  inferred. A stopped kernel clock, LPTIMxEN 0 included, freezes all of them until it runs
  again, inferred.
- LPTIM1RST and LPTIM2RST reset the registers and the state of their LPTIM.
- The output drives the `lptim1_out` and `lptim2_out` trigger signals and no pin, so ES0250
  2.18.4, LPTIM1 outputs not open-drain, is outside the model. With WAVE 0 it rises one kernel
  clock after CNT reaches CMP and falls one kernel clock after CNT reaches ARR, as ES0250 2.18.3
  Figure 3 draws it, with PRESC above 0 inferred, RM0351 34.4.9 reads "exceeds". A one-shot run
  gives one such pulse, RM0351 34.4.9. With WAVE 1 it rises
  once and keeps its level until ENABLE is written to 0, and a start after the first one is
  discarded until then, RM0351 34.4.7, the software start reading inferred. CMP at or above ARR
  keeps it inactive, inferred. WAVPOL inverts it at the CFGR write, also with ENABLE 0, RM0351
  34.4.9. ENABLE written to 0 and LPTIMxRST drop it to the inactive level at the write, inferred.
- While the core is halted with DBG_LPTIM1_STOP or DBG_LPTIM2_STOP set, that LPTIM's counter
  clock stops, RM0351 34.4.14, 48.16.4 and 48.16.5. The model freezes it as a stopped kernel
  clock: CNT, the kernel clocks spent in the current tick and those left before each pending
  CMPOK, ARROK, match delay, mode switch and stop resume at the release, the latencies inferred.
  A kernel rate change during the hold applies from the release, inferred. The hold is kept
  across LPTIMxRST, RM0351 34.4.14. At a machine reset the LPTIM reads DBG_LPTIMx_STOP again,
  which a system reset keeps and a power-on reset clears, RM0351 48.16.4 and 48.16.5.
- EXTI direct lines 32 and 33 stay Stop wake-up gates: the LPTIM raises LPTIM1_IRQn and
  LPTIM2_IRQn itself, and Stop is not modelled. ES0250 2.18.1 and 2.18.2, an LPTIM interrupt
  stuck at Stop entry or at a flag clear racing a new event, are outside the model.

## MDF and ADF

Source: [mdf.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/mdf.cpp)

These settings raise `UnmodelledConfig` naming the register and the field when they take effect:

- CKGMOD 1, and an input-direction CCKy used by an enabled serial interface with CKGDEN 1.
- Manchester modes and the MDF_CKIx clocks.
- The ADCITF sources, the window and snapshot modes, and triggers other than TRGO.
- A reserved SCALE code.
- An ADF CICMOD other than Sinc4 and Sinc5, and ILVNB above the last filter.
- OLDEN and SCDEN.
- In the ADF, SCKSRC 1x, BSSEL above 1, TRGSRC other than 0000 and ACQMOD 100.
- A reserved SNTHR at SADEN 0 to 1.

ACQMOD 110 and 111 run as 000, MDF CICMOD 110 and 111 as Sinc5, and ADF ACQMOD 101 as 000.

Choices where RM0456 is silent or the model differs:

- With a generated CCK, CKABF stays 0 at SITFEN. Silicon holds it until the first clock
  transition.
- With CKGDEN 0, CKABF sets at SITFEN, and STH cycles at the PROCDIV rate after CKGDEN goes to 0.
- The sound activity detector sees only the samples NBDIS keeps. RM0456 taps it at the DFLT0
  output.
- A partial SAD frame carries across a DFLT0 stop and start.

## OTG_FS

Source: [otg_fs.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/otg_fs.cpp)

Not modelled, logged:

- Host mode, `GUSBCFG.FHMOD`. The core stays a device.
- Session request and host negotiation, `GOTGCTL` SRQ and HNPRQ.
- Battery charging detection, `GCCFG.BCDEN`.
- Test modes, `DCTL.TCTL`.
- A TX FIFO numbered other than its IN endpoint. DFIFOn feeds IN endpoint n.
- An isochronous endpoint armed with PKTCNT above 1. One packet goes per frame.

The timing is full speed. A firmware `GCCFG.PWRDWN` write that powers the transceiver, PWRDWN
going from 0 to 1, while the 48 MHz kernel clock, `RCC_CCIPR1.ICLKSEL` on the STM32U5 and
`RCC_CCIPR.CLK48SEL` on the STM32L4, is stopped or off 48 MHz by more than 0.25 % logs one
`model_warning` per machine life and raises `model_error` under `strict_peripherals`. A debugger
write, a later write while powered and a clock change after the power-up check nothing. 0.25 % is
the full-speed data rate tolerance of USB 2.0 7.1.11, the reference manuals give no figure,
inferred. A machine without an RCC model checks nothing.

The device connects only once firmware declares VDDUSB valid, `PWR_SVMCR.USV` on the STM32U5,
RM0456 10.4.6, and `PWR_CR2.USV` on the STM32L4, RM0351 5.1.3. `HAL_PWREx_EnableVddUSB` sets
it. With USV 0 and every other connect condition met the device stays disconnected and logs one
`model_warning` per machine life. A USV change, a PWR reset included, connects or disconnects
the device at once. Everything driven by the host's SOF stops with it, so on the STM32U5 a CRS
synchronised on the USB SOF, `CRS_CFGR.SYNCSRC` 2, gets no SYNC, no SYNCOKF and no trim until USV
is set. A test rig that attaches a host to the U5 OTG_FS sets USV first.

The STM32L47x/L48x core, RM0351 section 47, is the STM32U5 core with these differences:

- GRSTCTL bit 2 is FCRST, RM0351 47.15.5, read 0 again as soon as it is written, as FSRST on the
  STM32U5.
- No PSRST, ERRATIM, STSPHST or CURMOD, RM0351 Table 322. The PSRST note of 47.15.5 and the
  ERRATIM note of 47.15.35 agree: "This bit is available in STM32L49x/L4Ax products, reserved
  otherwise". The 47.15.1 GOTGCTL drawing shows CURMOD at bit 21. The model follows the table,
  inferred: GRSTCTL bit 1 and GOTGCTL bit 21 read 0.
- GSNPSID at offset 0x40 reads 0x4F54310A, with GHWCFG1 to GHWCFG4 and GDFIFOCFG at their L476
  reset values, inferred: RM0351 47.15 documents no register at 0x040 to 0x05C. The values come
  from the HAL core ID and a TinyUSB register dump of an STM32L476.
- The ADP registers `GPWRDN` and `GADPCTL`, RM0351 47.15.18 and 47.15.19, are storage. A
  firmware write logs one `model_warning` per register and raises `model_error` under
  `strict_peripherals` before it is stored.
- The model raises `OTG_FS_IRQn`, 67, itself. EXTI line 17, the OTG_FS wakeup, is a Stop mode
  wakeup line, RM0351 14.3, and stays idle.
- ES0250 2.27.1, a TX FIFO write sequence interrupted by an access to an endpoint register
  corrupts the FIFO on silicon. The model keeps the FIFO whole.
- ES0250 2.27.2 concerns host mode through a hub to a low-speed device. The core is a device.

## USB

Source: [usb_drd.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/usb_drd.cpp)

The USB full-speed device controller of the STM32H5 runs in device mode.

- A byte or half-word write of the packet memory stores its lanes and writes 0 to the other lanes
  of the word, from the CPU and the debugger alike, as measured on an STM32H573. A narrow read
  returns its lanes. Word accesses are the ones RM0481 61.7 allows, so a CPU byte or half-word
  access logs once and raises `ModelError` under `strict`. Debugger accesses of any width pass.
- A byte or half-word write of a USB register is a word write with the other lanes 0, as
  measured on an STM32H573 for `CHEPnR` and `DADDR` and applied to every register: in `CHEPnR` a
  toggle bit there stays, `VTRX` and `VTTX` there clear and `EA` or `UTYPE` there read 0.
- The packet memory reads 0 after a power-on reset and keeps its content across every other
  reset, an RCC USBFSRST pulse included.
- While `BCDR.DPPU_DPD` is set and `CNTR.SUSPEN` is clear, `ISTR.ESOF` rises and `FNR.LSOF`
  counts every 1 ms with no SOF, with a host attached or not. `SUSP` rises at each ESOF that
  brings the count of missed frames to 3, the count wrapping at 4 and starting again at a SOF, a
  bus reset, a resume and the timer start, so with no SOF it rises every 4 ms, as read on an
  STM32H573 with nothing attached and with a PC host before its bus reset. Setting `SUSPEN` stops
  the count and clears `LSOF`, as read on the part. The first ESOF comes 1 ms after the pull-up
  and each SOF moves the next one to 1 ms plus 1 us after it, both inferred. A bus reset stops
  the count while SE0 lasts, as read on an STM32H573 with a PC host after a reset with and
  without suspend. The next ESOF comes 1 ms after SE0 ends, as RM0481 61.6.2 enables the
  suspend check right after any USB reset. Clearing `DPPU_DPD` stops the count, and `FN`, `LCK`,
  `LSOF` and the `ISTR` flags keep their values, as read on the part.
- A bus reset or a SOF while `CNTR.SUSPEN` is set raises `ISTR.WKUP` and clears `SUSPEN` and
  `SUSPRDY`, RM0481 61.5.7 and 61.6.2, the bus reset as read on an STM32H573 with a PC host.
- `FNR.LCK` sets at the second SOF after a bus reset, with `ISTR.RST_DCON` pending or cleared,
  and reads 0 while `ISTR.WKUP` is set, as measured on an STM32H573 with a PC host after a bus
  reset. After a resume it sets at the second SOF, RM0481 FNR.LCK, and reads 0 while `WKUP` is
  set, inferred. A SOF while `WKUP` is set starts the count of two again, so `LCK` sets at the
  second SOF after the firmware clears `WKUP`, inferred: on the part `WKUP` was cleared before
  SE0 ended. A firmware that leaves `WKUP` set reads `LCK` 0.
- FNR.RXDP and FNR.RXDM read 1 and 1 with no host attached, as measured on an STM32H573. With a
  host attached they read SE0, 0 and 0, while the device does not pull D+ up, and the idle J
  state, 1 and 0, while it does, as measured on an STM32H573 with a PC host.
  A suspend raises SUSP after the host's 3 ms of idle bus.
- Transfers run whatever kernel clock `RCC_CCIPR4.USBFSSEL` selects.
- A double-buffered endpoint fills or sends descriptor word 2n while its DTOG is 0 and word
  2n + 1 while it is 1, in both directions, as RM0481 61.5.3 states in its text and the ST HAL
  and tinyusb drivers expect.
- The first transaction of a double-buffered bulk endpoint after `EPKIND` or `UTYPE` changes, or
  after a reset, sets its STAT to NAK. Every later transaction keeps STAT VALID.
- Writing `CNTR.USBRST` from 0 to 1 clears every `CHEPnR` but `VTRX` and `VTTX`. `DADDR`,
  `ISTR.RST_DCON` and `FNR.LCK` stay as they are, `DADDR` as measured on an STM32H573.
- A written 0 keeps `CNTR.SUSPEN`, as measured on an STM32H573. A resume and a bus reset clear
  it.
- A bus reset clears the endpoints and `DADDR` when SE0 starts, inferred, and sets
  `ISTR.RST_DCON` when SE0 ends, as measured on an STM32H573 with a PC host.
- Tokens get no answer while `ISTR.RST_DCON` is set.
- A SETUP to control endpoint 0 is acknowledged with `STATRX` NAK or STALL, and leaves both
  toggles at 1.

Not modelled, logged:

- Host mode, `CNTR.HOST`. The controller stays a device.
- LPM, `LPMCSR.LPMEN`.
- Battery charging detection, `BCDR.BCDEN`.
- Remote wake-up signalling, `CNTR.L1RES` and `CNTR.L2RES`.

On the STM32L1, RM0038 section 24, the same model runs with the L1 register names: `EPnR` with
`STAT_TX`, `DTOG_TX`, `CTR_TX`, `EP_KIND`, `EP_TYPE`, `STAT_RX`, `DTOG_RX` and `CTR_RX`,
`ISTR.RESET` and `EP_ID`, and `CNTR.FRES`, `FSUSP`, `LPMODE` and `RESUME`.

- The rules above that were measured on an STM32H573 apply as inferred: the narrow register
  write, the suspend timer, `FNR.LCK`, `FNR.RXDP` and `RXDM`, and the first transaction of a
  double-buffered bulk endpoint.
- The packet memory is 512 bytes in the `USB_SRAM` peripheral, local half-word k in the low
  half of the word at offset 4k, RM0038 24.5.3. The upper half reads 0 and ignores writes,
  inferred. Half-word and word accesses of the low half pass. A CPU byte access or an access of
  the upper half logs once and raises `ModelError` under `strict`. Debugger accesses of any
  width pass.
- The descriptor table starts at `BTABLE`, and a buffer address ignores bit 0, inferred from
  RM0038 24.5.3, which says bit 0 must be written 0.
- The D+ pull-up is `SYSCFG_PMC.USB_PU`, RM0038 24.4.2. A write or a SYSCFG reset that changes
  it tells the host at once. `LCD_CAPA` in the same register is storage: a CPU write that sets a
  bit of it logs once and raises `ModelError` under `strict`.
- `FRES` written from 0 to 1 clears every `EPnR` but `CTR_RX` and `CTR_TX` and clears `DADDR`,
  RM0038 24.5.1 `CNTR.FRES`, which resets the peripheral like a USB reset, and `ISTR.RESET`,
  which resets the function address and endpoint registers at a USB reset. `ISTR.RESET` sets at
  the write, inferred.
- `FSUSP` and `LPMODE` keep what the firmware writes. While `FSUSP` is set the suspend timer
  stops. A bus reset, a SOF or a resume while `FSUSP` is set raises `WKUP` and clears `LPMODE`,
  and `FSUSP` stays set, RM0038 24.4.5.
- `USB_LP` is any `ISTR` flag whose `CNTR` mask is set. `USB_HP` is a `CTR_RX` or `CTR_TX` of an
  isochronous or double-buffered bulk endpoint whatever `CNTR.CTRM` holds, inferred from RM0038
  24.3.1 and the endpoint interrupt condition of 24.5.2.
- EXTI line 18 follows `ISTR.WKUP`: it rises when `WKUP` sets and falls when the firmware clears
  it, inferred from RM0038 24.5.1, so `USB_FS_WKUP` needs the line's EXTI rising edge and mask.

Not modelled on the STM32L1, logged, `ModelError` under `strict`:

- Remote wake-up signalling, `CNTR.RESUME`.

## PWR

Source: [pwr.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/pwr.cpp)

- The low-power modes `PWR_CR1.LPMS` selects on the U5 and `PWR_PMCR.LPMS` on the H5 are not
  modelled. A deep-sleep WFI idles as a plain WFI.
- STM32U5: the voltage scaling ready bits follow a VOS write after the board's `ready_delay`,
  50 us by default. A write that steps to a higher voltage clears VOSRDY for that delay, one that
  steps to a lower voltage keeps VOSRDY set. ACTVOSRDY clears for the delay on every change.
- STM32H5: a VOS write moves ACTVOS one level toward VOS at once and one more level every
  10.4 us, and VOSRDY and ACTVOSRDY set 15.2 us after the last step. VOS3 to VOS0 reads
  0x4000, 0x8000 and 0xC000 in PWR_VOSSR, then 0xE008 36 us after the write, as measured on an
  STM32H573. A step to a lower voltage keeps VOSRDY set and takes the same steps, and a change
  of one or two levels follows the same rule. The measurement covers VOS3 to VOS0 only. FLASH
  checks its latency against the range of the last ready.
- STM32H5: PWR_USBSCR is storage, the USB model runs whatever USB33DEN and USB33SV hold.
- STM32H5: `PWR_SCCR` resets with SMPSEN (0x200) on the packages with the SMPS regulator, the
  part names ending in Q, and with LDOEN (0x100) on the others, RM0481 10.11.11. Both bits are
  read-only. The supply the model runs is the board's.
- STM32U5 packages with the SMPS, the part names ending in Q: `PWR_SVMSR.REGS` takes the value
  of `PWR_CR3.REGSEL` after the typical transition time of DS13737 Table 75 for the range
  ACTVOS gives at the REGSEL write: LDO to SMPS 14, 17, 16 and 16 us in range 4 to 1, SMPS to
  LDO 16, 15, 14 and 14 us. The figures are the DS13737 typical values, applied to every U5 SMPS
  part. Every reset sets both to 0, the LDO.
- STM32U5 packages without the SMPS: REGSEL is reserved, RM0456 10.10.3, and REGS stays 0. A
  firmware write that changes REGSEL from 0 to 1 logs one warning per machine life. Under
  `strict_peripherals` it is a `model_error` and `PWR_CR3` keeps its value before the write.
  Otherwise the firmware REGSEL write is stored, an inferred rule.
- STM32U5: `PWR_CR3.FSTEN` is storage.
- STM32L1: a VOS write to another range sets `PWR_CSR.VOSF` for the board's `ready_delay`, 50 us
  by default, in either direction, and the new range applies when VOSF clears, RM0038 5.4.1 and
  5.4.2. The delay is not a silicon figure. A write of VOS 00 keeps the range, RM0038 5.4.1. A
  firmware write of it also logs one warning per machine life and is a `model_error` under
  `strict_peripherals`.
- STM32L1: the low-power modes `PWR_CR.PDDS` selects are not modelled, the first PDDS change logs
  one warning. `DBP` does not gate the RTC domain bits of `RCC_CSR` or the RTC registers. LPSDSR,
  LPRUN, ULP, FWU, the PVD and the wakeup flags are storage. A peripheral reset through
  `RCC_APB1RSTR.PWRRST` keeps FWU and ULP, RM0038 5.4.1.
- STM32L47x/L48x: a VOS write to the other range sets `PWR_SR2.VOSF` for the board's
  `ready_delay`, 50 us by default, in either direction, and the new range applies when VOSF
  clears, RM0351 5.1.8 and 5.4.6. DS10198 Table 53 gives tVOST 20 us typical and 40 us maximum,
  the board option sets the delay. A write of VOS 00 or 11 keeps the range, RM0351 5.4.1. A
  firmware write of either also logs one warning per machine life and is a `model_error` under
  `strict_peripherals`.
- STM32L47x/L48x: the low-power modes `PWR_CR1.LPMS` selects are not modelled, the first LPMS
  change logs one warning naming the mode, and a deep-sleep WFI idles as a plain WFI. A firmware
  write that sets `LPR`, `PVDE`, `PVME1` to `PVME4`, `EWUP1` to `EWUP5` or `APC` from 0 logs one
  warning per field per machine life and stores the bit. Under `strict_peripherals` it is a
  `model_error` and the register keeps its value before the write. `REGLPS`, `REGLPF`, `PVDO`,
  the `PVMOx` flags and the wakeup and Standby flags stay 0.
- STM32L47x/L48x: `RCC_BDCR` and the RTC registers take writes whatever `DBP` holds. `USV`, `IOSV`, `PLS`,
  `RRS`, `EIWUL`, `PWR_CR4`, `PWR_SCR` and `PWR_PUCRx` and `PWR_PDCRx` are storage. A peripheral
  reset through `RCC_APB1RSTR1.PWRRST` resets every PWR register.
- STM32L47x/L48x PWR errata, ES0250 2.2: a `PWR_CR4` write takes effect whatever low-power entry
  follows it, 2.2.21. The PWR model raises no reset, so a short run sequence gives no brown-out
  reset, 2.2.5.

## RCC

Source: [rcc.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/rcc.cpp), [rcc_l1.hpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/rcc_l1.hpp)

- A peripheral's bus clock enable, `<instance>EN` or its RM spelling in an `RCC_xxxENR` register,
  gates its register access: while the bit is 0 a CPU or DMA read gives 0 and a write is lost, no
  model handler runs, one `model_warning` per peripheral per machine life names the first such
  access, and under `strict_peripherals` the access raises `model_error`. Debugger accesses are
  served. Read 0 and write lost are inferred: RM0456, RM0481, RM0351 and RM0038 say only that
  the access is not supported. The enable gates no activity of the peripheral: its timers,
  transfers and events run on. A gated write reaches no model check, such as an illegal-access
  event of a TrustZone-aware model, inferred.
- The STM32L1 and STM32L4 RTC is gated on `RTCEN` of `RCC_CSR` or `RCC_BDCR`, which a system
  reset keeps, as RM0038 5.1.3 and RM0351 5.1.5 list it among the steps of RTC register access.
  The STM32U5 and STM32H5 RTC and TAMP are gated on `RTCAPBEN`.
- Instances whose clock the reference manual names only through their host block are gated on the
  host's bit, inferred: the GTZC TZIC and MPCBB blocks on the GTZC or TZSC enable, the OCTOSPI
  delay blocks on the OCTOSPI enable, the STM32U5 SDMMC delay blocks on the SDMMC enable,
  STM32U5 FDCAN1_RAM on `FDCAN1EN`, STM32L1 OPAMP on `COMPEN` and STM32L1 USB_SRAM on `USBEN`.
  The STM32H563 and STM32H573 ETH is gated on `ETHEN` alone, `ETHTXEN` and `ETHRXEN` are
  storage, inferred.
- RCC, EXTI, IWDG and DBGMCU, the STM32H5 PWR and the STM32U5 and STM32H5 ICACHE have no enable
  and are never gated. FLASH and the STM32U5 and STM32H5 DCACHE enables reset to 1.
- SRAMnEN and SRAMnSMEN stay Non-secure. The MPCBB and INVSECSTATE rule of RM0456 5.4 does not
  apply to them.
- A peripheral reset bit in an `RCC_xxxRSTR` register, written from 0 to 1, resets every
  register of that instance that no model claims, for any instance, modelled or not, such as
  LPTIM1. It also resets the instance's model when the model supports it: RNG, CRC, HASH,
  SYSCFG, ADC1 and ADC12 through ADC12RST on the STM32U5, ADC1, ADC2 and ADCC on the STM32H5,
  GPDMA1, LPDMA1, the timers, GPIOA to GPIOI, SPI, I2C, SAI, MDF1 and ADF1 on the STM32U5, the
  USARTs, UARTs, LPUART1 on the STM32U5 and LPUART on the STM32H5, OTG_FS on the STM32U5 and USB
  on the STM32H5. A native plugin model that derives
  from `periph::bus_reset_listener`, and every Python model, hears it through `on_bus_reset()`.
  Any other model gets `reset()` with `reset_kind::peripheral`. A peripheral reset leaves the
  flags of RCC_CSR, and of RCC_RSR on the STM32H5, as they are.
- Board state survives such a reset: a pin's board drive, SPI devices and I2C targets, which an
  RCC reset of their controller does not reset, PDM sources, SAI external clocks and the frames
  a test queued for a USART receiver.
- A USART reset mid-frame aborts the frame on its UartLink, as clearing UE does. Frames the host
  queued wait until firmware enables the receiver again.
- An OTG_FS reset releases the pull-up, so the board's USB host sees a detach and enumerates the
  device again once firmware reconnects. The cable stays plugged or unplugged as it was.
- A GPIO reset returns its pins to their reset modes, analog except PA13 to PA15, PB3 and PB4,
  so outputs drop and listeners see the edges. A pin in analog mode reads low whatever the board
  drives, so an active-low SPI chip select on a reset port selects its device until firmware
  configures the pin again.
- SYSCFGRST keeps `SYSCFG_CSLCKR`, `SYSCFG_CNSLCKR` and `SYSCFG_CFGR2`, whose bits RM0456 15.3
  clears only on a system reset.
- While a reset bit is 1, bus writes to that peripheral are ignored, from the CPU, DMA and
  debugger alike. The TrustZone filter still runs first, so an illegal access still raises its
  event. Reads are not blocked. RM0456 gives no access rule for a peripheral held in reset.
- TrustZone: an RCC bit shared by several peripherals is Secure while any of them is, on the
  STM32U5 and the STM32H5 alike. RM0456 11.5.1 and RM0481 11.5.1 secure "its related clock
  source selection" for each Secure peripheral and name no rule for a shared one. The shared bits
  are `ADCDACSEL`, `LPTIM34SEL`, `OCTOSPISEL`, `SDMMCSEL` and `TIMICSEL` on the STM32U5,
  `ADCDACSEL`, `TIMICSEL` and the `FDCAN12` bits on the STM32H5.
- STM32H5 TrustZone: `RCC_CR.HSIDIV`, `HSIDIVF` and the `RCC_CFGR2` `AHBxDIS` and `APBxDIS` bits
  take Non-secure writes, RM0481 Table 122 lists them under no security bit.
- SWS follows SW in the store that selects a ready source. On an STM32H573, the first one or two
  reads of `RCC_CFGR1` after that store return the old SWS. Firmware that polls SWS ends in the
  same state.
- RCC holds each write RM0456 11.8 and RM0481 11.8 say the hardware ignores, with no log record, on
  the STM32U5 and the STM32H5 alike: a clear of the ON bit of the SYSCLK source and of the PLL1
  input while PLL1 drives SYSCLK, on the STM32H5 a clear of HSEON, HSION or CSION while that
  oscillator feeds PLL1 and the same write leaves PLL1ON set, RM0481 11.8.1, a write of
  `PLLxSRC`, `PLLxM`, `PLLxRGE`, `PLLxN` and the output dividers while the PLL is on or ready, on
  the STM32H5 of PLL1PEN, PLL1QEN and PLL1REN too, RM0481 11.8.7, and a clear of the PLL1 output
  enable that feeds SYSCLK. The STM32U5 `PLL1MBOOST` is storage. ST's CMSIS `SystemInit` run with
  PLL1 as SYSCLK logs nothing.
- A switch to a SYSCLK source that runs at 0 Hz, such as a PLL1 output whose enable is clear,
  keeps the core clock at its last rate and logs one `model_warning` per machine.
- A PLL loads `PLLxFRACN` as its fraction when `PLLxON` sets with `PLLxFRACEN` 1, and runs in
  integer mode with `PLLxFRACEN` 0, whatever order firmware wrote them in, as measured on an
  STM32H573. While the PLL runs, it takes a new `PLLxFRACN` only on a `PLLxFRACEN` write from 0
  to 1 and keeps the loaded fraction across a `PLLxFRACEN` clear. That running rule follows
  RM0456 11.4.6 and RM0481 11.4.5, unmeasured. The new fraction applies at once, where silicon
  takes it three reference clock periods later.
- STM32H5: `RCC_CR.HSIDIV` keeps its value while PLL1 drives SYSCLK from HSI and takes a write in
  every other state, an enabled PLL1 off SYSCLK fed by HSI included, as measured on an STM32H573
  for PLL1. For PLL2 and PLL3 fed by HSI the model applies the same rule, inferred from PLL1. One
  write that clears PLL1ON of a PLL1 off SYSCLK and changes `HSIDIV` takes both.
- STM32H5: after an `HSIDIV` change, `HSIDIV` reads the new value at once. `HSIDIVF` clears and sets
  again after the cycles of the running HSI rate measured on an STM32H573: /2 to /4 or /8 clear at 3
  and set at 6, /2 to /1 3 and 16, /4 to /2 2 and 8, /4 to /1 2 and 14, /8 to /1 2 and 8. The other
  transitions are inferred: from /1 they take the /2 row, /4 to /8 and /8 to /2 and /4 the /4 to /2
  timing. The new HSI rate applies when `HSIDIVF` sets, a point the board run did not measure. On an
  STM32H573 ST's HAL reads `HSIDIVF` about 24 cycles after the store and reads 1 for every
  transition. The model runs one cycle per instruction, so the same load lands 13 cycles after the
  store and reads 1, except for /2 to /1 and /4 to /1, where it reads 0 and HAL takes `HSI_VALUE`
  either way.
- STM32U5 and STM32H5: `HSEBYP` in `RCC_CR` and `LSEBYP` in `RCC_BDCR` take writes while their
  oscillator runs, where RM0456 and RM0481 allow a write with the oscillator off only.
- An oscillator whose ON bit a system reset keeps and whose ready bit is 0, such as an LSE still
  starting when the reset comes, starts again at the reset and is ready after the board's
  `ready_delay`.
- Without TrustZone, a coverage report classes `RCC_CIFR` and `RCC_CICR` as `unimplemented`. The
  RCC sets the `RCC_CIFR` flags `RCC_CIER` enables, and a `RCC_CICR` write clears them.
- STM32L1: every category runs one clock tree, RM0038 section 6, with the enable, reset and
  low-power bits of a peripheral the category lacks absent from its description. The RCC sets the
  `RCC_CIR` ready flags its enables select, a `xxxRDYC` write clears them, and no ready interrupt
  is raised. A system reset keeps the RTC domain bits of `RCC_CSR`, `RTCPRE` and `HSEBYP`, a power
  on reset and a BOR reset them, RM0038 6.1.2 and 6.3.14. A BOR sets PORRSTF and PINRSTF,
  inferred. `RTCRST` resets the RTC domain bits of
  `RCC_CSR` while it is 1. The LSE CSS bits exist on the L100, L151 and L162 from Cat.2, and
  OBLRSTF on every line but the L152, RM0038 6.3.4 and 6.3.14 against the SVDs, inferred. A
  software reset sets SFTRSTF only, the PINRSTF of the NRST pulse is not
  set, an inferred rule. The reset flags of `RCC_CSR` keep their value on a write, only RMVF and a
  power reset clear them, inferred from RM0038 6.3.14.
- STM32L1: `RTCPRE` and `HSEBYP` keep their value on a write while `HSEON` is 1 and `LSEBYP` while
  `LSEON` is 1, RM0038 6.3.1 and 6.3.14. The ON bit is the one before the write, so one store that
  clears `HSEON` and changes `RTCPRE` keeps `RTCPRE`, an inferred order. `RTCRST` clears `LSEBYP`
  while the LSE runs, RM0038 6.3.14.
- STM32L1: `MSIRANGE` 7 and the PLLMUL and PLLDIV codes RM0038 6.3.2 and 6.3.3 do not allow run
  their output at 0 Hz, an inferred rule. The PLL VCO and SYSCLK limits of each voltage range are
  not checked. The CSS on HSE and on LSE is storage, and `LSECSSON` can be cleared by firmware.
  The USB and SDIO 48 MHz clock, PLLVCO / 2, is not a clock domain.
- STM32L47x/L48x: one clock tree, RM0351 section 6. The RCC sets the `RCC_CIFR` ready flags
  that `RCC_CIER` enables, a `RCC_CICR` write clears them, and no ready interrupt is raised. Each
  MSI range runs at its nominal rate, with or without `MSIPLLEN`, and HSI16 at 16 MHz: the
  `RCC_ICSCR` trim fields are storage. A ready flag clears in the write that clears its ON bit,
  where RM0351 6.4.1 and 6.4.30 give 6 oscillator cycles, 3 for the LSI.
- STM32L47x/L48x: a write of `MSIRANGE` while `MSION` is 1 and `MSIRDY` is 0, which RM0351 6.4.1
  forbids, takes the new range. `LSEBYP` takes every write, RM0351 6.4.29 states no rule for it.
  `HSIKERON`, `HSIASFS` and `STOPWUCK` are storage, they act on Stop mode entry and exit. The CSS
  on HSE and on LSE and `LSEDRV` are storage, `CSSF`, `LSECSSF` and `LSECSSD` stay 0. `RCC_BDCR`
  takes writes whatever `PWR_CR1.DBP` holds.
- STM32L47x/L48x: the main PLL, PLLSAI1 and PLLSAI2 run at the rate their PLLSRC, PLLM, N and
  output dividers give, RM0351 6.4.4 to 6.4.6, for any VCO input and output: the 4 to 16 MHz
  input and 64 to 344 MHz output ranges and the 80 MHz output limits are the firmware's to
  keep. A PLL is ready after the board's `ready_delay`. The PLLSAI1 and PLLSAI2 outputs reach
  no kernel clock until the `RCC_CCIPR` muxes select them.
- STM32L47x/L48x: `ADCRST` holds ADC1, ADC2, ADC3 and ADC_Common in reset together, RM0351
  6.4.11. The enable bits gate the register access of their peripherals, the timer clocks and the
  kernel clocks `RCC_CCIPR` selects. `MCOSEL` and `MCOPRE` are storage, no clock reaches the MCO
  pin. The sleep and stop enables of `RCC_xxxSMENR` are storage.
- STM32L47x/L48x: `SAIxSEL` 11 selects `SAIx_EXTCLK`, which runs the SAI kernel clock at 0 Hz.
  `LSCOSEL` and `LSCOEN` are storage, no clock reaches the LSCO pin, and keep their value
  through `BDRST`, RM0351 6.4.29. `FWRSTF` stays 0, the firewall raises no reset.
- STM32L47x/L48x clock errata, ES0250 2.2: `MSIRDY` clears whenever the MSI stops, 2.2.1. The MSI
  runs at its range rate from the first cycle after a wakeup, 2.2.3 and 2.2.24. The LSI runs at
  32 kHz with a 50 % duty, 2.2.8. The HSE is ready after the board's `ready_delay` at any VDD,
  2.2.10. The HSI16 user trim is storage, 2.2.22. A PLL locks at any VCO rate and temperature,
  2.2.25. The LSE runs at the board rate whatever PC13 does, 2.2.11.

## RNG

Source: [rng.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/rng.cpp)

The words come from a seed in the machine description or a queue the test fills, never from a
noise source. The model differs from RM0456 Rev 7 chapter 48 here:

- `RCC_AHB2ENR1.RNGEN` 0 gates the register access, see RCC, and stops generation.
- `RCC_AHB2RSTR1.RNGRST` written from 0 to 1 resets the RNG, CONFIGLOCK included, and keeps
  the entropy test controls, as every other reset kind does. While the bit is 1, writes are
  ignored as the RCC section says, and a debugger read of DR shows the empty FIFO.
- There is no noise source. NSCR and HTCR are storage, and SECS comes only from a test's fault
  injection.
- Configuration B timing is not modelled. The configuration A and C column of Table 463 applies
  to every configuration.
- A clock rate change does not rescale an init or a conditioning round already started.
- The tamper output to TAMP is not modelled.
- With ARDIS 0, SECS clears 2 AHB plus 2 RNG cycles after the injected fault is released. RM0456
  gives no auto-reset time.

On the STM32L4, RM0351 section 27:

- RNG_CR holds IE, RNGEN and CED. The ST SVDs and CMSIS headers lack CED, the description adds
  it at bit 5, RM0351 27.7.1. DR holds one word: the first comes 46 RNG clock cycles after
  RNGEN 1, the next 42 cycles after the read that empties DR, 27.5. RNGEN 0 keeps the DR word
  and ends the word in progress, 27.3.8.
- With CED 0 and RNGEN 1, CECS is 1 while fRNG is below fHCLK / 16 and its rise sets CEIS,
  which stays 1 until written 0, 27.7.2. Words keep coming at the unchanged rate, inferred,
  RM0351 27.3.7 as corrected by ES0250 2.15.1.
- A CED write while RNGEN is 1 before and in the write keeps CED, inferred. RM0351 27.7.1 asks
  for the RNG disabled and gives no rule for such a write.
- A seed error sets SECS and SEIS and stops generation, 27.3.7, and DR reads 0, the 27.3.5 note
  and 27.7.3.
- The 27.3.7 recovery, SEIS cleared and RNGEN cleared and set, restarts the RNG. SECS clears
  when RNGEN is written 0 and stays 1 with RNGEN 1 after the injected fault ends, both inferred:
  RM0351 gives the recovery sequence, not when SECS clears.
- RNGEN 1 while DR holds a word starts the 46 cycles at the read that empties DR, inferred.

## RTC and TAMP

Source: [rtc.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/rtc.cpp), [rtc_classic.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/rtc_classic.cpp)

Not modelled, logged:

- Binary and mixed modes. The calendar counts in BCD.
- Smooth calibration and the shift register.
- Timestamps, the RTC outputs, ALRxFCLR and SSRUIE.
- WUTOCLR. WUTF stays set until SCR clears it.
- The backup domain write protection. `PWR_DBPR.DBP` does not gate RTC writes.
- Subsecond alarm comparison. MASKSS reads as 0.
- The security and privilege `RTC_SECCFGR`, `RTC_PRIVCR`, `TAMP_SECCFGR` and `TAMP_PRIVCR`
  select. They are storage.

The STM32L1 RTC:

- INITF sets at the second RTCCLK edge after INIT, and the calendar restarts at the fourth
  edge after INIT clears, the upper bounds of RM0038 20.3.5.
- RSF sets at the second RTCCLK edge after a clear, the end of init mode, a BYPSHAD clear or a
  system reset, inferred from the copy every two RTCCLK periods of RM0038 20.3.2.
- TR, DR and SSR read the calendar at the time of the read. A read of SSR or TR holds TR and DR
  until DR is read, and after a system reset they read their reset values until RSF sets.
- TAMP2F and TAMP3F clear by a 0 under the `RTC_WPR` key only, RM0038 20.3.5 and the note
  under 20.6.4, which exempt `RTC_ISR[13:8]`.
- `RTC_TSTR`, `RTC_TSDR`, `RTC_TSSSR`, `RTC_CALIBR`, `RTC_CALR`, `RTC_SHIFTR` and `RTC_TAFCR`
  are storage: coverage reports them `storage_only`, and `m.mem` and the register views refuse
  them as they refuse a peripheral without a model.
- ADD1H and SUB1H move the calendar at the write.
- RTC writes land whatever `PWR_CR.DBP` holds.
- On Cat.1 a PREDIV_A of 0 divides by 1.
- The alarm interrupt, ALRxF with ALRxIE, drives EXTI line 17 and RTC_Alarm, and WUTF with
  WUTIE drives EXTI line 20 and RTC_WKUP, RM0038 20.5. The flag gated by its interrupt enable is
  inferred from RM0038 Table 90. A flag set while its enable is 0 raises the line when the
  enable sets.
- On Cat.1 ALRAWF and ALRBWF clear when their alarm is enabled and in init mode, and set at the
  second RTCCLK edge after the alarm is disabled, inferred from RM0038 20.3.5. On Cat.2 to Cat.6
  they read 1, so `RTC_ALRMxR` takes writes while its alarm is enabled, inferred.
- The wakeup timer counts from the write that sets WUTE, inferred. WUTWF clears and sets at the
  second RTCCLK edge after WUTE changes, the upper bound of RM0038 20.6.4.
- A system reset keeps a pending set of WUTWF, ALRAWF or ALRBWF, inferred from the bit text of
  RM0038 20.6.4. A system reset in init mode restarts the calendar as the end of init mode does,
  inferred: on Cat.1 ALRxWF reads 1 at once for an alarm with ALRxE clear, and a ck_spre wakeup
  counts its period from the restart. The U5 and H5 RTC rebase a ck_spre wakeup the same way,
  inferred.
- Logged once, `ModelError` under `strict` at the CPU or DMA write that selects them, before
  the store: timestamps, tamper detection, coarse and smooth calibration, the shift control, the
  RTC_CALIB and RTC_ALARM outputs and the reference clock detection. A debugger write stores
  them with neither.

The STM32L4 RTC, RM0351 section 38, runs the STM32L1 rules above on its own register set:

- `RTC_TAMPCR` at 0x40 in place of `RTC_TAFCR`, `RTC_OR` at 0x4C, no `RTC_CALIBR` and no
  `CR.DCE`.
- RSF sets at the first RTCCLK edge after a clear, the end of init mode, a BYPSHAD clear or a
  system reset, inferred from the copy every RTCCLK period of RM0351 38.3.4 and 38.3.8, where the
  BYPSHAD text of 38.6.3 says every two RTCCLK cycles.
- The `RTC_WPR` key leaves `RTC_ISR[31:8]`, `RTC_TAMPCR`, `RTC_OR` and the backup registers open,
  RM0351 38.3.7: ALRAF to TAMP3F clear by a 0 without it.
- `RTC_TAMPCR` and `RTC_OR` are storage: coverage reports them `storage_only`. `RTC_ISR.ITSF`,
  bit 17 in RM0351 38.6.4, is not in the description and reads 0.
- A brown-out keeps the calendar and the backup registers, RM0351 6.1.3.
- The alarm interrupt, ALRxF with ALRxIE, drives EXTI line 18 and RTC_Alarm, RM0351 Table 62,
  and WUTF with WUTIE drives EXTI line 20 and RTC_WKUP. The flag gated by its interrupt enable is
  inferred from RM0351 Table 230.
- ALRAWF and ALRBWF read 1, so `RTC_ALRMxR` takes writes while its alarm is enabled, inferred:
  RM0351 38.3.7 programs the alarm with no ALRxWF poll, where RM0038 20.3.5 skips the poll on the
  STM32L1 Cat.2 to Cat.6, whose flags are always set. `RTC_ALRMxSSR` takes writes with ALRxE clear
  or in init mode, RM0351 38.6.17 and 38.6.18.
- Logged once, `ModelError` under `strict` at the CPU or DMA write that selects them: the internal
  timestamp `CR.ITSE`, smooth calibration, the shift control, the outputs and the reference clock
  detection.
- Tampers, RM0351 38.3.14: RTC_TAMP1 on PC13, RTC_TAMP2 on PA0 and RTC_TAMP3 on PE6, DS10198
  Table 16. A pin the package does not bond, such as PE6 on the 64-pin parts, is never active.
  With TAMPFLT 0 an edge of the TAMPxTRG polarity is an event. With TAMPFLT 1 to 3 the input is
  sampled every 2^(15 - TAMPFREQ) RTCCLK cycles, on multiples of that period counted from the
  calendar's RTCCLK origin, and 2, 4 or 8 consecutive samples at the TAMPxTRG level make an
  event. The sample phase, and the count restarting after each event, are inferred.
- A sample of an undriven tamper input reads high with TAMPPUDIS 0, the precharge, and its PUPDR
  pull-up with TAMPPUDIS 1. TAMPPRCH sets no timing: the board drive is ideal.
- TAMPxF sets at the event with TAMPFLT 0 and TAMPTS 0, else 3 ck_apre cycles later, RM0351
  38.3.14, counted from the event, inferred. The backup registers are erased when TAMPxF sets,
  unless TAMPxNOERASE, inferred: RM0351 38.3.14 erases at the tamper event and gives the latency
  for TAMPxF only. An event while TAMPxF is set, or while one is pending, is lost.
- TAMPxMF masks TAMPxF and the erase. The tamper trigger to the LPTIM is not connected. A TAMPIE
  or TAMPxIE write with a TAMPxMF set is stored as written. With TAMPxMF an event is lost during
  the latency only, where RM0351 38.3.14 adds 2.5 ck_rtc cycles to that window.
- TAMPTS stamps the calendar of the tamper event as a timestamp does, at the board time of an
  edge. TSF sets with TAMPxF, and TSOVF at once when a stamp is set or pending, RM0351 38.3.13.
  TAMPIE, or TAMPxIE, with TAMPxF drives EXTI line 19.
- ES0250 2.19.1 is modelled: clearing TAMPxE of a falling-edge tamper, TAMPFLT 0 and TAMPxTRG 1,
  while its input is high is a tamper event. It erases unless TAMPxNOERASE was set by an earlier
  write.
- ES0250 2.19.5: with DBG_RTC_STOP a held core stops the alarms, the subsecond alarm included,
  and no flag repeats during the hold.
- ES0250 2.19.6: a tamper event right after a TSF clear stamps as any other event, with no APB
  cycle window.
- ES0250 2.19.7: Stop 2 is not modelled, and RTC_REFIN and RTC_OUT on PB2 are outputs and inputs
  the model does not drive.
- Timestamps, RM0351 38.3.13: with `CR.TSE` an edge of RTC_TS on PC13, rising with TSEDGE 0 and
  falling with 1, captures the calendar at the board time of the edge, also when the GPIO applies
  it at a later block boundary, into `RTC_TSTR`, `RTC_TSDR` and `RTC_TSSSR`
  and sets TSF 2 x (PREDIV_A + 1) RTCCLK cycles later, the "2 ck_apre cycles" of 38.3.13 counted
  from the edge, inferred. An edge while TSF is set or still pending sets TSOVF at once and keeps
  the stamp, the 38.3.13 note "TSOVF can be seen as '1' while TSF is still '0'". A TSF clear
  zeroes the three registers. TSF with TSIE drives EXTI line 19 and TAMP_STAMP_IRQn.
- An edge right after a TSF clear records a new stamp with TSF set and TSOVF 0, where the
  38.3.13 Caution sets both TSF and TSOVF for an event immediately after the clear.
- RTC_TS reads PC13 whatever its GPIO mode, RM0351 38.3.2: the board drive, else the PUPDR pull-up,
  inferred for an undriven pin. A PUPDR write and a TSEDGE write make no timestamp edge.
- The 32 backup registers keep their value through a system reset. `RCC_BDCR.BDRST`, a power-on
  and a read protection regression from level 1 to level 0 clear them, RM0351 38.6.20, 6.1.3 and
  3.5.1.
- The model holds no VDD or VBAT supply level, so the missed backup domain power-on reset of
  ES0250 2.2.13 has no trigger.
- ES0250 2.19.2: an SSR or TR read always holds TR and DR until DR is read, whatever the APB clock
  phase against the shadow copy.
- ES0250 2.19.3: alarm A and alarm B drive one level on EXTI line 18, so a request that sets while
  the other is still set makes no new edge. An `EXTI_PR1` clear takes effect at the write, with
  no delay window.
- ES0250 2.19.4: INITF sets at the second RTCCLK edge after INIT for every INIT entry, a
  consecutive one included.

## SAI

Source: [sai.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/sai.cpp)

- SPDIF, AC'97, PDM, companding, mute and synchronization with another SAI are not modelled.
- STM32L4 SAI1 and SAI2, RM0351 section 43, run on the registers GCR to BDR, OSR read as 0. MCKDIV
  above 0 divides the kernel clock by 2 x MCKDIV and MCKDIV 0 by 1. A master frame lasts 256 such
  divided clocks with NODIV 0, and FRL + 1 kernel clocks with NODIV 1, which bypasses MCKDIV, RM0351
  43.3.8.
- The external clock always matches the block's frame, so AFSDET and LFSDET stay 0 and the I2S
  slave resynchronisation of ES0250 2.24.1, a reference manual correction, has nothing to act
  on.
- The SAI drives no pin, its frames go to the board's audio endpoints, so ES0250 2.24.2 to
  2.24.4, the SCK and MCLK pulses at a master disable and the MCLK output with FRL 0xFF, lie
  outside the model.

## SPI

Source: [spi.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/spi.cpp), [spi_classic.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/spi_classic.cpp)

- STM32U5 and STM32H5: half duplex, CRC, TI mode, the autonomous trigger and SS timing are not
  modelled. The SPI runs as a plain full-duplex or simplex SPI. One model_warning per machine comes at the first transfer
  started with SPI_CFG2.SP, MIDI or MSSI or SPI_CFG1.CRCEN set.
- The dummy frame RM0456 Figure 858 shows ahead of the UDRDR pattern is UDRDR too, and UDR rises
  at once, with no propagation latency.
- RM0481 Table 645 limits SPI4 to SPI6 of the STM32H5 to DSIZE of 16 bits and CRC of 17 bits.
  The model accepts the DSIZE values of SPI1 to SPI3 there, and the CRC is not modelled.
- SPI_I2SCFGR of the STM32H5 SPI1 to SPI3 is storage: the I2S mode is not modelled.
- STM32L1 SPI1 to SPI3, RM0038 section 28, run SPI mode with one transmit buffer, one receive
  buffer and frames of 8 or 16 bits by CR1.DFF. The frame time is DFF bits of the APB clock of
  the instance, PCLK2 for SPI1, PCLK1 for SPI2 and SPI3, divided by 2 << BR, whatever
  `RCC_APBxENR` holds, which gates only the register access, see RCC. A frame starts at the DR
  write, without the 2 APB clock delay of the RM0038 28.3.5 note.
- `SPI_I2SCFGR` and `SPI_I2SPR` are storage: the I2S mode is not modelled. On Cat.3 to Cat.6
  CR2.FRF 1 with SPE 1 logs one `model_warning`, raises `model_error` under `strict_peripherals`
  at the CR1 or CR2 write, and the SPI runs the Motorola format. On Cat.1 and Cat.2 FRF
  is storage, RM0038 28.5.2. TIFRFE, UDR and CHSIDE stay 0.
- Clearing SPE lets the frame in flight finish and land with BSY 1, then no frame starts,
  RM0038 28.3.8 for the receive-only modes, inferred for the others. Both buffers keep their
  word across SPE 0, and a word written with SPE 0 is sent once SPE and MSTR are 1, inferred.
- A target whose transmit buffer is empty at a frame start sends the last frame it received, 0
  after a reset, inferred.
- The CRC runs serially over the frame bits in wire order, RM0038 28.3.6: with LSBFIRST 1 it
  takes the DR value LSB first. The CRC frame shifts TXCRCR out as a data word, bit reversed with
  LSBFIRST 1, inferred. The CRC frame clears CRCNEXT at its end, inferred from ST's HAL, which
  never clears it. With DMA, the completed transmit transfer arms the CRC frame after the last
  data. In receive-only mode, controller or target, the completed receive transfer makes the
  frame in flight the CRC frame, inferred, or the next frame when none is in flight.
- TXCRCR and RXCRCR run on across a CRC phase: no data frame after it clears them, inferred,
  RM0038 28.3.6 clears them only on the CRCEN write.
- While OVR is set, every frame received is lost, also after the DR read of the clear sequence
  and up to its SR read, RM0038 28.3.10.
- RXCRCR takes every frame received, also those lost while OVR is set, inferred from RM0038
  28.3.6.
- A target, MSTR 0 with CRCEN 1, folds into RXCRCR every frame its controller clocks, also with
  NSS high or SPE 0, RM0038 28.3.6. Such a frame lands nowhere else and leaves TXCRCR as it is,
  inferred: RM0038 names no calculator for a target that drives no MISO.
- A target keeps BSY 1 from one frame to the next of a continuous transfer. RM0038 28.3.7 gives
  a slave a BSY low of one SPI clock between frames.
- SPI_DR takes byte accesses as well as the half-word and word accesses RM0038 28.5 requires: a
  byte write merges into the DR value and sends it, a byte read returns its byte.
- The CR1 write that ends the MODF clear sequence leaves SPE and MSTR clear, inferred: RM0038
  28.3.10 restores them after the sequence.
- The SPI IRQ and the DMA requests follow their flags and enables with SPE 0 too, inferred:
  RM0038 Table 159 and 28.3.9 name no SPE term.
- The STM32L1 SPI takes a controller's frames through `spi_controller_port::transfer()` only.
  `select()`, `frame()`, `deselect()` and `drop()` throw `config_error`, so an STM32L1 SPI is a
  link end of no linked system.
- An STM32L1 SPI device replies at the frame's end. The model calls
  `spi_target::exchange_deferred()` with the frame's generation as ticket, offers no
  `spi_target_port` and calls neither `attached()` nor `detached()`. A device that leaves its
  reply pending drives all ones in that frame, and the frame's end fails the run with
  `config_error`.

STM32L4 SPI1 to SPI3, RM0351 section 42, follow the STM32L1 rules above with a 4-byte TXFIFO and
RXFIFO and frames of CR2.DS + 1 bits:

- A DR write pushes one byte at 8 bits and two bytes at 16 or 32 bits, a DR read pops as many,
  the 32-bit access as the 16-bit one, inferred. Byte accesses to DR + 1 to DR + 3 move no FIFO
  byte, inferred.
- A DR write whose bytes do not all fit the TXFIFO is dropped whole and the stored bytes stay,
  with one `model_warning`, and raises `model_error` under `strict_peripherals`, inferred. RM0351
  42.4.9 clears TXE above half full to prevent "possible corruption of 3x 8-bit data frames
  already stored in the TXFIFO when software tries to write more data in 16-bit mode": silicon may
  corrupt the stored frames, popcorn keeps them.
- With DS above 8 a frame takes two bytes, so a byte write waits for the next byte before the
  frame starts, inferred. BSY stays 0 while the TXFIFO holds half a frame, RM0351 42.4.10.
- A DR read of an empty RXFIFO slot returns 0 and pops nothing, inferred.
- FRLVL reads 11 with 3 bytes in the RXFIFO, as with 4, inferred.
- The TXFIFO keeps its bytes across SPE 0 and sends them once SPE and MSTR are 1, inferred.
- The CRC phase of DS 8 frames with CRCL 1 sends TXCRCR[15:8] first with LSBFIRST 0 and
  TXCRCR[7:0] first with LSBFIRST 1, inferred. DS 16 frames with CRCL 0 carry the CRC-8 in one
  16-bit frame and CRCERR compares its low 8 bits, inferred.
- CRCEN with DS other than 8 or 16 runs the CRC over the DS bits, with one `model_warning` and
  `model_error` under `strict_peripherals` at the frame start.
- The last 16-bit DR read of an LDMA_RX transfer returns its one byte in [7:0] and 0 above,
  inferred.
- The end of a DMA transfer arms LDMA_TX or LDMA_RX only with DS 8 or less. The next DR access of
  that side takes the armed flag whatever its width, and SPE written to 0 clears it, inferred:
  RM0351 42.4.9 and 42.6.4 give LDMA meaning only for packed transfers and state no clear.
- The NSS pulse of NSSP 1 lasts exactly one SCK period, inferred: RM0351 42.4.12 gives at least
  one.
- ES0250 2.23.1, BSY staying high once the SPI is disabled, ES0250 2.23.2, BSY staying high at
  the end of a target transfer, and ES0250 2.23.3, CRC corruption when another peripheral's DMA
  transfer completes, are not modelled: BSY follows RM0351 42.4.10 and the CRC RM0351 42.4.14.

## SBS

Source: [sbs.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/sbs.cpp), [secure_config.hpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/secure_config.hpp)

- The SBS model holds the temporal isolation level and the bus rules of RM0481 14.3.4 and 14.5,
  with TrustZone on and off. Under TrustZone `SBS_SECCFGR` and `SBS_RSSCMDR` are Secure,
  `SBS_EPOCHSELCR`, `SBS_ECCNMIR` and `SBS_CSLCKR` Secure and privileged, `SBS_CNSLCKR`
  privileged, `SBS_FPUIMR` privileged and Secure with FPUSEC, `SBS_MESR`, `SBS_CCCSR`,
  `SBS_CCVALR` and `SBS_CCSWCR` Secure with SBSSEC, `SBS_CFGR2` Secure with CLASSBSEC. A
  Non-secure access to a Secure register is RAZ/WI and sets SBSF in `GTZC1_TZIC_SR4`, on a read
  and on a write. An unprivileged access to a privileged register is RAZ/WI and sets no flag.
- SBSSEC makes `RCC_APB3ENR.SBSEN` Secure, so a Non-secure write of it is ignored while SBSSEC
  is set.
- FPUSEC takes privileged writes and reads for every Secure transaction, after the register
  description of RM0481 14.5.8. RM0481 14.3.4 lists FPUSEC among the privileged registers,
  whose unprivileged reads are RAZ.
- Without TrustZone `SBS_SECCFGR`, `SBS_RSSCMDR` and `SBS_CSLCKR` are RAZ/WI, RM0481 14.5.6
  gives `SBS_RSSCMDR` "always secure (RAZ/WI if nonsecure)". `SBS_EPOCHSELCR`, `SBS_ECCNMIR`,
  `SBS_FPUIMR` and `SBS_CNSLCKR` take privileged accesses only.
- `SBS_DBGCR` and `SBS_DBGLOCKR` take any access at HDPL0 and HDPL1 and are RAZ/WI above it,
  RM0481 14.5.4 and 14.5.5.
- The SBS has no RCC reset bit, RM0481 11.8.25. `RCC_APB3RSTR` bit 1, which the STM32H562, H563
  and H573 SVDs name SBSRST, is reserved and reads 0, and a write of it leaves every SBS
  register as it was, as read back on an STM32H573.
- A power-on or brown-out reset restores every SBS register. Every other machine reset restores
  all but `SBS_RSSCMDR` and MCLR of `SBS_MESR`, RM0481 14.5.6 and 14.5.12.
- `SBS_MESR` reads MCLR 1 from the power-on: the memory erase after a power-on is done at once.
  MCLR and IPMEE clear on a written 1. IPMEE stays 0, the erase after a tamper or a product
  state regression is outside the model. The power-on value and the MCLR clear are read on an
  STM32H573.
- Every SBS register but `SBS_HDPLCR`, `SBS_HDPLSR`, `SBS_SECCFGR` and `SBS_MESR` is storage:
  the functions of the debug, RSS, EPOCH, ECC NMI, FPU interrupt, compensation cell, Class B and
  lock registers are not modelled. Coverage reports a firmware access to them as `storage_only`.
  A firmware write that lands on one logs one `model_warning` per register. Under `strict` it
  raises `ModelError` before the store. Firmware reads are silent.
- `SBS_PMCR` takes every access. RM0481 14.5.9 makes its PBx_FMP bits Secure with their GPIOB
  pin, ANASWVDD and IO_ANA_BOOST_EN with ADC12SEC and ETH_SEL_PHY with ETHSEC.
- HDPL starts at HDPL1 after every reset, the level of a user flash boot. The RSS boot at HDPL0
  is outside the model.

## SYSCFG

Source: [syscfg.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/syscfg.cpp), [syscfg_classic.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/syscfg_classic.cpp), [secure_config.hpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/secure_config.hpp)

- Every register but `SYSCFG_SECCFGR` is storage. The functions it selects are not modelled. A
  firmware write lands and logs one `model_warning` per register. Under `strict` it raises
  `ModelError` before the store. A write while SYSCFGRST holds the unit raises the illegal
  access event the TrustZone rules give and is not noted.
- `SYSCFG_CSLCKR` stays storage at TZEN 0. RM0456 Table 131 makes it RAZ/WI.
- STM32L1: `SYSCFG_MEMRMP` is storage. The machine boots from main flash, BOOT_MODE and
  MEM_MODE read 00, and a MEM_MODE write maps no other memory at 0x00000000. A firmware write
  logs one `model_warning`. Under `strict` it raises `ModelError` before the store.
- STM32L4: `SYSCFG_EXTICR1` to `EXTICR4` select PA to PG on every line and PH on lines 0 and 1,
  code 7, RM0351 9.2.3 to 9.2.6. `SYSCFG_MEMRMP` with FB_MODE, `CFGR1` with BOOSTEN and the
  I2C fast-mode plus bits, `CFGR2` with its parity flag and locks, and `SWPR` are storage: a
  firmware write logs one `model_warning` per register, and under `strict` raises `ModelError`
  before the store. SRAM2 takes every write whatever `SWPR` holds.
- STM32L4: the SRAM2 erase, RM0351 9.2.7 and 9.2.10. `SYSCFG_SKR` written 0xCA then 0x53
  unlocks `SCSR.SRAM2ER`, another value locks it, SKR reads 0. A write of SRAM2ER 1 while
  unlocked sets SRAM2ER and SRAM2BSY at once and locks SRAM2ER again, inferred. The erase takes
  8192 HCLK periods at the HCLK of the write, one word per cycle, inferred, then SRAM2 reads 0 and
  both bits clear. A write to SCSR during the erase changes nothing. A CPU or DMA write to SRAM2
  during the erase lands and is zeroed at its end, inferred.
- STM32L4: ES0250 2.2.19, an SRAM2 read during the hardware erase: the read returns the current
  content, with no stall.
- STM32L4: a SYSCFGRST pulse resets the EXTI selection, `CFGR1` and the key, and keeps
  `SYSCFG_MEMRMP`, `SCSR`, `CFGR2` and `SWPR` with a running erase, inferred from their "System
  reset value" in RM0351 9.2.
- STM32L4: SRAM2 keeps its content across a system reset, whatever the `SRAM2_RST` option bit
  holds, RM0351 2.4.4.
- STM32L4: ES0250 2.2.7, the analog switch booster current injection, is electrical: `CFGR1`
  BOOSTEN stays storage. ES0250 2.2.12, the SRAM write error at a reset, has no register effect.
- STM32L1: `SYSCFG_PMC.USB_PU` drives the USB D+ pull-up. `LCD_CAPA` is storage: a firmware
  write that sets a bit of it logs one `model_warning`, and under `strict` raises `ModelError`
  before the store.

## TIM

Source: [tim.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/tim.cpp)

Not modelled, logged:

- The encoder index, `ECR.IE`.
- Internal input sources of `TIxSEL` and internal ETR sources of `ETRSEL`. They read low.
- Master modes above 011 and TRGO2. Under `strict` the CR2 write that selects one raises
  `ModelError` before the store.
- A slave mode trigger input that no pin and no routed timer drives. No trigger reaches the slave
  mode controller. Under `strict` the SMCR or OR write that selects it raises `ModelError` before
  the store.
- The trigger DMA request, TDE, and COM and trigger burst requests. Under `strict` the DIER write
  that sets TDE raises `ModelError` before the store.
- The slave mode preload, `SMCR.SMSPE` and `SMSPS`. Both are storage and a counter start with
  SMSPE set logs once. Dithering, `CR1.DITHEN`, logs the same way.
- A capture on TRC, `CCxS` 11. CCxIF never sets. Under `strict` the CCMR1 or CCMR2 write that
  selects it raises `ModelError` before the store.
- The reserved `CC1S` 10 and 11 of a one-channel timer, RM0038 18.5.6 and RM0481 TIMx_CCMR1:
  TIM10 and TIM11 of the L1, TIM13, TIM14, TIM16 and TIM17. CC1IF never sets. Under `strict` the
  CCMR1 write that selects one raises `ModelError` before the store.
- The CCx DMA requests at the update event, `CR2.CCDS` 1. They follow the CCx events. Under
  `strict` the CR2 write that sets CCDS raises `ModelError` before the store.
- Gated mode on TI1F_ED, TS 00100. The edge detector gives pulses and the counter holds. Under
  `strict` the SMCR write that selects it raises `ModelError` before the store.
- OC1 as an ITR source in OC1M values other than 0000, 0100, 0101, 0110 and 0111, or with
  OC1CE set. OC1 holds low. Under `strict` the CCMR1 write that selects one while a slave
  listens, and the slave's SMCR or OR write that selects the OC1 output then, raise `ModelError`
  before the store.
- Offset 0x08 of a timer whose description has no SMCR, TIM6 and TIM7 among them. A read gives
  0 and logs nothing, inferred: RM0351 Table 215 marks it reserved and gives no read value.
  `HAL_TIMEx_MasterConfigSynchronization` reads it on every timer. A write is dropped with one
  `model_warning`, and under `strict` it raises `ModelError` before the store.

A debugger write raises none of the `strict` errors of this model.

A slave mode reset, trigger start or gate opening at a timer edge counts from the edge's exact
instant, which can lie between two ps, on any pair of master and slave clocks.

While UIFREMAP is 1 a CNT read gives UIF in `CNT.UIFCPY`, and in bit 31 of a 32-bit count with
no UIFCPY field, the STM32U5 TIM2 to TIM5 and the STM32H5 TIM2 and TIM5, RM0456 55.4.20 and
RM0481 44.4.20. A byte or halfword write of the low half keeps the count's bit 31.

On every timer with `BDTR.MOE`, `EGR.BG` and `B2G` clear MOE, and with `BDTR.AOE` 1 each update
event sets it again, RM0351 30.4.6 and 30.4.20, RM0456 and RM0481 TIMx_EGR and TIMx_BDTR. The
break inputs have no model, so no break input holds MOE at 0. MOE drives no pin. A BDTR read or
write first brings the counter up to the access, so an update that sets MOE shows in a BDTR read
alone and a MOE 0 write holds until the next update. The other BDTR fields read back as written,
with no warning under strict_peripherals, and the coverage report counts BDTR as served.

Differs from RM0456:

- An ARR written below the count lets the counter run to its maximum and wrap with no update.
  A count written above ARR in center-aligned mode does the same.

Capture/compare event outputs, the ADC triggers:

- Each channel x drives `<instance>_ccx`, a pulse one timer clock wide at each CCxIF set by a
  match, a capture or `EGR.CCxG`, at the event's exact instant, the width and the capture and
  CCxG events inferred. RM0038 Tables 61 and 62 name it TIMx_CCx.
- The STM32U5 and STM32H5 ADC tables select `tim_ocx`, the channel output in the RM0456 TIM
  chapter. The ADC takes the CC event for it, as ST's LL driver describes the selection,
  inferred.

OC1 as an ITR source, TIM13, TIM14, TIM16 and TIM17 OC1 by RM0481, TIM10 and TIM11 OC1 by RM0038:

- The ITR carries OC1, OC1REF with CC1P inverting it, RM0038 18.5.6 and RM0481 TIMx_CCMR1.
  The ITR follows OC1 whatever CC1E, and on TIM16 and TIM17 whatever BDTR.MOE and OSSI,
  inferred. With CC1S other than 00 the ITR reads low, inferred.
- OC1REF holds low in frozen mode, OC1M 0000, where silicon keeps the level it had.
- RM0038 takes the comparison of CNT with CCR1 at a PWM mode entry only from frozen, RM0481 from
  frozen and from the forced modes. The model takes it from every mode, so it differs at a PWM 1
  to PWM 2 switch, and on the L1 at a forced to PWM switch.
- Every OC1 edge reaches the slave whatever the pulse width. RM0481 asks for at least two clock
  cycles of the destination timer.
- A timer that drives OC1 counts up only. Its CR1 with DIR or CMS throws `ConfigError` at
  construction.

STM32L1 TIM2 to TIM7 and TIM9 to TIM11:

- TIM5 has no row in RM0038 Table 80, so every TS value of TIM5 reaches nothing and logs once,
  inferred.
- The touch sensing I/O that TIM9_OR.ITR1_RMP gives TIM9 ITR1 has no model. The ITR reaches
  nothing and logs once.
- TIM10_OR and TIM11_OR TI1_RMP values other than 00, and 11 on TIM11, connect LSI, LSE, the RTC
  wakeup interrupt, MSI or HSE_RTC to tim_ti1, and TI1_RMP_RI 1 the routing interface. tim_ti1
  then reads low and the OR write logs once. TIM10 and TIM11 external clock mode 2 counts nothing
  whatever ETR_RMP, LSE or TIM9 TRGO, and the SMCR write logs once. Under `strict` each write
  raises `ModelError` before the store.
- TIM9_OR.TI1_RMP 01 connects the LSE to tim_ti1, which then reads low. TIM9 external clock mode 2
  takes the internal ETR and counts nothing, inferred: RM0038 18.3.3 names no ETR source. Each
  logs once. Under `strict` the write that selects it raises `ModelError` before the store.
- TIM6, TIM7, TIM10, TIM11 and the Cat.1 and Cat.2 TIM9 count up only. On that TIM9 SMS 001 to 011 are
  reserved: no encoder edge counts and the SMCR write logs once. Under `strict` it raises
  `ModelError` before the store.
- TIM2_ETR is the TIM2_CH1 pad at AF1, PA0, PA5 or PA15, which also carries TI1. TIM3_ETR is PD2.
  TIM4_ETR and TIM5_ETR are on PE0 and PE9, pads the shipped 64-pin packages lack, so their
  tim_etr_in stays low.

Every STM32L4 timer, RM0351 sections 30 to 33, follows RM0351 where ES0250 2.17.1, 2.17.2 and
2.17.3 give the silicon behaviour. SMCR.MSM is storage, so a trigger at CNT = ARR starts the
one-pulse in combined reset + trigger mode. Every compare match sets CCxIF, also the second of
two matches on consecutive counts after a CCR change. The OCREF clear input has no model.

STM32L4 TIM2 to TIM5, RM0351 section 31:

- OTG_FS_SOF, the RM0351 Table 207 source of TIM2 ITR1 at TIM2_OR1.ITR1_RMP 1, has no model.
  TS 001 on TIM2 then reaches nothing and logs once. Under `strict` the SMCR or TIM2_OR1 write
  that selects it raises `ModelError` before the store.
- TIM2_OR1.ITR1_RMP is a field on every line. RM0351 31.4.22 marks the bit reserved on the
  STM32L471, which has no OTG_FS. Its value 1 reaches nothing there as on the other lines.
- The comparator outputs that TIM2_OR1.TI4_RMP, TIM3_OR1.TI1_RMP and the TIM2 and TIM3
  `OR2.ETRSEL` select, and the LSE of TIM2_OR1.ETR_RMP 1, have no model: the input reads low and
  the OR write logs once. Reserved ETRSEL values read low the same way. Under `strict` each
  write raises `ModelError` before the store.
- TIM2_ETR is PA0 at AF14, PA5 or PA15 at AF2, TIM3_ETR is PD2 or PE2 at AF2, DS10198 Tables 17
  and 18. PE2, TIM4_ETR on PE0 and TIM5_ETR on PF6 are pads the shipped 64-pin package lacks, so
  TIM4 and TIM5 tim_etr_in stays low.
- TIM3 and TIM4 `ARR` bits 31:16 read 1, their reset value in the RM0351 31.4.26 register map,
  and keep it after a write, inferred. RM0351 states no write rule for the reserved half.
- UIFREMAP puts UIFCPY in bit 31 of TIM3 and TIM4. On the 32-bit TIM2 and TIM5, while UIFREMAP
  is 1, a read gives UIF in bit 31 in place of the count's bit 31, RM0351 31.3.16. A write
  reaches the count's bit 31 only through the byte that holds it, so a byte or halfword write of
  the low half keeps it.
- SMCR.OCCS is storage: the OCREF clear input is not modelled.

STM32L4 TIM1 and TIM8, RM0351 section 30:

- TIM1 and TIM8 run the counter, the repetition counter, channels 1 to 4, the slave mode
  controller and TRGO as TIM2 to TIM5 do. Their ITR0 to ITR3 follow RM0351 Table 202.
- No timer drives an output pin. BDTR, the complementary outputs CCxNE and CCxNP, the dead time,
  CCR5, CCR6, CCMR3 and the OR2 and OR3 break sources are registers only. EGR.BG and B2G set BIF
  and B2IF, raise the break interrupt and clear BDTR.MOE, which AOE 1 sets again at the next
  update event.
- TIM1_ETR is PA12 or PE7 at AF1 and TIM8_ETR is PA0 at AF3, DS10198 Tables 17 and 18. PE7 is a
  pad the shipped 64-pin package lacks. ETRSEL 001 and
  010, COMP1 and COMP2, read low. ETR_ADC1_RMP, ETR_ADC2_RMP and ETR_ADC3_RMP 01 to 11 OR an
  ADC analog watchdog into the ETR: the watchdog has no model, tim_etr_in follows the pin. Each
  such write logs once, and under `strict` raises `ModelError` before the store.
- TIM1_OR1.TI1_RMP 1 and TIM8_OR1.TI1_RMP 1, COMP1 and COMP2 into tim_ti1, read low, log once and
  raise under `strict`.
- TIM1 raises the NVIC lines it shares with TIM15, TIM16 and TIM17, RM0351 Table 61.

STM32L4 TIM15, TIM16 and TIM17, RM0351 section 32:

- TIM15 counts up with channels 1 and 2, the slave mode controller and TRGO from MMS 000 to 011.
  Its ITR0 to ITR3 follow RM0351 Table 210: TIM1, TIM3 and the OC1 outputs of TIM16 and TIM17.
  TIM16 and TIM17 count up with one channel, no slave mode and no master mode.
- TIM15, TIM16 and TIM17 raise NVIC lines 24, 25 and 26, the lines they share with TIM1, RM0351
  Table 61.
- No timer drives an output pin. BDTR, the complementary outputs, the dead time and the OR2
  break sources are registers only, as on TIM1. BDTR has no BKF field, RM0351 32.6.16 and
  32.7.14.
- TIM15_OR1.TI1_RMP 1, the LSE, and ENCODER_MODE 01 to 11, the TIM2, TIM3 or TIM4 inputs, read
  low on tim_ti1 and tim_ti2. TIM16_OR1.TI1_RMP 01 to 11, the LSI, LSE and RTC wakeup, and
  TIM17_OR1.TI1_RMP 01 to 11, the MSI, HSE/32 and MCO, read low on tim_ti1. Each such write logs
  once, and under `strict` raises `ModelError` before the store. TIM16_OR1.TI1_RMP has two bits,
  RM0351 32.4: the extended remap is absent from the STM32L47x and STM32L48x.

STM32L4 TIM6 and TIM7, RM0351 section 33:

- TIM6 and TIM7 count up only and have no slave mode controller. UIFREMAP puts UIFCPY in CNT
  bit 31, RM0351 33.4.6.
- CR2.MMS 000 to 010 drives `tim6_trgo` and `tim7_trgo`, RM0351 33.4.2, the DAC1 and ADC trigger
  sources. No timer takes them as an ITR, RM0351 Table 207.

## USART, UART and LPUART

Source: [usart.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/usart.cpp)

- Only plain asynchronous mode is modelled. Synchronous, smartcard, IrDA, LIN, half-duplex,
  driver enable, auto baud and receiver timeout are logged as not modelled.
- The model runs in Run mode only. A CPU or DMA write that sets CR1.UESM, CR3.UCESM, CR3.WUFIE
  or CR3.WUS, where the description has them, logs one `model_warning` and raises `ModelError`
  under `strict_peripherals` before the store, on every family. WUF stays 0.
- GTPR and AUTOCR are not served, so the locks on their fields are not enforced.
- IDLE is never gated on RXFNE, which RM0456 66.8.10 does.
- BUSY does not follow frames from other machines.
- An aborted frame delivers nothing and holds the line until the abort. The chip would see a
  truncated frame and FE.
- A frame from another machine still being sampled when a link holds the line low becomes the
  break character, data 0. Silicon keeps the bits before the hold.
- STM32L1 USART and UART, RM0038 section 27, run the same frame engine with one data word each
  way. USART_SR resets to 0x000000C0, TXE and TC set, RM0038 Table 158.
- The STM32L1 kernel clock is the APB clock of the instance, PCLK2 for USART1, PCLK1 for USART2,
  USART3, UART4 and UART5, whatever `RCC_APBxENR` holds, which gates only the register access,
  see RCC.
- The first frame after TE is set starts at its DR write, without the idle preamble and the
  1 bit-time delay of the RM0038 27.6.4 TE note. Clearing UE lets the frame in flight end, then
  stops the output, RM0038 27.6.4, and drops the word waiting in TDR.
- A write of 0 to RXNE drops the received word, and DR reads 0 while RXNE is 0, inferred.
- A write of 0 to SBK before the break starts cancels it, RM0038 27.3.2. Once the break's low
  part runs, SBK reads 1 until it ends, a write of 0 meanwhile keeps it, inferred. The line stays
  high 1 bit after the break before the next frame, RM0038 27.3.2.
- SBK written with TE 0 and UE 1 sends the break once TE is 1. A write with UE 0 clears SBK,
  inferred.
- Mute mode, RWU, is logged as not modelled. LBD never sets.
- UART4 and UART5 have no hardware flow control, RM0038 Table 157: CTSE and RTSE set there log
  one `model_warning` and transmission and reception ignore them.
- The STM32L1 SR-then-DR clear: a CPU or DMA read of USART_SR notes the IDLE, ORE, NF, FE, PE
  and TC flags it reads set, any later DR read clears the noted IDLE, ORE, NF, FE and PE, and
  any later DR write the noted TC and PE, whatever other accesses lie between, inferred. A flag
  set after the SR read stays set, and a DR write with no SR read since TC rose sends the word
  and leaves TC set, RM0038 27.6.1.
- STM32L1 EIE raises the interrupt on ORE, NF or FE only while DMAR is 1, RM0038 27.6.6.
- STM32L4 USART1 to USART3, UART4, UART5 and LPUART1, RM0351 sections 40 and 41, run the sci3 frame
  engine with one data word each way: ISR.TXE, RXNE and NF, CR1.TXEIE and RXNEIE, ICR.NCF clears NF.
  The kernel clock is the instance's `RCC_CCIPR` selection, BRR alone divides it, the L4 has no
  PRESC. Every instance has CTS and RTS flow control, RM0351 Table 251. USART_ISR reads 0x000000C0
  after reset, TXE and TC, bit 25 0, inferred: RM0351 40.8.8 gives 0x020000C0 with TCBGT, which the
  Table 251 note gives to the L49x and L4Ax only.
- TC sets at the end of a data frame when TXE is set, a break following or not, on every family,
  RM0456 66.8.10 and 67.7.8, RM0481 56.8 and 57.7, RM0351 40.8.8, RM0038 27.6.1. A held data frame
  keeps TC 0.
- STM32L4 ES0250 2.21.2 and 2.22.1 and STM32L1 ES0125 2.11.3, ES0242 2.11.3, ES0224 2.10.3,
  ES0240 and ES0241 2.12.3 and ES0578 2.11.3 are modelled: a break requested while a data frame
  shifts with CTSE 1 and nCTS deasserted at its end keeps TC 0 until the break has run, inferred.
- STM32L1 ES0125 2.11.8, ES0242 2.11.8, ES0224 2.10.8, ES0240 and ES0241 2.12.8 and ES0578 2.11.8
  are modelled: with CTSE 1 a break leaves at the end of the current transmission whatever nCTS
  says. A data frame waits for nCTS.
- STM32L4 ES0250 2.21.3 and 2.22.2: RTS is driven low once RTSE is set, also while UE or RE is 0.
  The model reads ready to the host while UE is 0, and while RE is 0 with room in RDR. With RE 0
  and RDR full it reads not ready, inferred: the erratum names no RDR state.
- STM32L4 LPUART1, RM0351 section 41: 256 times the kernel clock over BRR[19:0] gives the baud,
  and a BRR below 0x300 sends and receives nothing, RM0351 41.4.4. The kernel clock is the
  `RCC_CCIPR` LPUART1SEL selection. RQR has no TXFRQ, a write of bit 4 does nothing.
- STM32L4 ES0250 2.22.3, LPUART transmitter jitter at a low BRR, and 2.22.4, LPUART1 outputs not
  open-drain, are pin-level effects the frame engine leaves out.
- STM32L4 ES0250 2.21.1, NACK sampling in smartcard mode, follows smartcard: logged as not
  modelled. ES0250 2.21.4, the receiver timeout start with two stop bits, follows RTOEN: logged
  as not modelled. ES0250 2.21.5, data corruption on a noisy receive line, and 2.21.6, data
  corrupted when ABREN is cleared, are analog and auto-baud effects the frame engine leaves out.
  ES0250 2.21.7, NF with ONEBIT: the model samples once with ONEBIT and sets NF only for a host
  frame tagged with noise.
