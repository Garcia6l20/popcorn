# Limits

A built-in model serves the registers and behaviour the firmware of the examples and the tests
use. It does not serve every field of every register. Each section below lists what its model
leaves out and the deviations from the reference manual, RM0456 for the STM32U5, that its source
documents. The class doc in the linked source has the detail.

Most models meet an unmodelled setting with one `model_warning` log record per instance, then
keep running as the section says. Under `strict` each such warning raises `ModelError` instead.
The MDF and ADF model raises `UnmodelledConfig`. Read the log records of a run to see which
settings a firmware reached.

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

## ADC

Source: [adc.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/adc.cpp)

Not modelled, logged once each:

- Auto-delayed conversions, `ADC_CFGR1.AUTDLY`.
- The low frequency trigger mode, `ADC_CFGR2.LFTRIG`.
- The MDF data path, `ADC_CFGR1.DMNGT` 10.
- DMA requests gated by the AWD1 window, `AWDFILT1` with `DMNGT`.
- Left shift, gain compensation and offset of the result.
- Reserved trigger selections in `EXTSEL`.
- A reserved prescaler in `ADC12_CCR.PRESC` divides by 256.
- ADC4 has no model.

## CRC

Source: [crc.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/crc.cpp)

- A `CRC_DR` write that is not right-aligned is ignored and logged, RM0456 24.4.
- An even polynomial, which RM0456 24.3.3 does not support, is computed anyway and logged.
- A write narrower than 32 bits to any register but `CRC_DR` is ignored and logged. Such a read
  is logged.
- The RCC clock gate is not modelled. The CRC works with its RCC enable bit clear.

## DBGMCU

Source: [dbgmcu.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/dbgmcu.cpp)

- A freeze bit written while the core is held takes effect at the next hold or release. On
  silicon it takes effect at once.

## FLASH

Source: [flash.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/flash.cpp)

Not modelled:

- Write protection by the WRP areas. WRPERR never sets for it.
- Option-byte programming. OPTSTRT is cleared and the option bytes stay those of the machine
  description.
- Single ECC errors. ECCC never sets and ECCIE has no effect.

Differs from RM0456:

- A store while BSY is set completes at once and is applied when BSY clears. Silicon stalls the
  bus until then.
- Host and debugger reads, instruction fetches and vector table reads never detect an ECC error.
- An unprivileged fetch of a privileged page is a BusFault. RM0456 Table 73 gives zeros.
- A Secure bank erase over a bank with mixed pages is refused whole, by Table 70, not by 3.5.5.

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

## HASH

Source: [hash.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/hash.cpp)

- An HMAC key longer than 64 bytes with LKEY 0 is hashed as with LKEY 1, and logged.
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

## PWR

Source: [pwr.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/pwr.cpp)

- The low-power modes `PWR_CR1.LPMS` selects are not modelled. A deep-sleep WFI idles as a plain
  WFI.

## RCC

Source: [rcc.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/rcc.cpp)

- SRAMnEN and SRAMnSMEN stay Non-secure. The MPCBB and INVSECSTATE rule of RM0456 5.4 does not
  apply to them.
- A peripheral reset bit in an `RCC_xxxRSTR` register, written from 0 to 1, resets every
  register of that instance that no model claims, for any instance, modelled or not, such as
  LPTIM1. It also resets the instance's model when the model supports it: RNG, CRC, HASH,
  SYSCFG, ADC1 and ADC12 through ADC12RST, GPDMA1, LPDMA1, the timers, GPIOA to GPIOI, SPI, I2C,
  SAI, MDF1, ADF1, the USARTs, UARTs and LPUART1, and OTG_FS. A native plugin model that derives
  from `periph::bus_reset_listener`, and every Python model, hears it through `on_bus_reset()`.
  Any other model gets `reset()` with `reset_kind::peripheral`. A peripheral reset sets no
  RCC_CSR reset flag.
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
  as a plain full-duplex or simplex SPI.
- The dummy frame RM0456 Figure 858 shows ahead of the UDRDR pattern is UDRDR too, and UDR rises
  at once, with no propagation latency.

## SYSCFG

Source: [syscfg.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/syscfg.cpp)

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
