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
