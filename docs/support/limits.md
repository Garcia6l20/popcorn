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

- Nothing is mapped at address 0, as on the STM32U5. Under the default `faults="stop"`, a load,
  store or fetch there stops the run with a bus fault.
- The ICACHE remap of external memories onto the code region is not modelled. CRR0 to CRR3 store
  what the firmware writes and map nothing.
- Parts that alias boot memory at address 0, such as the STM32F4 and STM32L4, are not modelled.

## Core faults

Source: [exceptions.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/arch/armv8m/exceptions.cpp)

Under `faults="handler"`, a fault enters the firmware's handler as the core does, with these
exceptions. They stop the run with a FAULT stop instead:

- Lockup, and an INVSTATE fault.
- Without TrustZone: INVPC, UNSTKERR, STKERR, a STKOF on exception entry, and VECTTBL.
- With TrustZone: a STKOF of the additional state context alone, and a fault while taking a
  derived exception.

[Faults and errors](../guide/faults.md) shows both policies.

## Reset requests

- Only a lone Python `Machine` applies the reset a RESET_REQUEST stop reported at its next
  run, step or free run. A C++ driver of `machine::run_slice` or `host::run` resumes the old code
  after the stop. A member of a linked system follows `on_member_reset`.
- A reset a store requests applies after the instructions that follow the store up to the next
  branch, as for AIRCR.SYSRESETREQ.

## ADC

Source: [adc.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/adc.cpp)

Not modelled, logged once each:

- Auto-delayed conversions, `ADC_CFGR1.AUTDLY`.
- The low frequency trigger mode, `ADC_CFGR2.LFTRIG`.
- The MDF data path, `ADC_CFGR1.DMNGT` 10.
- DMA requests gated by the AWD1 window, `AWDFILT1` with `DMNGT`.
- Left shift, gain compensation and offset of the result.
- Bulb sampling, `ADC_CFGR2.BULB`.
- The sampling time control trigger mode, `ADC_CFGR2.SMPTRIG` with `SWTRIG`.
- Triggered regular oversampling, `ADC_CFGR2.TROVS`: all samples of a result convert back to
  back from the first trigger.
- Differential conversions, the `ADC_DIFSEL` bit of a converted channel: the channel converts
  single-ended.
- Reserved trigger selections in `EXTSEL`.
- A reserved prescaler in `ADC12_CCR.PRESC` divides by 256.
- ADC4 has no model.

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

## CRC

Source: [crc.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/crc.cpp)

- A `CRC_DR` write that is not right-aligned is ignored and logged, RM0456 24.4.
- An even polynomial, which RM0456 24.3.3 does not support, is computed anyway and logged.
- A write narrower than 32 bits to any register but `CRC_DR` is ignored and logged. Such a read
  is logged.
- The RCC clock gate is not modelled. The CRC works with its RCC enable bit clear.

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
  The RCC clock enable of the CRS gates nothing.

## DBGMCU

Source: [dbgmcu.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/dbgmcu.cpp)

- A freeze bit written while the core is held takes effect at the next hold or release. On
  silicon it takes effect at once.
- STM32L1: `DBGMCU_CR` is storage, as on the STM32U5. `DBGMCU_IDCODE` reads the description's
  value of the category, REV_ID 0x1000.

## EXTI

Source: [exti.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/exti.cpp), [exti_classic.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/exti_classic.cpp)

- STM32L1: `EXTI_EMR` is storage, and an EXTI event wakes no WFE.
- STM32L1: lines 16, 17, 18, 20 and 23 set their PR bit only, from `EXTI_SWIER`. The
  description gives `PVD_IRQn`, `RTC_Alarm_IRQn`, `USB_FS_WKUP_IRQn`, `RTC_WKUP_IRQn` and
  `COMP_ACQ_IRQn` to PWR, RTC, USB and COMP. Lines 16 to 23 take their edges from `EXTI_SWIER`
  only, the PVD, the RTC, the USB and the comparators drive none.
- STM32L1, inferred rules: an edge or a SWIER write sets PR only while the `EXTI_IMR` bit is set,
  the NVIC line follows PR whatever IMR holds, a SWIER write of 0 clears the SWIER bit, a write
  of 1 to a PR bit clears the SWIER bit of the line whether PR was set or not, a
  `SYSCFG_EXTICRn` write that moves a line to a pin of another level is an edge on the line, and
  a selector code RM0038 8.5.3 to 8.5.6 does not list selects no pin.

## GPIO

Source: [gpio.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/gpio.cpp)

- STM32L1: `GPIOx_BRR` exists on Cat.4, Cat.5 and Cat.6, RM0038 7.4.11. On Cat.1 to Cat.3 a
  firmware store to offset 0x28 is an unmodelled access and changes no pin.
- STM32L1: `GPIOx_OSPEEDR` is storage, as on the STM32U5 and the STM32H5.

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
  typical of the STM32L15x datasheets, and the memory reads its new content from the last store of
  the operation on. During a word write or a page erase, code and data reads of the bank complete
  and the core keeps running from it. RM0038 3.4.3 stalls code run from the bank for one tprog.
- From the first store of a half page sequence to the end of its tprog, a CPU or DMA load of the
  bank written, its program memory and its half of the data EEPROM, is a bus error and a fetch from
  it a BusFault, RM0038 3.4.3 note 2 and Table 15 note 3. A data EEPROM double word erase or write
  locks its bank the same way. A parallel page erase or parallel half page locks both banks the same
  way, from the first store, inferred from RM0038 3.4.2 and 3.4.3, which run them from SRAM only.
  Host and debugger reads complete. A vector read from a locked bank is a bus error too, so an
  exception entry with VTOR in that bank escalates to HardFault with HFSR.VECTTBL, inferred from
  RM0038 Table 15 note 3. Firmware runs these operations from SRAM with VTOR in SRAM or in the other
  bank.
- A store while BSY is set is decided against FLASH_PECR at its own time and writes at once. An
  operation it completes adds its tprogs to BSY. EOP sets once at the end of each operation.
  Silicon stalls the store until the operation before it ends. While a half page or a data
  EEPROM double word of one bank is BSY, a program memory or data EEPROM store to the other bank
  sets WRPERR and writes nothing, RM0038 3.4.4 and 3.7.3.
- A program over a word that is not erased writes the word as given. A page erase writes 0, and a
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
  value a PCROP read returns and the absence of a fault are inferred. On every other category and
  part FLASH_WRPRx bits at 1 are write protection.
- `Machine(option_bytes=)` selects the boot bank: the flash base, or with `swap_bank` on Cat.4 to
  Cat.6 the bank 2 base, whatever the first word of bank 2 holds. RM0038 Table 19 boots the
  system bootloader, which jumps to bank 2 when its first word is an SRAM address. FLASH_OBR.nBFB2
  then loads as the inverse of `swap_bank` at every option load, whatever the USER word holds. Any
  other field away from its default is refused as `option_bytes.<field>`.
- OPTVERR and OPTVERRUSR set again at every reset and FLITFRST pulse after an option load that
  set them, RM0038 3.9.7. An option store programs whatever OPTVERR and OPTVERRUSR hold.
- A program in voltage range 3 completes, logs one `model_warning` and raises `ModelError` under
  `strict`. RM0038 3.6 allows programming in ranges 1 and 2 only.
- FLASH_ACR.RUN_PD powers nothing down: the flash stays readable and fetchable, and
  FLASH_SR.READY reads the inverse of RUN_PD. SLEEP_PD and PRFTEN are storage.
- An RCC FLITFRST pulse resets the FLASH registers only while FLASH_ACR.RUN_PD is set, RM0038
  6.3.5, and keeps FLASH_OBR and FLASH_WRPRx at their loaded values. While FLITFRST is held,
  FLASH register writes are ignored whatever RUN_PD holds.
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
- A transfer error leaves the count and the current addresses at the failed item, inferred.
- A reserved `PSIZE` or `MSIZE` of 3 transfers words, and `CIRC` with `MEM2MEM` runs the copy
  once. Each logs one `model_warning`.
- A byte or halfword DMA write to an APB1 or APB2 address is a word write at the address
  aligned down to 4, with the item repeated on every lane, RM0038 11.3.4. A CPU byte or
  halfword store to the same address writes its own lanes only, inferred.
- The requests of a channel are ORed, RM0038 11.3.7. Each item acknowledges every request of
  the channel that is high, and the end of a non-circular transfer tells every request of the
  channel, inferred.
- A peripheral-requested transfer that leaves one item tells every request of the channel, once
  per pass of a circular channel. This is the EOT_1 signal the I2C reads with CR2.LAST, RM0038
  26.3.7. A memory-to-memory copy tells none.
- The channels run whatever `RCC_AHBENR.DMAxEN` holds.

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
- The H5 HASH keeps SHA-3 and SHAKE on the STM32H543 and H553 only, RM0481 Table 397. On every
  other H5 part, ALGO keeps 0000, 0010, 0011 and 1100 to 1111. A CR write of another code, with
  or without INIT, stores ALGO 0000 and the hash runs SHA-1, as read back on an STM32H573 for
  0001 and 0100 to 1011. The H543 and H553 store 0000 for 0001, 1010 and 1011 by the same rule,
  which is the H573 reading applied there. The write logs once and raises a model_error under
  strict_peripherals before CR changes.
- The SHA-3 and SHAKE rules follow RM0481 and are served on the STM32H543 and H553 rows. Building
  a description for those parts needs ST's DIE47C rzone file.
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
- The RCC clock gate is not modelled.

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
  acknowledged address or byte, none after a NACKed byte. PE 0 written between a match of an address
  of the I2C and the port's STOP disables it at that STOP, RM0038 26.6.1: until then it answers,
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
  nothing, inferred.
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
- The hardware watchdog option is not modelled.

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

## RCC

Source: [rcc.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/rcc.cpp)

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
  on reset and a BOR reset them, RM0038 6.1.2 and 6.3.14. `RTCRST` resets the RTC domain bits of
  `RCC_CSR` while it is 1. A software reset sets SFTRSTF only, the PINRSTF of the NRST pulse is not
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

## RNG

Source: [rng.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/rng.cpp)

The words come from a seed in the machine description or a queue the test fills, never from a
noise source. The model differs from RM0456 Rev 7 chapter 48 here:

- Register accesses are served with `RCC_AHB2ENR1.RNGEN` 0. Only generation stops.
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

## RTC and TAMP

Source: [rtc.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/rtc.cpp)

Not modelled, logged:

- Binary and mixed modes. The calendar counts in BCD.
- Smooth calibration and the shift register.
- Timestamps, the RTC outputs, ALRxFCLR and SSRUIE.
- WUTOCLR. WUTF stays set until SCR clears it.
- The backup domain write protection. `PWR_DBPR.DBP` does not gate RTC writes.
- Subsecond alarm comparison. MASKSS reads as 0.
- The security and privilege `RTC_SECCFGR`, `RTC_PRIVCR`, `TAMP_SECCFGR` and `TAMP_PRIVCR`
  select. They are storage.

## SAI

Source: [sai.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/sai.cpp)

- SPDIF, AC'97, PDM, companding, mute and synchronization with another SAI are not modelled.

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
  `RCC_APBxENR` holds. A frame starts at the DR write, without the 2 APB clock delay of the
  RM0038 28.3.5 note.
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
- While OVR is set, every frame received is lost, also after the DR read of the clear sequence
  and up to its SR read, RM0038 28.3.10.
- RXCRCR takes every frame received, also those lost while OVR is set, inferred from RM0038
  28.3.6.
- A target, MSTR 0 with CRCEN 1, folds into RXCRCR every frame its controller clocks, also with
  NSS high or SPE 0, RM0038 28.3.6. Such a frame lands nowhere else.
- A target keeps BSY 1 from one frame to the next of a continuous transfer. RM0038 28.3.7 gives
  a slave a BSY low of one SPI clock between frames.
- SPI_DR takes byte accesses as well as the half-word and word accesses RM0038 28.5 requires: a
  byte write merges into the DR value and sends it, a byte read returns its byte.
- The CR1 write that ends the MODF clear sequence leaves SPE and MSTR clear, inferred: RM0038
  28.3.10 restores them after the sequence.
- The SPI IRQ and the DMA requests follow their flags and enables with SPE 0 too, RM0038
  Table 159 and 28.3.9 name no SPE term.
- The STM32L1 SPI takes a controller's frames through `spi_controller_port::transfer()` only.
  `select()`, `frame()`, `deselect()` and `drop()` throw `config_error`, so an STM32L1 SPI is a
  link end of no linked system.
- An STM32L1 SPI device replies at the frame's end. The model calls
  `spi_target::exchange_deferred()` with the frame's generation as ticket, offers no
  `spi_target_port` and calls neither `attached()` nor `detached()`. A device that leaves its
  reply pending drives all ones in that frame, and the frame's end fails the run with
  `config_error`.

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

Source: [syscfg.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/syscfg.cpp), [secure_config.hpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/secure_config.hpp)

- Every register but `SYSCFG_SECCFGR` is storage. The functions it selects are not modelled. A
  firmware write lands and logs one `model_warning` per register. Under `strict` it raises
  `ModelError` before the store. A write while SYSCFGRST holds the unit raises the illegal
  access event the TrustZone rules give and is not noted.
- `SYSCFG_CSLCKR` stays storage at TZEN 0. RM0456 Table 131 makes it RAZ/WI.
- STM32L1: `SYSCFG_MEMRMP` is storage. The machine boots from main flash, BOOT_MODE and
  MEM_MODE read 00, and a MEM_MODE write maps no other memory at 0x00000000. A firmware write
  logs one `model_warning`. Under `strict` it raises `ModelError` before the store.
- STM32L1: `SYSCFG_PMC` is storage until the USB model reads `USB_PU`. A firmware write logs one
  `model_warning`. Under `strict` it raises `ModelError` before the store.

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

A debugger write raises none of the `strict` errors of this model.

A slave mode reset, trigger start or gate opening at a timer edge counts from the edge's exact
instant, which can lie between two ps, on any pair of master and slave clocks.

Differs from RM0456:

- An ARR written below the count lets the counter run to its maximum and wrap with no update.
  A count written above ARR in center-aligned mode does the same.
- UIFREMAP puts UIFCPY in bit 31 only where the description names a UIFCPY field, not on the
  32-bit TIM2 to TIM5.

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
  takes the internal ETR, RM0038 18.3.3, and counts nothing. Each logs once. Under `strict` the
  write that selects it raises `ModelError` before the store.
- TIM6, TIM7, TIM10, TIM11 and the Cat.1 and Cat.2 TIM9 count up only. On that TIM9 SMS 001 to 011 are
  reserved: no encoder edge counts and the SMCR write logs once. Under `strict` it raises
  `ModelError` before the store.
- TIM2_ETR is the TIM2_CH1 pad at AF1, PA0, PA5 or PA15, which also carries TI1. TIM3_ETR is PD2.
  TIM4_ETR and TIM5_ETR are on PE0 and PE9, pads the shipped 64-pin packages lack, so their
  tim_etr_in stays low.

## USART, UART and LPUART

Source: [usart.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/usart.cpp)

- Only plain asynchronous mode is modelled. Synchronous, smartcard, IrDA, LIN, half-duplex,
  driver enable, auto baud, receiver timeout and wake-up from Stop are logged as not modelled.
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
  USART3, UART4 and UART5, whatever `RCC_APBxENR` holds.
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
