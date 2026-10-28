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
descriptions build from their SVD, and attaching a Machine to them raises `ConfigError`.

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
  model_error under strict_peripherals, and converts the driven input as if the bit were set.
- ADC2 `ADC_OR.OP0`, the VDDCORE channel enable, is storage.
- In the synchronous clock modes the ADC clock is HCLK divided by CKMODE, whatever ADC12EN.
  CKMODE 01 with HCLK not SYSCLK is logged once at the ADEN edge, a model_error under
  strict_peripherals, and converts at HCLK.
- ADEN sets ADRDY at the write once ADVREGEN is set out of deep power-down. The tADCVREG_STUP
  wait of DS14121 Table 96 is the firmware's.

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

## FLASH

Source: [flash.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/flash.cpp)

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
and off, Table 72 note 1. A closed sector reads zero to a load, a fetch from it is a BusFault,
and a program or erase of it sets WRPERR, Table 64. FLASH_HDPEXTR extends the area at HDPL 3.
Without TrustZone the rule needs machine option bytes, which set the HDP areas.

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
- The H5 SHA-3 and SHAKE rules below come from RM0481 and run on the H543 and H553 rows. Their
  cases run on the STM32H573 description renamed STM32H553ZGTx: an H543 or H553 description
  build takes ST's DIE47C rzone file.
- The H5 BUSY time of one block, from its launching DIN write, is 82 HCLK cycles under SHA-1 and
  the SHA-512 family and 64 under SHA2-224 and SHA2-256, as measured on an STM32H573 at 32 MHz.
  RM0481 Table 401 gives 82, 98 and 66. The U5 keeps RM0456 Table 486: 82 under SHA-1 and 66
  under MD5, SHA2-224 and SHA2-256.
- An H5 final digest keeps BUSY 15 HCLK cycles past its blocks under SHA-1, SHA2-224 and SHA2-256
  and 27 under the SHA-512 family. DCIS, DINIS, NBWE and the digest registers follow 11 cycles
  after BUSY clears, and the digest registers read 0 in between. An STM32H573 measured DCAL to
  BUSY 0 in 97, 79 and 109 cycles and DCAL to DCIS in 108, 90 and 120 for SHA-1, SHA2-256 and
  SHA2-512 on a one-block message. SHA2-224 and SHA2-384 to SHA2-512/256 take the figures of the
  measured algorithm with their block size. SHA-3 and SHAKE digests take their block times, DCIS
  with BUSY 0. The U5 sets DCIS with BUSY 0 after one block time per padded block.
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

Source: [i2c.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/i2c.cpp)

- Target mode, 10-bit addressing, SMBus, PEC and the autonomous trigger are not modelled. The I2C
  runs as a plain 7-bit controller.
- PE set during a bus hold reads BUSY 1 at once. Silicon waits for the next START.
- A transfer that meets a hold waits. On silicon it would lose arbitration. A target's ARLO reply
  models that case.

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

- A CPU access of the packet memory reaches only its own byte lanes of the word. Word accesses
  are the ones RM0481 61.7 allows, so a byte or half-word access logs once and raises
  `ModelError` under `strict`. Debugger accesses of any width pass.
- The packet memory reads 0 after a power-on reset and keeps its content across every other
  reset, an RCC USBFSRST pulse included.
- ESOF, FNR.RXDP and FNR.RXDM read 0. A suspend raises SUSP after the host's 3 ms of idle bus.
- Transfers run whatever kernel clock `RCC_CCIPR4.USBFSSEL` selects.
- A double-buffered endpoint fills or sends descriptor word 2n while its DTOG is 0 and word
  2n + 1 while it is 1, in both directions, as RM0481 61.5.3 states in its text and the ST HAL
  and tinyusb drivers expect.
- The first transaction of a double-buffered bulk endpoint after `EPKIND` or `UTYPE` changes, or
  after a reset, sets its STAT to NAK. Every later transaction keeps STAT VALID.
- Writing `CNTR.USBRST` from 0 to 1 clears every `CHEPnR` but `VTRX` and `VTTX`, and `DADDR`.
  `ISTR.RST_DCON` and `FNR.LCK` stay as they are.
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
- STM32H5: `HSIDIVF` reads 0 for 1 us after an `HSIDIV` change and the HSI rate changes in the
  write. On an STM32H573 the flag clears one read after the write and sets again within 35
  cycles.
- Without TrustZone, a coverage report classes `RCC_CIFR` and `RCC_CICR` as `unimplemented`. The
  RCC sets the `RCC_CIFR` flags `RCC_CIER` enables, and a `RCC_CICR` write clears them.

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

Source: [spi.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/spi.cpp)

- Half duplex, CRC, TI mode, the autonomous trigger and SS timing are not modelled. The SPI runs
  as a plain full-duplex or simplex SPI. One model_warning per machine comes at the first transfer
  started with SPI_CFG2.SP, MIDI or MSSI or SPI_CFG1.CRCEN set.
- The dummy frame RM0456 Figure 858 shows ahead of the UDRDR pattern is UDRDR too, and UDR rises
  at once, with no propagation latency.
- RM0481 Table 645 limits SPI4 to SPI6 of the STM32H5 to DSIZE of 16 bits and CRC of 17 bits.
  The model accepts the DSIZE values of SPI1 to SPI3 there, and the CRC is not modelled.
- SPI_I2SCFGR of the STM32H5 SPI1 to SPI3 is storage: the I2S mode is not modelled.

## SBS

Source: [sbs.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/sbs.cpp), [secure_config.hpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/secure_config.hpp)

- The SBS model holds the temporal isolation level and the bus rules of RM0481 14.3.4 and 14.5,
  with TrustZone on and off. Under TrustZone `SBS_SECCFGR` and `SBS_RSSCMDR` are Secure,
  `SBS_EPOCHSELCR`, `SBS_ECCNMIR` and `SBS_CSLCKR` Secure and privileged, `SBS_CNSLCKR`
  privileged, `SBS_FPUIMR` privileged and Secure with FPUSEC, `SBS_MESR`, `SBS_CCCSR`,
  `SBS_CCVALR` and `SBS_CCSWCR` Secure with SBSSEC, `SBS_CFGR2` Secure with CLASSBSEC. A
  Non-secure access to a Secure register is RAZ/WI and sets SBSF in `GTZC1_TZIC_SR4`, on a read
  and on a write. An unprivileged access to a privileged register is RAZ/WI and sets no flag.
- SBSSEC makes `RCC_APB3ENR.SBSEN` and `RCC_APB3RSTR.SBSRST` Secure, so a Non-secure write of
  them is ignored while SBSSEC is set.
- FPUSEC takes privileged writes and reads for every Secure transaction, after the register
  description of RM0481 14.5.8. RM0481 14.3.4 lists FPUSEC among the privileged registers,
  whose unprivileged reads are RAZ.
- Without TrustZone `SBS_SECCFGR`, `SBS_RSSCMDR` and `SBS_CSLCKR` are RAZ/WI, RM0481 14.5.6
  gives `SBS_RSSCMDR` "always secure (RAZ/WI if nonsecure)". `SBS_EPOCHSELCR`, `SBS_ECCNMIR`,
  `SBS_FPUIMR` and `SBS_CNSLCKR` take privileged accesses only.
- `SBS_DBGCR` and `SBS_DBGLOCKR` take any access at HDPL0 and HDPL1 and are RAZ/WI above it,
  RM0481 14.5.4 and 14.5.5.
- RM0481 11.8.25 gives `RCC_APB3RSTR` no SBS bit. The STM32H562, H563 and H573 SVDs place
  SBSRST at bit 1, and the model serves it. A pulse keeps the security state of the unit and
  returns every other register to its reset value. It keeps HDPL in `SBS_HDPLCR` and
  `SBS_HDPLSR`, `SBS_SECCFGR`, the locks `SBS_CSLCKR` and `SBS_CNSLCKR`, `SBS_CFGR2`,
  `SBS_DBGCR` and `SBS_DBGLOCKR`, which RM0481 14.5.4 and 14.5.5 clear by a debug reset, and
  `SBS_RSSCMDR`, which RM0481 14.5.6 clears by a power-on reset. A Non-secure pulse leaves every
  Secure setting and lock as it was.
- A power-on or brown-out reset restores every SBS register. Every other machine reset restores
  all but `SBS_RSSCMDR`.
- Every SBS register but `SBS_HDPLCR`, `SBS_HDPLSR` and `SBS_SECCFGR` is storage: the functions
  of the debug, RSS, EPOCH, ECC NMI, FPU interrupt, memory erase, compensation cell, Class B and
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
  `ModelError` before the store.
- `SYSCFG_CSLCKR` stays storage at TZEN 0. RM0456 Table 131 makes it RAZ/WI.

## TIM

Source: [tim.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/tim.cpp)

Not modelled, logged:

- The encoder index, `ECR.IE`.
- Internal input sources of `TIxSEL` and internal ETR sources of `ETRSEL`. They read low.
- Master modes above 011 and TRGO2.
- A slave mode trigger input that no pin and no routed timer drives. No trigger reaches the slave
  mode controller.
- The trigger DMA request, TDE, and COM and trigger burst requests.
- The slave mode preload, `SMCR.SMSPE` and `SMSPS`. Both are storage and a counter start with
  SMSPE set logs once.

Differs from RM0456:

- An ARR written below the count lets the counter run to its maximum and wrap with no update.
  A count written above ARR in center-aligned mode does the same.
- UIFREMAP puts UIFCPY in bit 31 only where the description names a UIFCPY field, not on the
  32-bit TIM2 to TIM5.

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
