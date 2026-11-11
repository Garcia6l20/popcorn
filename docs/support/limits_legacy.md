# Limits not yet on a peripheral page

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
