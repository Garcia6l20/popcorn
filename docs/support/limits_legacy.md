# Limits not yet on a peripheral page

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

## ADC4

Source: [adc4.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/adc4.cpp), [adc4_rows.hpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/adc4_rows.hpp)

STM32U5 ADC4, RM0456 section 34: the voltage regulator, the calibration, the enable and disable
sequence, the ADC clock, the interrupt, the channel sequencer, the conversion timing and
`ADC_DR`.

Not modelled, each logged once at the `ADSTART` that uses it, a model_error under
strict_peripherals that keeps `ADC_CR`:

- The triggers `tim1_trgo2` and `exti15`, `EXTSEL` 0 and 7: no model produces them, `ADSTART`
  stays set and nothing converts.
- `AUTOFF` with `DPD`, the autonomous mode of RM0456 34.4.24: its clock requests and LDO
  power-off are not modelled, the conversions run as with `AUTOFF` alone. `DPD` alone is stored.
- `VIN[12]`, VCORE, and `VIN[14]`, VBAT / 4, with `VBATEN` in the sequence: they convert 0, the
  board model has no VBAT source and the level of VCORE depends on the PWR voltage range of
  DS13737, which is not modelled.
- `VIN[0]` sampled for less than the typical 12.65 us of tS_vrefint, DS13737 Table 36, or
  `VIN[13]` for less than the 13 us of tS_temp, Table 109, at `ADSTART` or at the `VREFEN` or
  `VSENSESEL` write with `ADSTART` set: the exact level converts.
- `VREFPROT` and `VREFSECSMP` of `ADC_PWRR`: tCONV stays at tSMPL + tSAR, the DS13737 Table 106
  minimum.

Analog watchdogs, RM0456 34.4.25:

- At the end of each conversion the 12-bit raw result, left aligned, is compared with
  `ADC_AWDxTR`: AWDx sets when it is below LTx or above HTx, an LTx above HTx flagging every
  result. A result an overrun discards is compared too. AWDx sets with EOC, inferred, no figure
  gives a delay.
- AWD1 guards every channel of the sequence with `AWD1EN` and `AWD1SGL` 0, the channel `AWD1CH`
  names with `AWD1SGL` 1. An `AWD1CH` of 18 to 21 or 24 to 31, reserved, guards no channel and
  is logged once at `ADSTART`, a model_error under strict_peripherals. AWD2 and AWD3 guard the
  channels set in `ADC_AWD2CR` and `ADC_AWD3CR`.
- A threshold write while a conversion samples or converts masks that watchdog for that
  conversion. The conversion starts with its sampling, inferred. The new thresholds apply from
  the next one. With the oversampler the mask lasts for the whole series of N conversions,
  inferred. A threshold write at the exact end instant of a conversion masks the next one,
  inferred. A result arriving while `ADC_DR` is frozen after a one-shot DMA end is compared,
  inferred.
- The `ADC_AWDx_OUT` signals to the timers' ETR inputs are not produced.

Oversampler, RM0456 34.4.26:

- With `OVSE` each result is the sum of N conversions of the channel, N = 2 << `OVSR`, 2 to 256,
  shifted right by `OVSS` with rounding to the nearest, then truncated to 16 bits, Table 335. It
  lands right aligned whatever `ALIGN` says. RES applies to each conversion before the sum.
- The conversions of a series follow each other, each with its own sampling: EOSMP sets after
  every sampling, EOC after the Nth conversion, EOS after N conversions of every channel. ADSTP
  and ADDIS drop the sum, ADDIS inferred.
- An `OVSS` of 9 to 15, reserved, shifts by 8 and is logged once at `ADSTART`, a model_error under
  strict_peripherals, the shift inferred.
- The watchdogs compare `ADC_DR[15:4]` of the result and ignore RES, RM0456 34.4.26.
- With `DISCEN` one trigger starts the N conversions of a channel. With `TOVS`, `DISCEN` taken as
  1, each trigger starts one conversion and the series goes on at the next trigger. A software
  `ADSTART` clears at the end of each conversion with `TOVS`, and the oversampler fields of
  `ADC_CFGR2` are then writable between the conversions, both inferred.

Low-power features, RM0456 34.4.22 and 34.4.24:

- With `WAIT` a new conversion starts only when `ADC_DR` is read by a bus master other than the
  debugger or `EOC` is cleared, from that instant, inferred. A continuous sequence holds after each
  conversion, a software `ADSTART` with `EOC` set holds until then, and a hardware trigger that
  arrives while a conversion runs or `EOC` is set is ignored. No overrun occurs.
- With `AUTOFF` `ADRDY` stays clear and each start from a software `ADSTART` or a hardware
  trigger waits tLATR then tSTAB, a read that releases `WAIT` waits tSTAB, 17 ADC clock cycles,
  inferred. The conversions of a continuous sequence follow each other with no power-up,
  Figure 295.
- With `LFTRIG` a software or hardware trigger starts 2 ADC clock cycles later. With `WAIT` only
  the first trigger does, taken as the first after `ADSTART`, inferred. With `AUTOFF` it adds
  nothing, inferred.
- `ADC_PWRR` bits are stored. `VREFPROT` and `VREFSECSMP` are logged as above. The ADC4 has no
  VREGPW bit.

Triggers and DMA:

- With `EXTEN` not 00 the ADC listens to the signal `EXTSEL` selects, RM0456 Table 329:
  `tim1_cc4`, `tim2_trgo`, `tim15_trgo` and `tim6_trgo` are produced. `EXTEN` 01 takes the rising
  edge of a TRGO pulse, 10 the falling edge one timer clock later, 11 both. An edge during a
  conversion or the tLATR is ignored. The sequence starts tLATR after the edge, inferred for every
  clock. An edge ahead of the machine time is taken at its own time, after the ADC's own events
  due then, inferred. `ADSTART` stays set at the end of a sequence. The `tim1_oc4` selection takes
  the capture/compare event `tim1_cc4`, inferred.
- `EXTSEL` 5 and 6 take the outputs `lptim1_ch1` and `lptim3_ch2` of the LPTIM model, RM0456
  Table 329. `EXTEN` 01 takes the rise of the output, 10 its fall and 11 both, each at the instant
  the output changes, and the sequence starts tLATR after it. The ADC listens from `ADSTART` with
  the LPTIM running or not.
- DMAEN holds the `adc4_dma` request high while EOC is set and OVR clear, a DMA read of `ADC_DR`
  drops it. With DMACFG 0 the request stops and `ADC_DR` freezes when the GPDMA channel
  completes, `ADSTART` staying set until it rises again, inferred. A result arriving then leaves
  `ADC_DR` and EOC as they are, and the last result of a sequence sets EOS, inferred. DMACFG 1
  keeps requesting. DMAEN written with EOC set raises the request, inferred. The calibration
  order RM0456 34.4.23 asks for DMAEN is not checked.

Internal channels, RM0456 Table 329 and 34.4.27:

- `VIN[0]` reads VREFINT with `ADC_CCR.VREFEN` and `VIN[13]` the temperature sensor with
  `VSENSESEL`, 0 without. They read the factory VREFINT_CAL and TS_CAL1 words, taken at 3.0 V and
  30 C, scaled by 3000 / VDDA and cut from 14 to 12 bits, `RES` shifting them like any result,
  inferred. A part description with no such word makes an `ADSTART` that converts the channel a
  config_error.
- `VIN[21]` reads the DAC1 output `ADC_OR.CHN21SEL` selects, 0 while that output reaches no
  on-chip peripheral. It has no enable bit. The level is taken at the end of the sampling like a
  pad level.
- `ADC_OR` follows the lock of `ADC_CFGR1`, RM0456 34.7.17.

Timing and values:

- LDORDY sets 25 us after ADVREGEN, the tADCVREG_STUP maximum of DS13737 Table 106.
- The calibration takes 123 ADC clock cycles, the typical tOFF_CAL of DS13737 Table 106. The
  factor it leaves in `ADC_CALFACT` and `ADC_DR` is 0, inferred. The factor leaves the codes
  unchanged.
- ADRDY sets 17 ADC clock cycles after ADEN: tSTAB of DS13737 Table 106 is 3 cycles and one
  conversion, here a 12-bit conversion at SMP 000, inferred. ADEN before LDORDY sets ADRDY tSTAB
  after LDORDY, inferred.
- ADEN written with ADVREGEN 0 sets ADVREGEN and starts the regulator, ES0499 2.9.2, with the
  timing above, inferred.
- The calibration needs the regulator ready, its LDORDY event, whether software cleared the flag
  since, inferred.
- Sampling starts tLATR after the `ADSTART` write: 4 ADC clock cycles at fADC = HCLK and HCLK / 2
  and 3.75 at HCLK / 4 with the bus clock as kernel clock, DS13737 Table 106. Any other kernel clock
  takes 4 cycles, inferred, RM0456 Table 330 calls its latency not deterministic. An `ADSTART`
  before ADRDY samples too, inferred. ADSTP clears ADSTP and ADSTART in the write, inferred. A
  sequence that waited for the ADC clock starts sampling when the clock arrives, inferred. EOC and
  the result land at the end of tSAR, the `ADC_DR` write latency taken as 0, inferred. The next
  conversion samples from the end of the previous one.
- An `ADSTART` with no channel in the sequence is ignored and logged once, a model_error under
  strict_peripherals, inferred. `DISCEN` or `TOVS` with `CONT`, forbidden by RM0456 34.4.17,
  converts with `CONT` ignored and is logged once, a model_error under strict_peripherals.
- A write changing `ADC_CFGR1`, `ADC_SMPR`, `ADC_CHSELR`, `ADC_CFGR2`, `ADC_AWD2CR`,
  `ADC_AWD3CR` or `ADC_OR` with `ADSTART` set keeps the old register, RM0456 34.7.4 to 34.7.17,
  logged once for the seven, a model_error under strict_peripherals.
- With `OVRMOD` 0 every result is discarded while EOC or OVR is set, RM0456 34.4.23: `ADC_DR`
  and EOC keep their values and OVR is set. A rank past the end of a shortened sequence starts
  over at the first channel.
- `ADC_IER`, `ADC_AWD1TR` to `ADC_AWD3TR` and `ADC_CCR` accept writes while `ADSTART` is set.
  RES is accepted with ADEN 1 and ADSTART 0, where RM0456 34.4.18 asks for ADEN 0.
- ADDIS clears ADEN and ADDIS 6 ADC clock cycles later, as ES0499 2.9.2 states for its startup
  sequence, inferred for every other use.
- A reserved `ADC_CCR.PRESC`, 1100 to 1111, divides by 256, inferred, and is logged once, a
  model_error under strict_peripherals that keeps the old `ADC_CCR`.
- Writes that RM0456 34.4.8, 34.7.3, 34.7.13, 34.7.16 and 34.7.18 forbid are ignored and logged
  once, a model_error under strict_peripherals that keeps the old register.

Errata of ES0499 Rev 12:

- 2.9.1, an ADC4 conversion disturbed by an ADC1 sampling phase on VREF+, is an electrical effect
  outside the model. The workaround, both prescalers at 0000, applies unchanged.
- 2.9.2, the ADC clock request active after reset, is a current consumption effect with no
  counterpart. The timing of its workaround sequence, ADEN then ADDIS, is the model's.
- 2.2.8, MSIK and HSI16 kept on when ADCDACSEL selects them, is a clock-tree effect outside the
  model.

## DAC

Source: [dac.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/dac.cpp), [dac_rows.hpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/dac_rows.hpp)

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

STM32L1 DAC, RM0038 section 13, the same model as a subset of the STM32L4 one. It has no
self-calibration (`CENx`, `DAC_CCR`, `CAL_FLAGx`), no `DAC_MCR` and no sample and hold, and its
software trigger register is `DAC_SWTRIGR`.

- With ENx 1 the channel drives its pad, PA4 or PA5, with `DAC_DORx` x VDDA / 4096 from each load on.
  The output drives the pad only. BOFFx is stored and has no effect on the level, inferred.
- TSELx follows RM0038 Table 68: 000 `tim6_trgo`, 010 `tim7_trgo`, 011 `tim9_trgo`, 100
  `tim2_trgo`, 101 `tim4_trgo`, 110 `exti9`, 111 SWTRIGx. TSELx 001 is reserved: a `DAC_CR` write
  with TENx set that selects it loads on no source, logs one `model_warning`, and raises
  `ModelError` under strict_peripherals, leaving `DAC_CR` as before the write.
- The loads, the waves, the holding register views, DMAENx with `dac_ch1_dma` and `dac_ch2_dma` on
  DMA1 channels 2 and 3, DMAUDRx and the interrupt line `DAC_IRQn` follow the STM32L4 rules above
  with RM0038 13.3 in place of RM0351 19.4.
- ES0125, ES0224, ES0240, ES0241, ES0242 and ES0578 hold the same three DAC items, 2.6.1 to 2.6.3 in
  ES0125, ES0240 and ES0241 and 2.5.1 to 2.5.3 in the others. 2.x.1, the DMA request not cleared by
  clearing DMAENx or the DAC clock, status A, is reproduced: a high `dac_chx_dma` stays high until a
  DMA transfer acknowledges it. 2.x.2, DMAUDRx not set when an internal trigger lands on the cycle of
  the DMA acknowledge, status N: silicon decides at the trigger detection, popcorn decides DMAUDRx
  at the load, three APB1 cycles after the trigger, RM0038 13.3.6, and sets the flag when a hardware
  load finds `dac_chx_dma` still high. A
  trigger that lands on the cycle of the acknowledge follows that rule, where silicon does not set the
  flag, so a firmware that mixes software and hardware triggers can see DMAUDRx that silicon does not
  show. 2.x.3,
  the spurious activation of the output buffer on PA4 and PA5 by a fast falling edge, status A, is an
  electrical effect outside the model.

STM32U5 DAC1, RM0456 section 35, and STM32H5 DAC1, RM0481 section 30, the L4 model with another
register layout. The digital interface runs on the AHB clock, `dac_hclk`: loads take one cycle with
TENx 0 and three cycles after a trigger, RM0456 35.4.6. The clock selection of RCC ADCDACSEL does
not change the load timing.

- TSELx 0000 is SWTRIGx and 0001 to 1111 listen to `dac_chx_trg1` to `trg15`, RM0456 Table 342 and
  RM0481 Table 306: `tim1_trgo`, `tim2_trgo`, `tim4_trgo`, `tim5_trgo`, `tim6_trgo`, `tim7_trgo`,
  `tim8_trgo`, `tim15_trgo`, trg9 and trg10 reserved, trg11 `lptim1_ch1`, trg12 `lptim3_ch1` on the
  STM32U5 and `lptim2_ch1` on the STM32H5, trg13 `exti9`, trg14 and trg15 reserved. The STM32H5
  trg14 and trg15 are `play_out11` and `play_out12` of the STM32H533 and STM32H543 only. A
  reserved TSELx, or on the STM32H5 an LPTIM source, with TENx set logs one `model_warning`
  and loads on no source. The STM32U5 LPTIM1 and LPTIM3 produce their sources. Under strict_peripherals it raises `ModelError` and leaves `DAC_CR` as
  before the write.
- DACxRDY sets the typical `tWAKEUP` of the datasheet after ENx rises and clears with ENx, RM0456
  35.4.4: 4.2 us with the output buffer on and 2 us with it off on the STM32U5, 5 us and 2 us on the
  STM32H5, DS13737 and DS14121. The datasheet value is the analog settling time, so the DACxRDY
  duration is inferred. A holding register write or a SWTRIGx write with ENx 1 and DACxRDY 0, which
  RM0456 35.4.4 does not allow, logs one `model_warning`. Under strict_peripherals it raises
  `ModelError` and leaves the register. A hardware trigger in that window loads DAC_DORx and logs
  nothing, inferred, since the manual gives no effect. ENx cleared before DACxRDY sets cancels the
  wake-up.
- SINFORMATx inverts bit 11 of the data when it loads into `DAC_DORx`, RM0456 35.4.5 Table 343. The
  inversion is applied to the holding value before the wave is added, inferred.
- `DACCxDHRB` of `DAC_DHR12Rx`, `DAC_DHR12Lx` and `DAC_DHR8Rx` store what is written and read back
  through the three views.
- With DMAENx 1 a hardware load raises `dac1_ch1_dma` or `dac1_ch2_dma`, the GPDMA1 requests of RM0456
  Table 137 and the RM0481 GPDMA request table, and a DHRx write, SWTRIGx or a load with DMAENx 0
  raises none. The request drops at the GPDMA acknowledge or when DMAENx clears, inferred. A hardware
  load that finds the request high sets DMAUDRx and still loads DORx. DMAUDRx with DMAUDRIEx holds
  `DAC1_IRQn`, the DAC's own line.
- With DMADOUBLEx and DMAENx 1, RM0456 35.4.9 and RM0481 30.4.9, a hardware trigger alternates: the
  first loads DORx from DHRx, keeps DHRBx as DORBx and raises the request, the second makes DORBx the
  output, sets DORSTATx and raises no request, and the next one loads DORx and clears DORSTATx.
  DORx reads its own value while DORSTATx is 1. SINFORMATx inverts DORBx as it does DORx. The
  underrun check runs at the first trigger of a pair only, inferred. A DHRx write that loads,
  a trigger with DMAENx 0 and ENx 0 load DORx, clear DORSTATx and restart the
  alternation, inferred. The wave term is added at the first trigger only, inferred.
- STM32U5 `DAC_AUTOCR` is storage: a firmware write logs one `model_warning` and raises `ModelError`
  under strict_peripherals before the store. The autonomous mode of RM0456 35.4.16 is not modelled,
  and a hardware trigger with AUTOMODE set behaves as without.
- DMADOUBLEx written while ENx or DMAENx is 1 keeps the old bit, RM0456 35.4.9. It logs one
  `model_warning` and raises `ModelError` under strict_peripherals, leaving `DAC_MCR`.
- HFSEL is stored. It has no effect: the model has no conversion delay.
- STM32U5 `DAC_MCR.MODEx` drives the pad and the on-chip peripherals as the STM32L4 table does. On the
  STM32H5 MODEx 000, 010, 100 and 110 connect the external pin only, and 001, 011, 101 and 111 are
  reserved, RM0481 30.7.16: a write of a reserved value keeps the old MODEx, logs one `model_warning`
  and raises `ModelError` under strict_peripherals.
- ES0499 2.2.8 and ES0565 2.2.31, kernel clock request and output voltage with another ADCDACSEL, are
  clock-tree and electrical effects outside the model.

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

## LPTIM

Source: [lptim.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/lptim.cpp), [lptim_v2.cpp](https://github.com/Garcia6l20/popcorn/blob/main/native/src/periph/stm32/lptim_v2.cpp)

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

### STM32U5 LPTIM1 to LPTIM4

STM32U5 LPTIM1 to LPTIM4, RM0456 section 58, count the kernel clock of RCC_CCIPR1 LPTIM2SEL or
RCC_CCIPR3 LPTIM1SEL and LPTIM34SEL through the prescaler, started by CNTSTRT or SNGSTRT. LPTIM1
to LPTIM3 have the ISR, ICR and DIER of RM0456 58.7.2, 58.7.5 and 58.7.8, LPTIM4 those of 58.7.1,
58.7.4 and 58.7.7. A description patch removes the CCMR1 and CCR2 the STM32U5 SVDs list on
LPTIM4, as RM0456 58.3 Table 599 gives LPTIM4 no channel.

- The counter starts at the later of the CNTSTRT or SNGSTRT write plus 3 kernel clocks, RM0456
  58.4.7, and the ENABLE write plus two prescaled clocks, RM0456 58.4.13, the prescaled reading
  inferred. A start write made with ENABLE 0 is discarded, also when the same write sets ENABLE,
  inferred. CNTSTRT and SNGSTRT read 0 at once, inferred.
- The counter counts 0 to ARR and overflows to 0. The repetition counter, loaded with REP at the
  start, inferred, counts the overflows down, and an overflow with it at 0 is the update event: it
  transfers the preloaded values and reloads REP, RM0456 58.4.16. One-shot mode stops at 0 at the
  update event, RM0456 58.4.8. An ARR write clears UE, RM0456 58.7.2.
- CC1IF and ARRM set one counter cycle after the counter reaches CCR1 and ARR, so CC1IF reads CNT
  CCR1 + 1 and ARRM reads CNT 0, RM0456 58.4.19. A counter cycle is one prescaled tick. UE sets one
  counter cycle after the update event, inferred from the same note, which lists the update event
  flag. CCR1 above ARR never matches, CCR1 equal to ARR sets CC1IF with ARRM, and a counter above a
  lowered ARR counts to 0xFFFF before it wraps, inferred. A CCR1 value the counter holds at the
  instant the value takes effect matches one counter cycle later, a smaller one in the next period,
  inferred. With the kernel clock counting, a match flag in flight, its match at or before the
  instant its value takes effect and its flag after, sets at that instant, inferred. A COUNTRST or a
  RSTARE read restarts the count and loses a flag in flight, inferred.
- CMP1OK and ARROK set 3 kernel clocks after the CCR1 or ARR write, inferred: RM0456 58.4.11
  states a latency with no number. With PRELOAD 0, or with the counter stopped, the value applies
  at the flag, and counter ticks up to it compare with the old values, inferred. With PRELOAD 1
  and the counter started, from the CNTSTRT or SNGSTRT write on, inferred, it applies at the
  first update event at or after the flag, RM0456 58.4.11. ENABLE written to 0 and the one-shot
  stop apply such a value at once, inferred.
- REPOK sets 4 APB clocks plus 3 kernel clocks after an RCR write, RM0456 58.4.16, the APB clock
  of the instance's bus, APB3 or APB1 for LPTIM2, taken at the write, inferred. The repetition
  counter takes the new REP at the first update event at or after REPOK, with PRELOAD 0 or 1,
  RM0456 58.4.16, inferred for PRELOAD 0, where RM0456 58.4.11 states an immediate RCR update. The
  caution of RM0456 58.4.16 and 58.7.16, an RCR change less than five counter cycles
  before the autoreload match with PRELOAD 0, gets no special case: the rule above applies.
- A flag due at the instant of an update event applies before it: the update and a one-shot
  stop use the old value, the new one counts from then, and a REPOK at that instant reloads the
  new REP, inferred.
- COUNTRST written with ENABLE 1 resets CNT 3 kernel clocks later, RM0456 58.4.14, and reads 1
  until then, RM0456 58.7.11. The count restarts at that instant and an update event due then
  is dropped, inferred. A COUNTRST write with ENABLE 0 is discarded, RM0456 58.7.11.
- With RSTARE 1 a CPU or DMA read of CNT returns the count and resets CNT, RM0456 58.4.14, the
  count restarting at the read, inferred. A debugger read leaves CNT, inferred. RSTARE written
  with ENABLE 0 is discarded, RM0456 58.7.11.
- DIEROK sets 3 kernel clocks after a DIER write, and the enables take effect then, inferred:
  RM0456 58.7.8 states the flag with no latency. Each DIER bit the write changes clears its ISR
  flag at that instant, ES0499 2.17.3, the instant inferred. A flag that sets while its DIER bit
  is in effect raises the LPTIM IRQ until ICR clears it, so a DIER bit set after its flag raises
  nothing, RM0456 58.6.
- Terms due at the same time apply in the order CC1IF, ARRM, CMP1OK, ARROK, DIEROK, REPOK,
  COUNTRST, UE, update event, mode switch, inferred. A match flag due with a value that takes
  effect at the same instant belongs to the old value.
- SNGSTRT written while the counter runs in continuous mode makes it stop at a later update
  event, and CNTSTRT written in one-shot mode makes it go on counting, RM0456 58.4.8. The new mode
  reaches the counter 3 kernel clocks after the write, inferred, so an update event before then
  keeps the old mode. The same mode written again while the
  counter runs is discarded, inferred. A second CNTSTRT or SNGSTRT before the first reaches the
  counter replaces it, logs one `model_warning` and raises `ModelError` under `strict`, inferred.
- ENABLE written to 0 stops the counter at 0, keeps the flags and a pending CCR1, ARR, RCR or DIER
  write, and drops a pending COUNTRST and the repetition count, inferred.
- A CFGR write with ENABLE 1, a DIER, CCR1, ARR or RCR write with ENABLE 0, RM0456 58.7.8,
  58.7.10, 58.7.12, 58.7.13 and 58.7.16, and a second DIER, CCR1, ARR or RCR write before its OK
  flag, RM0456 58.4.11, 58.4.16 and 58.7.8, log one `model_warning` each and raise `ModelError`
  under `strict`. The register keeps its value, inferred, except the second write outside
  `strict`, whose value replaces the pending one, inferred.
- CFGR2 IN1SEL selects input 1, IN2SEL input 2, IC1SEL and IC2SEL the capture inputs.
- The output of channel 1 of LPTIM1 to LPTIM3 drives the trigger signal `lptim1_ch1`, `lptim2_ch1`
  or `lptim3_ch1`, channel 2 `lptim1_ch2` to `lptim3_ch2`, and the output of LPTIM4 `lptim4_out`. No
  LPTIM output drives a pin. OC1REF is high while CNT is above CCR1, so the output rises at CNT CCR1
  + 1 with CC1IF, CCR1 0 at CNT 1, and falls at CNT 0 with ARRM, RM0456 58.4.19 and Figures 747 and
  748. A signal edge carries the whole ps at or after its instant, so a kernel clock that is not a
  whole ps puts it up to 1 ps late. CCR1 at or above ARR keeps it inactive, RM0456 58.4.10. A CCR1
  value that takes effect moves the level at its CMP1OK, inferred. A one-pulse run gives one pulse.
  With WAVE 1 it rises once and keeps its level until ENABLE is written to 0, and a CNTSTRT or
  SNGSTRT after the first start is discarded until then, the software restart inferred. The level is
  the compare result xor CC1P bit 0 with CC1E 1 and CC1SEL 0, and 0 otherwise, inferred. LPTIM4 uses
  WAVPOL. A polarity or CC1E change takes effect at the register write, also with ENABLE 0, RM0456
  58.4.10. ENABLE written to 0 and LPTIMxRST drop the level to inactive at the write, inferred.
- Channel 2 of LPTIM1 to LPTIM3 compares CCR2 as channel 1 compares CCR1: CC2IF and the level of
  `lptim<n>_ch2` come at CNT CCR2 + 1, CMP2OK follows a CCR2 write 3 kernel clocks later, and
  PRELOAD 1 applies CCR2 at the update event. The level is the compare result xor CC2P bit 0 with
  CC2E 1 and CC2SEL 0, and 0 otherwise, inferred. CC2IF sets also with CC2E 0: RM0456 58.7.2 names
  CC2SEL 0 as the only condition, so the reset CCR2 0 sets it at CNT 1 of every run. WAVE 1 latches
  each channel on its own, also with its CCxE 0, inferred.
- With CCxSEL 1 channel x of LPTIM1 to LPTIM3 captures, RM0456 58.3 Table 599 and 58.4.18. Its input
  is the pin signal `lptim<n>_chx` with CFGR2 ICxSEL 00, RM0456 58.4.3 Tables 607 to 610. IC2SEL 01
  to 11 select the internal clocks of RM0456 Tables 608 to 610, LSI and LSE on LPTIM1, HSI16/256,
  MSIS/1024 and MSIS/4 on LPTIM2: the input toggles every half period of the divided rate, rounded
  up to a whole ps, from its level when the channel starts to capture, the phase inferred, and a
  rate change takes effect from the last toggle, inferred. With ICxF not 00 a level change counts
  once it has held for 2, 4 or 8 clocks of the RCC kernel clock, a shorter pulse rejected, RM0456
  58.7.17, the sampling phase taken as the change, inferred. ICxF applies per channel as CCMR1 holds
  it, inferred, where RM0456 58.4.5 sets the capture filters as one group. CCxP selects the rising,
  the falling or both edges and ICxPSC captures every 1, 2, 4 or 8 edges, RM0456 58.7.17, the
  prescaler counting only with CCxE 1 and reset by CCxE written to 0, inferred. With CCxE 1 a
  capture latches CNT into CCRx, RM0456 58.4.18, the RM0456 Table 613 latency in counter steps after
  the counter clock of the edge, so CCRx minus the offset is the count at the edge, and CCxIF sets
  at the latch, inferred. A capture while CCxIF is set also sets CCxOF, RM0456 58.4.18, and CCxOIE
  arms the IRQ. ES0499 2.17.4: a CCxOCF write from one counter clock before a capture up to the
  capture leaves CCxOF unset at every later over-capture, the window inferred, until CCxE is written
  to 0 and back to 1 at least PRESC x 3 kernel clocks later, the erratum's workaround, or ENABLE is
  written to 0, inferred. With the counter stopped the capture comes the same latency on the
  prescaled kernel clock counted from ENABLE and latches the stopped CNT, inferred. Captures in
  flight apply in the order of their instants, and edges whose captures fall on the same counter
  clock give one capture, inferred. A capture in flight is dropped by CCxE or CCxSEL written to 0
  and by ENABLE written to 0, and keeps its instant across a kernel rate change, inferred. A CPU or
  DMA read of CCRx clears CCxIF, RM0456 58.7.3, a debugger read leaves it, inferred. CCRx is read-
  only, RM0456 58.7.12 and 58.7.18: a write is discarded and sets no CMPxOK, inferred. A capture
  channel compares nothing and drives its signal low. A CFGR2 write that changes ICxSEL counts the
  level change it makes as an edge, inferred. With the kernel clock stopped and CKSEL 0 an edge
  captures nothing, inferred. LPTIMxRST takes the capture input levels, inferred. CCxP 10 on a
  capture channel logs one `model_warning` at a start and at a CCMR1 write while the counter runs
  and raises `ModelError` under `strict`, RM0456 58.7.17 reserving it: both edges then capture. A
  CCMR1 write that changes CCxSEL, ICxF, CCxP or ICxPSC of a capture channel with CCxE 1 logs one
  `model_warning` and raises `ModelError` under `strict`, RM0456 58.7.17, the new setting taking
  effect at the write. Two CCMR1 writes closer than PRESC x 3 kernel clocks both apply at once,
  where RM0456 58.7.17 calls the result unpredictable.
- A kernel rate change keeps CNT, the kernel clocks already spent in the current tick or the
  start latency, and the kernel clocks left before each pending OK flag, COUNTRST and mode
  switch, inferred. A stopped kernel clock, MSIKON 0 under LPTIM1SEL 00 included, freezes all of
  them until it runs again, inferred.
- With CKSEL 1 or COUNTMODE 1 the counter counts the active edges CKPOL selects on input 1, rising,
  falling or both, instead of the kernel clock, RM0456 58.4.12 and 58.7.10. Input 1 is the pin
  signal `lptim1_in1` to `lptim4_in1` with CFGR2 IN1SEL 00, RM0456 58.4.3 Table 605: among the pins
  the pin data names, PB5 AF1, PB1 AF14, PB13 AF2 and PD13 AF13 on the U575. A CFGR2 write that
  changes IN1SEL counts the level change it makes as an edge, inferred.
- With CKSEL 1 the active edges clock the kernel logic, RM0456 58.4.12, so the latencies stated in
  kernel or counter clocks count edges: ENABLE 2 edges, RM0456 58.4.13, COUNTRST 3 edges, RM0456
  58.4.14, and REPOK 3 edges, RM0456 58.4.16, with its 4 APB clocks dropped, inferred. The start,
  CMP1OK, CMP2OK, ARROK, DIEROK and a mode switch count 3 edges, inferred, which with the ENABLE
  latency gives the five lost edges of RM0456 58.4.12 after an ENABLE and a CNTSTRT. PRESC divides
  the edges after the start, RM0456 58.4.6, the phase inferred. An edge counts first, then the terms
  it completes apply, inferred, so a REPOK at an update edge reloads the old REP. With no edge
  nothing sets and the counter never starts.
- With COUNTMODE 1 every active edge counts at its instant from the start instant on, the sampling
  on the kernel clock taken as the edge itself, an edge with the kernel clock stopped is dropped and
  a term due at the instant of an edge applies before it, inferred.
- A counted edge moves CNT by one. CC1IF, CC2IF and ARRM set at the edge that leaves CCR1, CCR2 and
  ARR, and the output follows CNT, RM0456 58.4.19 read with one counted edge as the counter cycle,
  inferred. The edge that leaves ARR or 0xFFFF is the overflow, and with the repetition counter at 0
  the update event: PRELOAD, REP and the one-shot stop apply there. UE then sets at the next counted
  edge, or at once when the counter stops, inferred.
- With CKFLT 01, 10 or 11 a level change of input 1 counts once it has held for 2, 4 or 8 clocks of
  the RCC kernel clock, and a shorter pulse is rejected, RM0456 58.4.5 and 58.7.10, the sampling
  phase taken as the change and the rate read at the change, inferred. With that clock stopped the
  filter passes no change and the level it leaves becomes the filtered level, inferred. LPTIMxRST
  takes the input 1 level, inferred.
- A start with PRESC not 0 and COUNTMODE 1, CKPOL 11, CKPOL 10 with CKSEL 1, or CKFLT not 00 with
  the kernel clock stopped, which RM0456 58.4.5, 58.4.12 and 58.7.10 forbid, logs one
  `model_warning` each and raises `ModelError` under `strict`. Every edge then counts, both edges
  for the two CKPOL cases, inferred.
- With TRIGEN not 00, CNTSTRT and SNGSTRT arm the counter and choose its mode, and the edge TRIGEN
  selects on the trigger input TRIGSEL selects starts it, RM0456 58.4.7 and 58.7.10. TRIGSEL 0 is
  the pin signal `lptim1_etr` to `lptim4_etr`, RM0456 58.4.3 Table 604, among the pins the pin data
  names PB6 AF1, PA5 AF14, PB14 AF2 and PD2 AF13 on the U575. A CNTSTRT or SNGSTRT written again
  before the trigger re-arms in its mode, inferred. EXTTRIG sets at the trigger, RM0456 58.7.2, and
  the counter starts two counter clocks later, RM0456 58.4.7: two prescaled kernel clocks, or with
  CKSEL 1 two prescaled input 1 edges, inferred. With TRGFLT 01, 10 or 11 a trigger level change
  counts once it has held for 2, 4 or 8 clocks of the RCC kernel clock, and a shorter pulse is
  rejected, RM0456 58.4.5 and 58.7.10, the sampling phase taken as the change, inferred. A trigger
  before CNTSTRT or SNGSTRT starts nothing, RM0456 58.4.8, and sets no EXTTRIG, inferred. A trigger
  with the kernel clock stopped, and CKSEL 0, starts nothing and sets no EXTTRIG, inferred.
- A trigger while the counter runs is ignored and sets no EXTTRIG, RM0456 58.4.7 and 58.7.2, unless
  TIMOUT is 1: it then sets EXTTRIG and restarts the counter and the repetition counter from 0 two
  counter clocks later, RM0456 58.4.9 and 58.7.10, the latency inferred, and a match flag in flight
  is lost, inferred. In one-shot mode each trigger after the update event starts a new run, RM0456
  58.4.8, and with WAVE 1 only the first trigger starts one, a later one setting no EXTTRIG,
  inferred. LPTIMxRST takes the trigger level, inferred. TRGFLT not 00 with the kernel clock
  stopped logs one `model_warning` at the arm and raises `ModelError` under `strict`, RM0456
  58.4.5.
- With ENC 1 and CKSEL 0, on LPTIM1 and LPTIM2, RM0456 58.3 and 58.4.15, the counter counts the
  active edges CKPOL selects on the pin signals `lptim1_in1` and `lptim1_in2` (`lptim2_in1`,
  `lptim2_in2`) with CFGR2 IN1SEL and IN2SEL 00, RM0456 58.4.3 Tables 605 and 606, up or down by
  the level of the other input, RM0456 58.4.15 Table 612, between 0 and ARR: counting down from 0
  gives ARR. CKFLT filters both inputs, RM0456 58.4.5, and the direction takes the filtered level
  of the other input, inferred. A change of direction sets DOWN or UP at the edge, RM0456 58.4.15,
  and the count starts upwards, inferred. CC1IF, CC2IF and ARRM set at the counted edge that
  leaves CCR1, CCR2 and ARR in either direction, as with COUNTMODE 1, inferred. A down wrap from 0
  loads ARR, RM0456 58.4.15, and is an overflow: the repetition counter, the update event, PRELOAD
  and the one-shot stop apply there as at an up wrap, inferred. ENC takes precedence over
  COUNTMODE, inferred. An edge with the kernel clock stopped is dropped, inferred. The inputs never
  switch at the same time, RM0456 58.4.15. A CFGR2 write that changes IN2SEL counts the level
  change it makes as an edge, inferred. On LPTIM3 and LPTIM4, whose IN2SEL bits are reserved,
  RM0456 58.3 Table 599, a CFGR2 write stores them with no effect. SNGSTRT with
  ENC 1 logs one `model_warning` and raises `ModelError` under `strict`, RM0456 58.4.15 asking for
  continuous mode, and the one-shot stop then applies at a wrap. ENC 1 with CKSEL 1 logs one
  `model_warning` at a start and raises under `strict`, RM0456 58.4.15: input 1 then clocks the
  counter.
- LPTIM1RST to LPTIM4RST reset the registers and the state of their LPTIM.
- CNT reads the exact count, so the double read of RM0456 58.7.14 always agrees.
- These functions are not modelled. Each logs one `model_warning` at its first use and raises
  `ModelError` under `strict` at every use, after the write has taken the effect stated: at a
  counter start and at a CFGR2 write while it runs IN1SEL not 00 with CKSEL 1 or COUNTMODE 1, IN2SEL
  not 00 with ENC 1, IC1SEL not 00 and a reserved IC2SEL input on a capture channel, the input then
  reading 0. At a counter start and at a CCMR1 write while it runs CC1SEL or CC2SEL 1 with CKSEL 1,
  COUNTMODE 1 or ENC 1, a capture then latching CNT at the filtered edge. At a CNTSTRT or SNGSTRT
  write with TRIGEN not 00: TRIGSEL not 0, an input of RM0456 Table 604 no model produces, named in
  the warning, which leaves the counter stopped. A function the model lacks otherwise counts the
  kernel clock with channel 1 comparing CCR1.
- DMA requests, RM0456 58.4.21. With UEDE 1 a UE flag sets `lptimN_ue_dma` at each update event,
  and with CC1DE or CC2DE 1 a capture sets `lptimN_ic1_dma` or `lptimN_ic2_dma` with its CCxIF.
  A request stays high until a controller burst acknowledges it, and an event before the
  acknowledge is absorbed, inferred. A request raises only with its DIER enable in effect, at
  DIEROK. Writing 0 to UEDE, CC1DE or CC2DE resets its request at the write, RM0456 58.7.8. The ic
  requests also drop with CCxE 0 or ENABLE 0, the ue request with ENABLE 0 or every CCxE 0, RM0456
  58.4.21, the instant inferred. The ue request raises only while a CCxE is set, following the
  RM0456 58.4.21 Note literally, inferred. A CPU read of CCRx clears CCxIF and leaves the request,
  inferred. LPTIMxRST drops a raised request,
  inferred. LPTIM4 has no channel and no DMA request, and its UEDE bit is reserved, RM0456 58.3
  Table 599.
- While the machine holds its core with DBG_LPTIM1_STOP to DBG_LPTIM4_STOP set, RM0456 58.4.22, the
  instance freezes as a stopped kernel clock: CNT, the kernel clocks spent in the current tick and
  those left before each pending term are kept and resume at the release, the latencies inferred.
  A capture in flight keeps its instant and latches the frozen CNT there, and an edge on a pin
  input, a trigger or a capture input during the hold is dropped while the glitch filters keep
  sampling, inferred. A kernel rate change during the hold applies from the
  release. The hold is kept across LPTIMxRST. At a machine reset the model reads DBG_LPTIMx_STOP
  again, which a system reset keeps and a power-on reset clears.
- The internal input capture 2 clocks of LPTIM2 come from the RCC domains `timic_hsi`,
  `timic_msi_1024` and `timic_msi_4`, each the undivided oscillator rate with the divider in the
  LPTIM row, which run only with RCC_CCIPR1 TIMICSEL[2] set, RM0456 11.8.46. With it 0, the reset
  value, the input stays low, no capture happens and nothing is logged. TIMICSEL 001 to 011 are
  closed, inferred from the register text, which says 0xx disables the dividers, while the RM0456
  11.4 calibration note reads TIMICSEL[2:0] not 0. TIMICSEL bit 1 takes the /1024 tap from MSIK and
  bit 0 the /4 tap, else MSIS, the codes 100 to 111 of RM0456 11.8.46. Setting or clearing TIMICSEL
  starts or stops the clock at the write.
- The DIER bits LPTIM4 reserves, RM0456 58.7.7, read 0 and are not stored, inferred from "must be kept
  at reset value".
- A pin edge driven between two instructions applies at the next instruction instant, after the
  model events of that instant, because the scheduler orders by instruction count.
- The autonomous mode of LPTIM1 to LPTIM3, RM0456 58.4.20, has no LPTIM register bit. RCC_SRDAMR
  stores LPTIM1AMEN, LPTIM3AMEN and LPTIM4AMEN, and Stop is not modelled.
- ES0499 2.17.1, the LPTIM interrupt stuck at Stop entry when ENABLE clears near an interrupt, is
  probabilistic and outside the model. ES0499 2.17.2, ARRM and CMPM that "there is a chance" are
  not set at the end of a one-shot REP cycle with an odd REP and an APB clock slower than the
  kernel clock, unless {ARR, CMP} >= KER_CLK / (2 * APB_CLK), is probabilistic: the model always
  sets them and logs nothing. ES0499 2.17.4, the over-capture lost when CCxOF clears with a capture
  pulse active, is reproduced in the capture bullet above, on every silicon revision although the
  erratum lists revision A only.
- ES0499 2.3.1, LPTIM3_IN1 and LPTIM3_IN2 protected by LPTIM1SEC, is outside the model: the GPIO
  model passes an alternate function input to its peripheral whatever the security of the pin and of
  the peripheral, so neither the GTZC rule of RM0456 nor the erratum applies. ES0499 2.3.2, TIMICSEL
  not protected by LPTIM2SEC, is reproduced: the RCC model secures TIMICSEL while TIM16 or TIM17 is
  Secure, and a Non-secure TIMICSEL write with only LPTIM2 Secure is taken.

### STM32H5 LPTIM1 to LPTIM6

STM32H562, H563 and H573 LPTIM1 to LPTIM6, RM0481 section 48, use the model of the STM32U5 LPTIM
above with the rows of RM0481 48.3 Table 527. LPTIM1, 2, 3, 5 and 6 have the encoder mode, the input
capture, two channels and three DMA requests, LPTIM4 has no channel and no DMA request and drives
`lptim4_out` from WAVPOL. The STM32H5 SVDs give LPTIM4 the layout of LPTIM1, so its CCMR1 and CCR2 are
removed by a description patch, RM0481 48.7.19 Table 544 notes 4 and 5. LPTIM1 sits on APB3 with
LPTIM3 to LPTIM6, LPTIM2 on APB1. The STM32H523, H533, H543 and H553 implement LPTIM1 and LPTIM2
only and ship no description.

- The trigger inputs of TRIGSEL follow RM0481 Table 532: LPTIM1 tamp_trg2 and gpdma1_ch1_tcf at 4 and
  5, LPTIM2 gpdma1_ch0_tcf and gpdma1_ch4_tcf, the other instances reserved there. No model
  produces the RTC, TAMP or GPDMA triggers, so a start armed with such a TRIGSEL logs one
  `model_warning`, raises `ModelError` under `strict` and leaves the counter stopped.
- Input 1, input 2 and input capture 1 read the GPIO pin, the other multiplexer inputs of RM0481
  Tables 533 to 535 are reserved. Input capture 2 takes LSI and LSE on LPTIM1, HSI/1024, CSI/128 and
  HSI/8 on LPTIM2, Tables 536 and 537, and only the pin on LPTIM3, 5 and 6, Table 538.
- The internal input capture 2 clocks of LPTIM2 come from the RCC domains `timic_hsi` and
  `timic_csi`, which run only with RCC_CCIPR1 TIMICSEL set, RM0481 11.8.40. With it 0, the reset value,
  the input stays low, no capture happens and nothing is logged. Setting or clearing TIMICSEL starts or
  stops the clock at the write. The DIER bits LPTIM4 reserves, RM0481 48.7.7, read 0, inferred. The
  GPDMA block request rule of RM0481 Table 143 for `lptimN_ue_dma` is not checked.
- The DMA requests are `lptimN_ic1_dma`, `lptimN_ic2_dma` and `lptimN_ue_dma` of the GPDMA1 request
  table, N from 1 to 3, 5 and 6, and the debug hold reads DBG_LPTIM1_STOP to DBG_LPTIM6_STOP, as on
  the STM32U5. The STM32H5 has no autonomous mode, RM0481 Table 527.
- ES0565 2.11.3, a DIER bit change clearing its flag, and 2.11.4, the over-capture lost when CCxOF
  clears with a capture pulse active, are reproduced as ES0499 2.17.3 and 2.17.4 above, 2.11.4 on
  every silicon revision although the erratum lists revision A only. ES0565
  2.11.1, the interrupt stuck at Stop entry, is probabilistic and outside the model. ES0565 2.11.2,
  ARRM and CMPM not set with an APB clock slower than the kernel clock, is probabilistic: the model
  always sets them. ES0565 2.11.5, pll2_p_ck as kernel clock, is an RCC item outside the model: the
  kernel clock of LPTIMxSEL 001 is delivered as selected. ES0565 2.2.32, LPTIM2_CH1 holding off the
  low-power modes through EXTI input 49, is outside the model.
