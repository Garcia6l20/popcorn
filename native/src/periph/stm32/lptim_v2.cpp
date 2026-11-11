#include "dma.hpp"
#include "dma_signal.hpp"
#include "lptim_v2_rows.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/clock.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/periph/core_hold.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
using wide = unsigned __int128;

constexpr std::uint32_t compare_match          = 1u << 0;
constexpr std::uint32_t reload_match           = 1u << 1;
constexpr std::uint32_t trigger_event          = 1u << 2;
constexpr std::uint32_t counting_up            = 1u << 5;
constexpr std::uint32_t counting_down          = 1u << 6;
constexpr std::uint32_t compare_ok             = 1u << 3;
constexpr std::uint32_t reload_ok              = 1u << 4;
constexpr std::uint32_t update_event           = 1u << 7;
constexpr std::uint32_t repetition_ok          = 1u << 8;
constexpr std::uint32_t compare2_match         = 1u << 9;
constexpr std::uint32_t compare2_ok            = 1u << 19;
constexpr std::uint32_t overcapture1           = 1u << 12;
constexpr std::uint32_t overcapture2           = 1u << 13;
constexpr std::uint32_t interrupts_ok          = 1u << 24;
constexpr std::uint32_t update_dma_enable      = 1u << 23;
constexpr std::uint32_t capture_dma_enables[2] = {1u << 16, 1u << 25};
constexpr std::size_t   update_request         = 2;
constexpr std::uint32_t counter_top            = 0xFFFF;
constexpr std::uint32_t enable_bit             = 1u << 0;
constexpr std::uint32_t single_start           = 1u << 1;
constexpr std::uint32_t continuous_run         = 1u << 2;
constexpr std::uint32_t counter_reset          = 1u << 3;
constexpr std::uint32_t reset_after_read       = 1u << 4;
constexpr std::uint32_t tick_event             = 0;
constexpr std::uint64_t start_cycles           = 3;
constexpr std::uint64_t ok_cycles              = 3;
constexpr std::uint64_t bus_cycles             = 4;
constexpr std::uint64_t reset_cycles           = 3;
constexpr std::uint16_t enable_edges           = 2;
constexpr std::uint32_t input1_select          = 3u;
constexpr std::uint32_t input2_select          = 3u << 4;
constexpr std::uint32_t capture1_select        = 3u << 16;
constexpr std::uint32_t capture2_select        = 3u << 20;
constexpr std::uint32_t capture_setup          = 3u << 12 | 3u << 8 | 3u << 2 | 1u;

constexpr auto capture_offsets = std::to_array<std::array<std::uint8_t, 4>>({
    {2, 7, 9, 13},
    {3, 5, 6, 8 },
    {2, 3, 4, 5 },
    {2, 2, 3, 3 },
    {2, 2, 2, 2 },
    {2, 2, 2, 2 },
    {2, 2, 2, 2 },
    {2, 2, 2, 2 },
});

enum class term : std::uint8_t
{
  compare_written,
  compare2_written,
  reload_written,
  interrupts_written,
  repetition_written,
  counter_reset,
  compare,
  compare2,
  reload,
  update_flag,
  update,
  mode_switch,
  output,
  input,
  input2,
  trigger,
  capture1_input,
  capture2_input,
  capture1,
  capture2,
  capture2_clock,
};

enum class use : std::uint8_t
{
  input_source,
  capture,
  count,
};

constexpr auto unmodelled_table = std::to_array<std::string_view>({
    "IN1SEL not 00, comp1_out or a reserved input 1, with CKSEL 1 or COUNTMODE 1, IN2SEL not 00, "
    "comp2_out or a reserved input 2, with ENC 1, or IC1SEL or IC2SEL not 00 on a capture channel "
    "with a comparator or a reserved input: not modelled, the input reads 0",
    "CC1SEL or CC2SEL 1 with CKSEL 1, COUNTMODE 1 or ENC 1, the input capture with the counter "
    "clocked by an input: not modelled, a capture takes CNT at the filtered edge",
});
static_assert(unmodelled_table.size() == static_cast<std::size_t>(use::count));

struct lptim_v2_state
{
  std::uint64_t                             anchor_ps{};
  std::uint64_t                             anchor_frac{};
  std::uint64_t                             hz{};
  std::uint64_t                             enable_ps{};
  std::uint64_t                             settled_ps{};
  std::uint64_t                             floor_ps{};
  std::uint64_t                             compare_due{};
  std::uint64_t                             update_flag_due{};
  std::uint64_t                             compare2_due{};
  std::uint64_t                             reload_due{};
  std::uint64_t                             interrupts_due{};
  std::uint64_t                             repetition_due{};
  std::uint64_t                             reset_due{};
  std::uint64_t                             switch_due{};
  std::uint64_t                             out_ps{};
  std::uint64_t                             paused_units{};
  std::uint64_t                             generation{};
  std::uint64_t                             queued{};
  std::uint64_t                             input_due{};
  std::uint64_t                             input2_due{};
  std::uint64_t                             trigger_due{};
  std::uint32_t                             anchor_count{};
  std::uint32_t                             isr{};
  std::uint32_t                             armed{};
  std::uint32_t                             compare{};
  std::uint32_t                             reload{1};
  std::uint32_t                             compare_pending{};
  std::uint32_t                             compare2{};
  std::uint32_t                             compare2_pending{};
  std::uint32_t                             compare2_preload{};
  std::uint32_t                             reload_pending{};
  std::uint32_t                             interrupts{};
  std::uint32_t                             interrupts_pending{};
  std::uint32_t                             repetition{};
  std::uint32_t                             repetition_reload{};
  std::uint32_t                             repetition_pending{};
  std::uint32_t                             compare_preload{};
  std::uint32_t                             reload_preload{};
  std::uint8_t                              preloaded{};
  std::uint8_t                              running{};
  std::uint8_t                              held{};
  std::array<std::uint8_t, 3>               requested{};
  std::uint8_t                              single{};
  std::uint8_t                              paused_counting{};
  std::uint8_t                              switch_single{};
  std::uint8_t                              raw{};
  std::uint8_t                              out{};
  std::uint8_t                              latched{};
  std::uint8_t                              started_once{};
  std::uint8_t                              input_level{};
  std::uint8_t                              input_filtered{};
  std::uint8_t                              input_pending{};
  std::uint8_t                              input2_level{};
  std::uint8_t                              input2_filtered{};
  std::uint8_t                              input2_pending{};
  std::uint8_t                              down{};
  std::uint8_t                              divided{};
  std::uint8_t                              update_pending{};
  std::uint16_t                             enable_left{};
  std::uint8_t                              trigger_level{};
  std::uint8_t                              trigger_filtered{};
  std::uint8_t                              trigger_pending{};
  std::uint8_t                              armed_mode{};
  std::array<std::uint8_t, 2>               capture_level{};
  std::array<std::uint8_t, 2>               capture_filtered{};
  std::array<std::uint8_t, 2>               capture_pending{};
  std::array<std::uint8_t, 2>               capture_divided{};
  std::array<std::uint64_t, 2>              capture_filter_due{};
  std::array<std::uint64_t, 2>              capture_edge_ps{};
  std::array<std::vector<std::uint64_t>, 2> captures{};
  std::array<std::uint64_t, 2>              capture_last_ps{};
  std::array<std::uint64_t, 2>              capture_off_ps{};
  std::array<std::uint8_t, 2>               overcapture_lost{};
  std::uint64_t                             source_ps{};
  std::uint8_t                              source_level{};
  std::uint8_t                              source_live{};

  friend auto operator==(lptim_v2_state const&, lptim_v2_state const&) -> bool = default;
};

/**
 * @brief LPTIM of the STM32U5, RM0456 section 58, clocked by RCC's `lptim1` to `lptim4` domain
 *        from CCIPR1 and CCIPR3. The 16-bit counter is derived from the kernel clock, the
 *        prescaler PRESC and the virtual time of its last anchor, never ticked. CNTSTRT and
 *        SNGSTRT written with ENABLE already 1 start it from 0 at the later of the write plus 3
 *        kernel clocks, RM0456 58.4.7, and ENABLE plus two prescaled clocks, RM0456 58.4.13, the
 *        prescaled reading inferred. A write setting them with ENABLE 0 is discarded, RM0456
 *        58.4.7, also when the same write sets ENABLE, inferred. They read 0 at once, inferred.
 *        The counter counts 0 to ARR and overflows to 0. The repetition counter, loaded with
 *        REP at the start, inferred, counts the overflows down, and an overflow with it at 0 is
 *        the update event: it transfers the preloaded values and reloads REP, RM0456 58.4.16.
 *        One-shot mode stops at 0 at the update event, RM0456 58.4.8. CC1IF and ARRM set one
 *        counter cycle after the counter reaches CCR1 and ARR, so CC1IF reads CNT CCR1 + 1 and ARRM
 *        reads CNT 0, RM0456 58.4.19. A counter cycle is one prescaled tick. UE sets one counter
 *        cycle after the update event, the instant inferred from the same note, which lists the
 *        update event flag. CCR1 above ARR never matches, CCR1 equal to ARR sets CC1IF with ARRM,
 *        and a counter above a lowered ARR runs to 0xFFFF before it overflows, inferred. A CCR1
 *        value the counter holds at the instant the value takes effect matches one counter cycle
 *        later, a smaller one in the next period, inferred. With the kernel clock counting, a
 *        match flag in flight, its match at or before the instant its value takes effect and
 *        its flag after, sets at that instant, inferred. COUNTRST and a RSTARE read lose a flag
 *        in flight. A CCR1 or ARR write with ENABLE 1 sets CMP1OK or ARROK 3 kernel clocks
 *        later, inferred: RM0456 58.4.11 states a latency with no number. With PRELOAD 0, or
 *        with the counter stopped, the value takes effect at the flag, and counter ticks up to
 *        it compare with the old values, inferred.
 *        With PRELOAD 1 and the counter started, from the CNTSTRT or SNGSTRT write on, inferred, it
 *        takes effect at the first update event at or after the flag, RM0456 58.4.11. ENABLE
 *        written to 0 and the one-shot stop apply such a value at once, inferred. An RCR write
 *        with ENABLE 1 sets REPOK 4 APB clocks plus 3 kernel clocks later, RM0456 58.4.16, the
 *        APB clock taken at the write, inferred. The repetition counter takes the new REP at the
 *        first update event at or after REPOK, PRELOAD 0 or 1, RM0456 58.4.16, inferred
 *        for PRELOAD 0, where RM0456 58.4.11 states an immediate RCR update. A flag due
 *        at the instant of an update event applies before it: the update and a one-shot
 *        stop use the old value and the new one counts from then, and a REPOK at that
 *        instant reloads the new REP, inferred. An ARR write clears UE, RM0456 58.7.2.
 *        A DIER write with ENABLE 1 sets DIEROK, and takes effect, 3 kernel clocks
 *        later, inferred: RM0456 58.7.8 states the flag with no latency. Each DIER bit
 *        the write changes then clears its ISR flag, ES0499 2.17.3, at that instant,
 *        inferred. Terms due at the same time apply in the order CC1IF, ARRM, CMP1OK, ARROK,
 *        DIEROK, REPOK, COUNTRST, UE, update event, mode switch, inferred. A match flag due with a
 *        value that takes effect at the same instant belongs to the old value.
 *        COUNTRST written with ENABLE 1 resets CNT 3 kernel clocks later and reads 1 until then,
 *        RM0456 58.4.14 and 58.7.11, the count restarting at that instant with the update event
 *        due then dropped, inferred. A COUNTRST write with ENABLE 0 is discarded, RM0456 58.7.11.
 *        With RSTARE 1 a CPU or DMA read of CNT returns the count and resets CNT, RM0456 58.4.14,
 *        the count restarting at the read, inferred. A debugger read leaves CNT, inferred.
 *        RSTARE written with ENABLE 0 is discarded, RM0456 58.7.11.
 *        A flag that sets while its DIER bit is in effect arms the LPTIM IRQ until ICR clears
 *        it, so a DIER bit set after its flag raises nothing, RM0456 58.6. ENABLE written to 0
 *        stops the counter at 0, keeps the flags and a pending CCR1, ARR, RCR or DIER write and
 *        drops a pending COUNTRST and the repetition count, inferred.
 *        RM0456 58.7.10 allows CFGR writes only with ENABLE 0, and 58.7.8, 58.7.12, 58.7.13 and
 *        58.7.16 DIER, CCR1, ARR and RCR writes only with ENABLE 1, and RM0456 58.4.11, 58.4.16
 *        and 58.7.8 a second CCR1, ARR, RCR or DIER write only after its OK flag. The model keeps
 *        the register for a write outside its rule, inferred, the second write excepted, whose
 *        value replaces the pending one outside strict_peripherals, inferred: each logs one
 *        model_warning and raises model_error under strict_peripherals.
 *        SNGSTRT written while the counter runs in continuous mode makes it stop at a later
 *        update event, and CNTSTRT written in one-shot mode makes it go on counting, RM0456
 *        58.4.8. The new mode reaches the counter 3 kernel clocks after the write, inferred, so
 *        an update event before then keeps the old mode, and a CNTSTRT reaching a counter that
 *        the one-shot stop stopped starts it. The same mode written again while the counter runs
 *        is discarded, inferred. A second CNTSTRT or SNGSTRT before the first reaches the counter
 *        replaces it, logs one model_warning and raises model_error under strict_peripherals,
 *        inferred.
 *        A kernel rate change keeps CNT, the kernel clocks already spent in the current tick or
 *        the start latency, and the kernel clocks left before each pending OK flag, COUNTRST and
 *        mode switch, inferred. A stopped kernel clock freezes all of them until it runs again,
 *        inferred. LPTIMxRST resets the registers and the state and withdraws the pending event.
 *        The output of channel 1, or of LPTIM4, is the trigger signal `lptim<n>_ch1`, or
 *        `lptim4_out`, and drives no pin. OC1REF is high while CNT is above CCR1, so the output
 *        rises at CNT CCR1 + 1 with CC1IF, CCR1 0 at CNT 1, and falls at CNT 0 with ARRM, RM0456
 *        58.4.19 and Figures 747 and 748. A signal edge carries the whole ps at or after its
 *        instant, so a kernel clock that is not a whole ps puts it up to 1 ps late. CCR1 at or
 *        above ARR keeps it inactive, RM0456 58.4.10 requires ARR above CCR1. A CCR1 value that
 *        takes effect moves the level at its CMP1OK, inferred. A one-pulse run gives one pulse.
 *        With WAVE 1 it rises once and keeps its level until ENABLE is written to 0, and a CNTSTRT
 *        or SNGSTRT after the first start is discarded until then, the software restart inferred.
 *        The level is OC1REF xor CC1P bit 0 with CC1E 1 and CC1SEL 0, 0 otherwise, inferred, and
 *        WAVPOL on LPTIM4. A polarity or CC1E change takes effect at the register write, also
 *        with ENABLE 0, RM0456 58.4.10.
 *        ENABLE written to 0 and LPTIMxRST drop the level to inactive at the write, inferred. A
 *        sink that starts listening sees the edges from then on. Channel 2 of a channel
 *        instance compares CCR2 as channel 1 compares CCR1: CC2IF, CMP2OK, the level of the
 *        trigger signal `lptim<n>_ch2` with CC2P bit 0 and CC2E, and PRELOAD 1 at the update
 *        event follow channel 1, RM0456 58.4.19 and 58.7.18. CC2IF sets with CC2SEL 0, also with
 *        CC2E 0, RM0456 58.7.2, so the reset CCR2 0 sets it at CNT 1 of every run. WAVE 1
 *        latches each channel on its own, also with its CCxE 0, inferred.
 *        With CCxSEL 1 on LPTIM1 to LPTIM3 channel x captures, RM0456 58.3 Table 599 and 58.4.18.
 *        Its input is the pin signal `lptim<n>_chx` with CFGR2 ICxSEL 00, RM0456 58.4.3 Tables 607
 *        to 610. IC2SEL 01 to 11 select the internal clocks of RM0456 Tables 608 to 610, LSI and
 *        LSE on LPTIM1, HSI16/256, MSIS/1024 and MSIS/4 on LPTIM2: the input toggles every half
 *        period of the divided rate, rounded up to a whole ps, from its level when the channel
 *        starts to capture, the phase inferred, and a rate change takes effect from the last
 *        toggle, inferred. The LPTIM2 internal clocks are the RCC domains `timic_hsi`, `timic_csi`,
 *        `timic_msi_1024` and `timic_msi_4`, at 0 Hz while RCC TIMICSEL is closed, RM0456 11.8.46
 *        and RM0481 11.8.40, each the undivided oscillator rate with the divider in the row: with
 *        no clock the input stays low, no capture happens and nothing is logged. The U5 taps
 *        follow MSIK or MSIS by the TIMICSEL codes 100 to 111 of RM0456 11.8.46, TIMICSEL 001 to
 *        011 being closed, inferred from the register text.
 *        With ICxF not 00 a level change counts once it has held for 2, 4 or 8 clocks of the RCC
 *        kernel clock, a shorter pulse rejected, RM0456 58.7.17, the sampling phase taken as the
 *        change, inferred. ICxF applies per channel as CCMR1 holds it, inferred, where RM0456
 *        58.4.5 sets the capture filters as one group. CCxP selects the rising, the falling or both
 *        edges and ICxPSC captures every 1, 2, 4 or 8 edges, RM0456 58.7.17, the prescaler counting
 *        only with CCxE 1 and reset by CCxE written to 0, inferred. With CCxE 1 a capture latches
 *        CNT into CCRx, RM0456 58.4.18, the RM0456 Table 613 latency in counter steps after the
 *        counter clock of the edge, so CCRx minus the offset is the count at the edge, and CCxIF
 *        sets at the latch, inferred. A capture while CCxIF is set also sets CCxOF, RM0456 58.4.18,
 *        and CCxOIE arms the IRQ. ES0499 2.17.4: a CCxOCF write from one counter clock before a
 *        capture up to the capture leaves CCxOF unset at every later over-capture, the window
 *        inferred, even when CCxOF is clear at the write. With no counter tick, an external clock
 *        or a stopped kernel clock, the window shrinks to the capture instant itself, inferred.
 *        ES0499 2.17.4 and ES0565 2.11.4 list silicon revision A only, the fault is reproduced on
 *        every revision. The fault stays until CCxE is written to 0 and back to 1 at least PRESC x
 *        3 kernel clocks later, the erratum's workaround, or ENABLE is written to 0, inferred.
 *        With the counter stopped the capture comes the same latency on the prescaled kernel
 *        clock counted from ENABLE and latches the stopped CNT, inferred. Captures in flight apply
 *        in the order of their instants, and edges whose captures fall on the same counter
 *        clock give one capture, inferred. A capture in flight is dropped by CCxE or CCxSEL written
 *        to 0 and by ENABLE written to 0, and keeps its instant across a kernel rate change,
 *        inferred. A CPU or DMA read of CCRx clears CCxIF, RM0456 58.7.3, a debugger read leaves
 *        it, inferred. CCRx is read-only, RM0456 58.7.12 and 58.7.18: a write is discarded and
 *        sets no CMPxOK, inferred. A capture channel compares nothing and drives its signal low. A
 *        CFGR2 write that changes ICxSEL counts the level change it makes as an edge, inferred.
 *        With the kernel clock stopped and CKSEL 0 an edge captures nothing, inferred.
 *        LPTIMxRST takes the capture input levels, inferred. CCxP 10 on a capture channel logs one
 *        model_warning at a start and at a CCMR1 write while the counter runs and raises
 *        model_error under strict_peripherals, RM0456 58.7.17 reserving it: both edges then
 *        capture. A CCMR1 write that changes CCxSEL, ICxF, CCxP or ICxPSC of a capture channel
 *        with CCxE 1 logs one model_warning and raises model_error under strict_peripherals,
 *        RM0456 58.7.17, the new setting taking effect at the write. Two CCMR1 writes closer
 *        than PRESC x 3 kernel clocks both apply at once, where RM0456 58.7.17 calls the result
 *        unpredictable.
 *        With CKSEL 1 or COUNTMODE 1 the counter counts the active edges CKPOL selects on input 1,
 *        rising, falling or both, instead of the kernel clock, RM0456 58.4.12 and 58.7.10. Input 1
 *        is the pin signal `lptim<n>_in1` with CFGR2 IN1SEL 00, RM0456 58.4.3 Table 605, and a
 *        CFGR2 write that changes IN1SEL counts the level change it makes as an edge, inferred.
 *        With CKSEL 1 the active edges clock the kernel logic, RM0456 58.4.12, so the latencies
 *        stated in kernel or counter clocks count edges: ENABLE 2 edges, RM0456 58.4.13, COUNTRST 3
 *        edges, RM0456 58.4.14, and REPOK 3 edges, RM0456 58.4.16, with its 4 APB clocks dropped,
 *        inferred. The start, CMP1OK, CMP2OK, ARROK, DIEROK and a mode switch count 3 edges,
 *        inferred, which with the ENABLE latency gives the five lost edges of RM0456 58.4.12 after
 *        an ENABLE and a CNTSTRT. PRESC divides the edges after the start, RM0456 58.4.6, the phase
 *        inferred. An edge counts first, then the terms it completes apply, inferred, so a REPOK at
 *        an update edge reloads the old REP. With COUNTMODE 1 every active edge counts at its
 *        instant from the start instant on, the sampling on the kernel clock taken as the edge
 *        itself, an edge with the kernel clock stopped is dropped and a term due at the instant of
 *        an edge applies before it, inferred. A counted edge moves CNT by one. CC1IF, CC2IF and
 *        ARRM set at the edge that leaves CCR1, CCR2 and ARR, and the output follows CNT, RM0456
 *        58.4.19 read with one counted edge as the counter cycle, inferred. The edge that leaves
 *        ARR or 0xFFFF is the overflow, and with the repetition counter at 0 the update event. UE
 *        then sets at the next counted edge, or at once when the counter stops, inferred. With
 *        CKFLT not 00 a level change of input 1 counts once it has held for 2, 4 or 8 clocks of the
 *        RCC kernel clock, and a shorter pulse is rejected, RM0456 58.4.5 and 58.7.10, the sampling
 *        phase taken as the change and the rate read at the change, inferred. With that clock
 *        stopped the filter passes no change and the level it leaves becomes the filtered level,
 *        inferred. LPTIMxRST takes the input 1 level, inferred. A start with PRESC not 0 and
 *        COUNTMODE 1, CKPOL 11, CKPOL 10 with CKSEL 1, or CKFLT not 00 with the kernel clock
 *        stopped logs one model_warning each and raises model_error under strict_peripherals,
 *        RM0456 58.4.5, 58.4.12 and 58.7.10: every edge then counts, both edges for the two CKPOL
 *        cases, inferred.
 *        With TRIGEN not 00 CNTSTRT and SNGSTRT arm the counter and choose its mode, and the edge
 *        TRIGEN selects on the trigger input TRIGSEL selects starts it, RM0456 58.4.7 and 58.7.10.
 *        TRIGSEL 0 is the pin signal `lptim<n>_etr`, RM0456 58.4.3 Table 604, and a CNTSTRT or
 *        SNGSTRT written again before the trigger re-arms in its mode, inferred. EXTTRIG sets at
 *        the trigger, RM0456 58.7.2, and the counter starts two counter clocks later, RM0456
 *        58.4.7: two prescaled kernel clocks, or with CKSEL 1 two prescaled input 1 edges,
 *        inferred. With TRGFLT 01, 10 or 11 a trigger level change counts once it has held for 2, 4
 *        or 8 clocks of the RCC kernel clock, and a shorter pulse is rejected, RM0456 58.4.5 and
 *        58.7.10, the sampling phase taken as the change, inferred. A trigger with the kernel clock
 *        stopped, and CKSEL 0, starts nothing and sets no EXTTRIG, inferred. A trigger before
 *        CNTSTRT or SNGSTRT starts nothing and sets no EXTTRIG, inferred. A trigger while the
 *        counter runs is ignored and sets no EXTTRIG, RM0456 58.4.7 and 58.7.2, unless TIMOUT is 1:
 *        it then sets EXTTRIG and restarts the counter and the repetition counter from 0 two
 *        counter clocks later, RM0456 58.4.9 and 58.7.10, the latency inferred, and a match flag in
 *        flight is lost. In one-shot mode each trigger after the update event starts a new run,
 *        RM0456 58.4.8, and with WAVE 1 only the first trigger starts one, a later one setting
 *        no EXTTRIG, inferred. LPTIMxRST takes the trigger level, inferred. TRGFLT not 00 with the
 *        kernel clock stopped logs one model_warning at the arm and raises model_error under
 *        strict_peripherals, RM0456 58.4.5.
 *        With ENC 1 and CKSEL 0, RM0456 58.4.15, the counter counts the active edges CKPOL selects
 *        on input 1 and input 2, the pin signals `lptim<n>_in1` and `lptim<n>_in2` with CFGR2
 *        IN1SEL and IN2SEL 00, RM0456 58.4.3 Tables 605 and 606, up or down by the level of the
 *        other input, RM0456 58.4.15 Table 612, between 0 and ARR: counting down from 0 gives ARR.
 *        CKFLT filters both inputs, RM0456 58.4.5, and the direction takes the filtered level of
 *        the other input, inferred. A change of direction sets DOWN or UP at the edge, RM0456
 *        58.4.15, and the count starts upwards, inferred. CC1IF, CC2IF and ARRM set at the counted
 *        edge that leaves CCR1, CCR2 and ARR in either direction, as with COUNTMODE 1, inferred. A
 *        down wrap from 0 loads ARR, RM0456 58.4.15, and is an overflow: the repetition counter,
 *        the update event, PRELOAD and the one-shot stop apply there as at an up wrap, inferred.
 *        ENC takes precedence over COUNTMODE, inferred. An edge with the kernel clock stopped is
 *        dropped, inferred. The inputs never switch at the same time, RM0456 58.4.15. A CFGR2 write
 *        that changes IN2SEL counts the level change it makes as an edge, inferred. On LPTIM3 and
 *        LPTIM4, whose IN2SEL bits are reserved, RM0456 58.3 Table 599, a CFGR2 write stores them
 *        with no effect. SNGSTRT with ENC 1 logs one model_warning and raises
 *        model_error under strict_peripherals, RM0456 58.4.15 asking for continuous mode, and the
 *        one-shot stop then applies at a wrap. ENC 1 with CKSEL 1 logs one model_warning at a
 *        start and raises under strict_peripherals, RM0456 58.4.15: input 1 then clocks the
 *        counter.
 *        The functions below are logged as not modelled: each logs one model_warning at its first
 *        use and raises model_error under strict_peripherals at every use, after the write has
 *        taken the effect stated. At a counter start and at a CFGR2 write while it runs: IN1SEL not
 *        00 with CKSEL 1 or COUNTMODE 1, IN2SEL not 00 with ENC 1, IC1SEL not 00 and a reserved
 *        IC2SEL input on a capture channel, the input then reading 0. At a counter start and at a
 *        CCMR1 write while it runs: CC1SEL or CC2SEL 1 with CKSEL 1, COUNTMODE 1 or ENC 1, a
 *        capture then latching CNT at the filtered edge. At a CNTSTRT or SNGSTRT write with TRIGEN
 *        not 00: TRIGSEL not 0, an input of RM0456 Table 604 no model produces, which leaves the
 *        counter stopped.
 *        DMA requests, RM0456 58.4.21: with UEDE 1 a UE flag sets `lptim<n>_ue_dma` at each update
 *        event, with CC1DE or CC2DE 1 a capture sets `lptim<n>_ic1_dma` or `lptim<n>_ic2_dma` with
 *        its CCxIF. A request stays high until a controller burst acknowledges it, and an event
 *        before the ack is absorbed, inferred. A request raises only with its DIER enable in
 *        effect, at DIEROK. Writing 0 to UEDE, CC1DE or CC2DE resets its request at the write,
 *        RM0456 58.7.8. The ic request also drops with CCxE 0 or ENABLE 0, the ue request with
 *        ENABLE 0 or every CCxE 0, RM0456 58.4.21, the instant inferred. The ue request raises
 *        only with a CCxE set, following the RM0456 58.4.21 Note literally, inferred. A CPU read
 *        of CCRx clears CCxIF and leaves the request, inferred.
 *        LPTIMxRST drops a raised request, inferred. LPTIM4 has no channel and no DMA request,
 *        RM0456 58.3 Table 599, and its UEDE bit is reserved.
 *        While the machine holds its core with DBG_LPTIM<n>_STOP set, RM0456 58.4.22, the model
 *        freezes the instance as a stopped kernel clock: CNT, the kernel clocks spent in the
 *        current tick and those left before each pending term are kept and resume at the release,
 *        the latencies inferred. A capture in flight keeps its instant and latches the frozen
 *        CNT there, as with a stopped kernel clock, inferred. An edge on a pin input, a trigger
 *        or a capture input during the hold is dropped, and the glitch filters keep sampling,
 *        inferred. A kernel rate change during the hold applies from the release.
 *        The hold is kept across LPTIMxRST. At a machine reset the LPTIM reads DBG_LPTIM<n>_STOP
 *        again, which a system reset keeps and a power-on reset clears.
 *        Deviation: CNT reads exact, without the double-read rule of RM0456 58.7.14.
 */
class lptim_v2 final : public dispatch_model<lptim_v2, lptim_v2_state>,
                       public clock_listener,
                       public bus_reset_listener,
                       public core_hold_listener,
                       public dma_requester,
                       public trigger_source,
                       public trigger_sink
{
public:
  lptim_v2(model_context& ctx, peripheral_desc const& desc, lptim_v2_variant const& variant);

  void               reset(register_file& regs, reset_kind kind) override;
  void               load(std::span<std::byte const> in) override;
  void               on_bus_reset() override;
  void               on_core_hold(bool held) override;
  void               transfer_completed(dma_controller const& by, std::uint32_t request) override;
  void               acknowledged(dma_controller const& by, std::uint32_t request) override;
  void               interest_changed() override;
  [[nodiscard]] auto produces(std::string_view signal) const -> bool override;
  [[nodiscard]] auto level(std::string_view signal) const -> std::optional<bool> override;
  [[nodiscard]] auto listens(std::string_view signal) const -> bool override;
  [[nodiscard]] auto can_listen(std::string_view signal) const -> bool override;
  [[nodiscard]] auto takes_pin(std::string_view signal) const -> bool override;
  void triggered(std::string_view signal, trigger_edge edge, trigger_time at) override;
  void on_clock_change() override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
      -> std::expected<std::uint32_t, bus_error> override;
  [[nodiscard]] auto storage_only(std::uint32_t offset) const -> bool override;

  void                       plain_written(std::uint32_t old, std::uint32_t now);
  void                       clear_written(std::uint32_t old, std::uint32_t now);
  void                       interrupts_written(std::uint32_t old, std::uint32_t now);
  void                       configuration_written(std::uint32_t old, std::uint32_t now);
  void                       control_written(std::uint32_t old, std::uint32_t now);
  void                       channels_written(std::uint32_t old, std::uint32_t now);
  void                       inputs_written(std::uint32_t old, std::uint32_t now);
  void                       repetition_written(std::uint32_t old, std::uint32_t now);
  template <term Which> void value_written(std::uint32_t old, std::uint32_t now);

private:
  struct due_term
  {
    std::uint64_t due;
    term          which;
  };

  struct value_slot
  {
    std::uint32_t& value;
    std::uint32_t& pending;
    std::uint32_t& preload;
    std::uint64_t& due;
    std::uint32_t  flag;
    std::uint8_t   bit;
  };

  struct filter_line
  {
    std::uint8_t&  level;
    std::uint8_t&  filtered;
    std::uint8_t&  pending;
    std::uint64_t& due;
  };

  [[nodiscard]] auto slot(term which) -> value_slot;

  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto enabled() const -> bool;
  [[nodiscard]] auto anchor() const -> wide;
  [[nodiscard]] auto tick_unit() const -> wide;
  [[nodiscard]] auto first_wrap() const -> std::uint64_t;
  [[nodiscard]] auto position(std::uint64_t ticks) const -> std::uint32_t;
  [[nodiscard]] auto ticks_at(std::uint64_t ps) const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto count_at(std::uint64_t ps) const -> std::uint32_t;
  [[nodiscard]] auto ceil_ps(wide at) const -> std::uint64_t;
  [[nodiscard]] auto ticks_to(std::uint32_t value) const -> std::uint64_t;
  [[nodiscard]] auto match_due(std::uint32_t value) const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto next_term() const -> std::optional<due_term>;
  void               offer_captures(std::optional<due_term>& best) const;
  [[nodiscard]] auto in_flight(std::uint32_t value, std::uint64_t ps) const -> bool;
  [[nodiscard]] auto kernel_due(std::uint64_t ps, std::uint64_t cycles) const -> std::uint64_t;
  [[nodiscard]] auto repetition_ok_due(std::uint64_t ps) const -> std::uint64_t;
  [[nodiscard]] auto period() const -> std::uint64_t;
  [[nodiscard]] auto overflows(std::uint64_t ticks) const -> std::uint64_t;
  [[nodiscard]] auto repetition_after(std::uint64_t count) const -> std::uint32_t;
  [[nodiscard]] auto update_ticks() const -> std::uint64_t;
  [[nodiscard]] auto update_due() const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto preload() const -> bool;
  [[nodiscard]] auto wave() const -> bool;
  [[nodiscard]] auto outputs() const -> std::size_t;
  [[nodiscard]] auto compare_of(std::size_t channel) const -> std::uint32_t;
  [[nodiscard]] auto raw_at(std::uint64_t ps, std::size_t channel) const -> bool;
  [[nodiscard]] auto drive(bool raw, std::size_t channel) const -> bool;
  [[nodiscard]] auto out_due() const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto external() const -> bool;
  [[nodiscard]] auto clocked_by_input() const -> bool;
  [[nodiscard]] auto kernel_hz() const -> std::uint64_t;
  [[nodiscard]] auto input_now(bool second = false) const -> bool;
  [[nodiscard]] auto encoder() const -> bool;
  [[nodiscard]] auto selects_unmodelled_input() const -> bool;
  [[nodiscard]] auto line_of(bool second) -> filter_line;
  [[nodiscard]] auto active(bool rising) const -> bool;
  [[nodiscard]] auto capturing(std::size_t channel) const -> bool;
  [[nodiscard]] auto capture_input_now(std::size_t channel) const -> bool;
  [[nodiscard]] auto capture_line(std::size_t channel) -> filter_line;
  [[nodiscard]] auto capture_due(std::uint64_t edge_ps, std::size_t channel) const -> std::uint64_t;

  void               set_anchor(wide at);
  void               reanchor(std::uint64_t ps, std::uint64_t ticks);
  [[nodiscard]] auto settle_to(std::uint64_t ps) -> bool;
  void               catch_up(std::uint64_t ps);
  void               apply(due_term const& next);
  void               raise_flag(std::uint32_t flag);
  void               start(std::uint64_t ps, bool single);
  void               begin(wide at, std::uint64_t ps, bool single);
  void               switch_mode(std::uint64_t ps, bool single);
  void               stop();
  void               transfer_preloaded();
  void               restart_count(std::uint64_t ps);
  void               unmodelled(use which, std::string_view reg);
  void               refuse_at_start();
  void               refuse_channels();
  void               sync_out(std::uint64_t ps);
  void               refresh();
  void               raise_request(std::size_t request);
  void               drop_request(std::size_t request);
  void               sync_requests();
  [[nodiscard]] auto request_wanted(std::size_t request) const -> bool;
  [[nodiscard]] auto wants_update_events() const -> bool;
  void               reschedule();
  void               rebase(std::uint64_t ps, std::uint64_t hz);
  void               restore_reset_values();
  void               take_input_level();
  void               input_changed(std::uint64_t ps, bool level, bool second = false);
  void               input_edge(std::uint64_t ps, bool rising, bool second = false);
  void               encoder_edge(bool second, bool rising);
  void               clock_edge(std::uint64_t ps);
  void               count_edge(bool up = true);
  void               refuse_clock();
  [[nodiscard]] auto
      filtered_change(filter_line line, std::uint64_t ps, bool level, std::uint32_t code, bool live)
          -> bool;
  void               trigger_changed(std::uint64_t ps, bool level);
  void               trigger_fired(std::uint64_t ps, bool rising);
  void               restart_on_trigger(std::uint64_t ps, bool single);
  void               arm(bool single);
  void               capture_changed(std::size_t channel, std::uint64_t ps, bool level);
  void               capture_edge(std::size_t channel, std::uint64_t ps, bool rising);
  void               apply_capture(std::size_t channel, std::uint64_t due);
  void               drop_captures(std::size_t channel);
  void               sync_source(std::uint64_t ps);
  [[nodiscard]] auto source_due() const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto source_hz() const -> std::uint64_t;
  [[nodiscard]] auto source_wanted() const -> bool;
  [[nodiscard]] auto in_capture_pulse(std::size_t channel, std::uint64_t ps) const -> bool;
  void               overcapture_cleared(std::size_t channel, std::uint64_t ps);
  void               capture_enable_changed(std::size_t channel, bool on, std::uint64_t ps);

  lptim_v2_variant                          variant_;
  register_file&                            regs_;
  reg_ref                                   isr_;
  reg_ref                                   icr_;
  reg_ref                                   dier_;
  reg_ref                                   cfgr_;
  reg_ref                                   cr_;
  reg_ref                                   ccr1_;
  reg_ref                                   ccr2_;
  reg_ref                                   arr_;
  reg_ref                                   cnt_;
  reg_ref                                   inputs_;
  reg_ref                                   rcr_;
  field_ref                                 prescaler_;
  field_ref                                 clock_select_;
  field_ref                                 count_mode_;
  field_ref                                 clock_polarity_;
  field_ref                                 clock_filter_;
  field_ref                                 encoder_;
  field_ref                                 preload_;
  field_ref                                 wave_;
  field_ref                                 polarity_;
  std::array<field_ref, 2>                  channel_polarity_{};
  field_ref                                 trigger_enable_;
  field_ref                                 trigger_select_;
  field_ref                                 trigger_filter_;
  field_ref                                 timeout_;
  std::array<field_ref, 2>                  channel_enable_{};
  std::array<field_ref, 2>                  channel_select_{};
  std::array<field_ref, 2>                  capture_filter_{};
  std::array<field_ref, 2>                  capture_prescaler_{};
  std::array<std::string, 2>                capture_inputs_{};
  std::array<core::clock_domain, 3>         source_domains_{};
  std::array<bool, 2>                       capture_polarity_logged_{};
  std::array<bool, 2>                       capture_setup_logged_{};
  core::clock_domain                        kernel_;
  core::clock_domain                        bus_;
  irq_line*                                 line_ = nullptr;
  std::vector<trigger_output>               out_;
  std::array<std::optional<bool>, 2>        listened_{};
  std::array<bool, unmodelled_table.size()> unmodelled_logged_{};
  std::array<bool, 6>                       locked_logged_{};
  std::array<bool, 5>                       pending_logged_{};
  std::array<bool, 6>                       clock_logged_{};
  std::array<dma_signal, 3>                 requests_;
  bool                                      freeze_logged_ = false;
  std::string                               in1_;
  std::string                               in2_;
  std::string                               etr_;
  bool                                      switch_logged_  = false;
  bool                                      trigger_logged_ = false;
  bool                                      single_logged_  = false;
};

lptim_v2::lptim_v2(model_context& ctx, peripheral_desc const& desc, lptim_v2_variant const& variant)
    : dispatch_model{
          ctx,
          desc,
          "LPTIM",
          {}
},
      variant_{variant}, regs_{ctx.regs}, isr_{reg(variant.status)}, icr_{reg(variant.clear)},
      dier_{reg(variant.interrupts)}, cfgr_{reg("CFGR")}, cr_{reg("CR")}, ccr1_{reg("CCR1")},
      arr_{reg("ARR")}, cnt_{reg("CNT")}, inputs_{reg("CFGR2")}, rcr_{reg("RCR")},
      prescaler_{field("CFGR", "PRESC")}, clock_select_{field("CFGR", "CKSEL")},
      count_mode_{field("CFGR", "COUNTMODE")}, clock_polarity_{field("CFGR", "CKPOL")},
      clock_filter_{field("CFGR", "CKFLT")},
      encoder_{variant.encoder ? field("CFGR", "ENC") : field_ref{}},
      preload_{field("CFGR", "PRELOAD")}, wave_{field("CFGR", "WAVE")},
      polarity_{variant.channels == 0 ? field("CFGR", "WAVPOL") : field_ref{}},
      trigger_enable_{field("CFGR", "TRIGEN")}, trigger_select_{field("CFGR", "TRIGSEL")},
      trigger_filter_{field("CFGR", "TRGFLT")}, timeout_{field("CFGR", "TIMOUT")},
      kernel_{ctx.clock.domain(lower(desc.name))}, bus_{ctx.clock.domain(variant.bus_clock)},
      line_{&irq(desc.name + "_IRQn")},
      requests_{
          dma_signal{ctx.peers, lower(desc.name) + "_ic1_dma"},
          dma_signal{ctx.peers, lower(desc.name) + "_ic2_dma"},
          dma_signal{ctx.peers, lower(desc.name) + "_ue_dma"}},
      in1_{lower(desc.name) + "_in1"},
      in2_{variant.encoder ? lower(desc.name) + "_in2" : std::string{}},
      etr_{lower(desc.name) + "_etr"}
{
  if(variant.channels == 0)
  {
    out_.emplace_back(ctx.peers, lower(desc.name) + "_out");
  }
  for(std::size_t channel = 0; channel < variant.channels; ++channel)
  {
    out_.emplace_back(ctx.peers, std::format("{}_ch{}", lower(desc.name), channel + 1));
  }
  auto const writes = std::to_array<write_entry<lptim_v2>>({
      {variant.status,     &lptim_v2::plain_written                       },
      {variant.clear,      &lptim_v2::clear_written                       },
      {variant.interrupts, &lptim_v2::interrupts_written                  },
      {"CFGR",             &lptim_v2::configuration_written               },
      {"CR",               &lptim_v2::control_written                     },
      {"CCR1",             &lptim_v2::value_written<term::compare_written>},
      {"ARR",              &lptim_v2::value_written<term::reload_written> },
      {"CNT",              &lptim_v2::plain_written                       },
      {"CFGR2",            &lptim_v2::inputs_written                      },
      {"RCR",              &lptim_v2::repetition_written                  },
  });
  bind(writes);
  if(variant.encoder)
  {
    expect_field(variant.status, "UP", true);
    expect_field(variant.status, "DOWN", true);
    expect_field(variant.interrupts, "UPIE", true);
    expect_field(variant.interrupts, "DOWNIE", true);
    expect_field(variant.clear, "UPCF", true);
    expect_field(variant.clear, "DOWNCF", true);
  }
  if(variant.channels != 0)
  {
    auto const channel_writes = std::to_array<write_entry<lptim_v2>>({
        {"CCMR1", &lptim_v2::channels_written                     },
        {"CCR2",  &lptim_v2::value_written<term::compare2_written>},
    });
    bind(channel_writes);
    channel_enable_   = {field("CCMR1", "CC1E"), field("CCMR1", "CC2E")};
    channel_select_   = {field("CCMR1", "CC1SEL"), field("CCMR1", "CC2SEL")};
    channel_polarity_ = {field("CCMR1", "CC1P"), field("CCMR1", "CC2P")};
    ccr2_             = reg("CCR2");
  }
  if(variant.channels != 0 and variant.capture)
  {
    capture_filter_    = {field("CCMR1", "IC1F"), field("CCMR1", "IC2F")};
    capture_prescaler_ = {field("CCMR1", "IC1PSC"), field("CCMR1", "IC2PSC")};
    capture_inputs_    = {lower(desc.name) + "_ch1", lower(desc.name) + "_ch2"};
    for(std::size_t select = 1; select <= 3; ++select)
    {
      if(auto const input = variant.capture2_inputs[select]; not input.domain.empty())
      {
        source_domains_[select - 1] = ctx.clock.domain(input.domain);
      }
    }
  }
  state_.reload = describe("ARR").reset;
  state_.hz     = ctx.clock.hz(kernel_);
}

auto lptim_v2::now_ps() const -> std::uint64_t
{
  return ctx_.clock.time().ps;
}

auto lptim_v2::enabled() const -> bool
{
  return (regs_.peek(cr_.offset) & enable_bit) != 0;
}

auto lptim_v2::external() const -> bool
{
  return get(regs_, clock_select_) != 0 or get(regs_, count_mode_) != 0 or encoder();
}

auto lptim_v2::encoder() const -> bool
{
  return variant_.encoder and get(regs_, encoder_) != 0 and get(regs_, clock_select_) == 0;
}

auto lptim_v2::clocked_by_input() const -> bool
{
  return get(regs_, clock_select_) != 0;
}

auto lptim_v2::kernel_hz() const -> std::uint64_t
{
  return state_.held != 0 or clocked_by_input() ? 0 : ctx_.clock.hz(kernel_);
}

auto lptim_v2::input_now(bool second) const -> bool
{
  auto const& signal = second ? in2_ : in1_;
  return (regs_.peek(inputs_.offset) & (second ? input2_select : input1_select)) == 0
     and not signal.empty()
     and std::ranges::any_of(ctx_.peers.all(), [&](model const* peer) {
           auto const* const port = dynamic_cast<gpio_port const*>(peer);
           return port != nullptr and port->signal_level(signal).value_or(false);
         });
}

auto lptim_v2::selects_unmodelled_input() const -> bool
{
  auto const select = regs_.peek(inputs_.offset);
  return (external() and (select & input1_select) != 0)
      or (encoder() and (select & input2_select) != 0)
      or (capturing(0) and (select & capture1_select) != 0)
      or (capturing(1)
          and (select & capture2_select) != 0
          and variant_.capture2_inputs[(select & capture2_select) >> 20].domain.empty());
}

auto lptim_v2::line_of(bool second) -> filter_line
{
  if(second)
  {
    return {state_.input2_level, state_.input2_filtered, state_.input2_pending, state_.input2_due};
  }
  return {state_.input_level, state_.input_filtered, state_.input_pending, state_.input_due};
}

auto lptim_v2::active(bool rising) const -> bool
{
  auto const polarity = get(regs_, clock_polarity_);
  return not((polarity == 0 and not rising) or (polarity == 1 and rising));
}

auto lptim_v2::capturing(std::size_t channel) const -> bool
{
  return variant_.capture and variant_.channels != 0 and get(regs_, channel_select_[channel]) != 0;
}

auto lptim_v2::capture_input_now(std::size_t channel) const -> bool
{
  auto const& signal = capture_inputs_[channel];
  return (regs_.peek(inputs_.offset) & (channel == 0 ? capture1_select : capture2_select)) == 0
     and not signal.empty()
     and std::ranges::any_of(ctx_.peers.all(), [&](model const* peer) {
           auto const* const port = dynamic_cast<gpio_port const*>(peer);
           return port != nullptr and port->signal_level(signal).value_or(false);
         });
}

auto lptim_v2::capture_line(std::size_t channel) -> filter_line
{
  return {
      state_.capture_level[channel], state_.capture_filtered[channel],
      state_.capture_pending[channel], state_.capture_filter_due[channel]};
}

auto lptim_v2::capture_due(std::uint64_t edge_ps, std::size_t channel) const -> std::uint64_t
{
  if(state_.hz == 0)
  {
    return edge_ps;
  }
  auto const offset = capture_offsets[get(regs_, prescaler_)][get(regs_, capture_filter_[channel])];
  auto const edge   = wide{edge_ps} * state_.hz;
  auto const base =
      state_.running != 0 and edge >= anchor() ? anchor() : wide{state_.enable_ps} * state_.hz;
  auto const ticks = (edge - std::min(edge, base)) / tick_unit();
  return ceil_ps(base + (ticks + offset) * tick_unit());
}

auto lptim_v2::anchor() const -> wide
{
  return wide{state_.anchor_ps} * state_.hz + state_.anchor_frac;
}

auto lptim_v2::tick_unit() const -> wide
{
  return (wide{1} << get(regs_, prescaler_)) * ps_per_second;
}

void lptim_v2::set_anchor(wide at)
{
  state_.anchor_ps   = static_cast<std::uint64_t>(at / state_.hz);
  state_.anchor_frac = static_cast<std::uint64_t>(at % state_.hz);
}

auto lptim_v2::first_wrap() const -> std::uint64_t
{
  auto const from = state_.anchor_count;
  return from <= state_.reload ? state_.reload + 1u - from : counter_top + 1u - from;
}

auto lptim_v2::position(std::uint64_t ticks) const -> std::uint32_t
{
  auto const wrap = first_wrap();
  if(ticks < wrap)
  {
    return static_cast<std::uint32_t>(state_.anchor_count + ticks);
  }
  return static_cast<std::uint32_t>((ticks - wrap) % (std::uint64_t{state_.reload} + 1u));
}

auto lptim_v2::ticks_at(std::uint64_t ps) const -> std::optional<std::uint64_t>
{
  auto const at = wide{ps} * state_.hz;
  if(state_.running == 0 or state_.hz == 0 or external() or at < anchor())
  {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>((at - anchor()) / tick_unit());
}

auto lptim_v2::count_at(std::uint64_t ps) const -> std::uint32_t
{
  auto const ticks = ticks_at(ps);
  return ticks ? position(*ticks) : state_.anchor_count;
}

auto lptim_v2::ceil_ps(wide at) const -> std::uint64_t
{
  return static_cast<std::uint64_t>((at + state_.hz - 1) / state_.hz);
}

auto lptim_v2::ticks_to(std::uint32_t value) const -> std::uint64_t
{
  auto const from   = state_.anchor_count;
  auto const period = std::uint64_t{state_.reload} + 1u;
  return from <= state_.reload ? (value >= from ? value - from : period - from + value)
                               : first_wrap() + value;
}

auto lptim_v2::match_due(std::uint32_t value) const -> std::optional<std::uint64_t>
{
  if(state_.running == 0 or state_.hz == 0 or external() or value > state_.reload)
  {
    return std::nullopt;
  }
  auto const step    = wide{std::uint64_t{state_.reload} + 1u} * tick_unit();
  auto const floor   = wide{state_.floor_ps} * state_.hz;
  auto const settled = wide{state_.settled_ps} * state_.hz;
  auto       at      = anchor() + wide{ticks_to(value) + 1u} * tick_unit();
  if(at <= floor)
  {
    at += ((floor - at) / step + 1) * step;
  }
  if(at <= settled)
  {
    at += ((settled - at) / step + 1) * step;
  }
  return ceil_ps(at);
}

auto lptim_v2::kernel_due(std::uint64_t ps, std::uint64_t cycles) const -> std::uint64_t
{
  auto const units = wide{cycles} * ps_per_second;
  return state_.hz == 0 ? static_cast<std::uint64_t>(units) : ceil_ps(wide{ps} * state_.hz + units);
}

auto lptim_v2::repetition_ok_due(std::uint64_t ps) const -> std::uint64_t
{
  auto const cycles = wide{ok_cycles} * ps_per_second;
  if(state_.hz == 0)
  {
    return static_cast<std::uint64_t>(cycles);
  }
  auto const bus = ctx_.clock.hz(bus_);
  auto const crossed =
      bus == 0
          ? ps
          : ps + static_cast<std::uint64_t>((wide{bus_cycles} * ps_per_second + bus - 1) / bus);
  return ceil_ps(wide{crossed} * state_.hz + cycles);
}

auto lptim_v2::period() const -> std::uint64_t
{
  return std::uint64_t{state_.reload} + 1u;
}

auto lptim_v2::overflows(std::uint64_t ticks) const -> std::uint64_t
{
  auto const wrap = first_wrap();
  return ticks < wrap ? 0 : 1 + (ticks - wrap) / period();
}

auto lptim_v2::repetition_after(std::uint64_t count) const -> std::uint32_t
{
  if(count <= state_.repetition)
  {
    return static_cast<std::uint32_t>(state_.repetition - count);
  }
  auto const cycle = std::uint64_t{state_.repetition_reload} + 1u;
  return static_cast<std::uint32_t>(
      state_.repetition_reload - (count - state_.repetition - 1u) % cycle);
}

auto lptim_v2::update_ticks() const -> std::uint64_t
{
  return first_wrap() + std::uint64_t{state_.repetition} * period();
}

auto lptim_v2::update_due() const -> std::optional<std::uint64_t>
{
  if(state_.running == 0 or state_.hz == 0 or external())
  {
    return std::nullopt;
  }
  auto const step =
      wide{std::uint64_t{state_.repetition_reload} + 1u} * wide{period()} * tick_unit();
  auto const settled = wide{state_.settled_ps} * state_.hz;
  auto       at      = anchor() + wide{update_ticks()} * tick_unit();
  if(at <= settled)
  {
    at += ((settled - at) / step + 1) * step;
  }
  return ceil_ps(at);
}

auto lptim_v2::preload() const -> bool
{
  return get(regs_, preload_) != 0;
}

auto lptim_v2::wave() const -> bool
{
  return get(regs_, wave_) != 0;
}

auto lptim_v2::outputs() const -> std::size_t
{
  return out_.size();
}

auto lptim_v2::compare_of(std::size_t channel) const -> std::uint32_t
{
  return channel == 0 ? state_.compare : state_.compare2;
}

auto lptim_v2::drive(bool raw, std::size_t channel) const -> bool
{
  if(variant_.channels == 0)
  {
    return raw != (get(regs_, polarity_) != 0);
  }
  return get(regs_, channel_enable_[channel]) != 0
     and get(regs_, channel_select_[channel]) == 0
     and raw != ((get(regs_, channel_polarity_[channel]) & 1u) != 0);
}

auto lptim_v2::raw_at(std::uint64_t ps, std::size_t channel) const -> bool
{
  if(((state_.latched >> channel) & 1u) != 0)
  {
    return true;
  }
  if(state_.running == 0)
  {
    return false;
  }
  if(external())
  {
    return compare_of(channel) < state_.anchor_count and state_.anchor_count <= state_.reload;
  }
  if(state_.hz == 0)
  {
    return ((state_.raw >> channel) & 1u) != 0;
  }
  auto const ticks = ticks_at(ps);
  if(not ticks)
  {
    return false;
  }
  auto const count = position(*ticks);
  return compare_of(channel) < count and count <= state_.reload;
}

auto lptim_v2::out_due() const -> std::optional<std::uint64_t>
{
  if(state_.running == 0 or state_.hz == 0 or external())
  {
    return std::nullopt;
  }
  auto const step  = wide{std::uint64_t{state_.reload} + 1u} * tick_unit();
  auto const after = wide{state_.out_ps} * state_.hz;
  auto       best  = std::optional<std::uint64_t>{};
  for(std::size_t channel = 0; channel < outputs(); ++channel)
  {
    auto const silent = variant_.channels != 0
                    and (get(regs_, channel_enable_[channel]) == 0
                         or get(regs_, channel_select_[channel]) != 0);
    if(((state_.latched >> channel) & 1u) != 0
       or not(listened_[channel].value_or(false) or wave())
       or (silent and not wave()))
    {
      continue;
    }
    for(auto const value : {compare_of(channel), state_.reload})
    {
      if(value > state_.reload)
      {
        continue;
      }
      auto at = anchor() + wide{ticks_to(value) + 1u} * tick_unit();
      if(at <= after)
      {
        at += ((after - at) / step + 1) * step;
      }
      best = std::min(best.value_or(ceil_ps(at)), ceil_ps(at));
    }
  }
  return best;
}

void lptim_v2::sync_out(std::uint64_t ps)
{
  for(std::size_t channel = 0; channel < outputs(); ++channel)
  {
    auto const mask = static_cast<std::uint8_t>(1u << channel);
    auto const raw  = raw_at(ps, channel);
    if(raw and wave() and state_.running != 0)
    {
      state_.latched |= mask;
    }
    state_.raw       = static_cast<std::uint8_t>(raw ? state_.raw | mask : state_.raw & ~mask);
    auto const level = drive(raw, channel);
    if(level != ((state_.out & mask) != 0))
    {
      state_.out = static_cast<std::uint8_t>(level ? state_.out | mask : state_.out & ~mask);
      if(listened_[channel].value_or(false))
      {
        out_[channel].edge(level ? trigger_edge::rising : trigger_edge::falling, ps);
      }
    }
  }
  state_.out_ps = std::max(state_.out_ps, ps);
}

void lptim_v2::load(std::span<std::byte const> in)
{
  dispatch_model::load(in);
  listened_ = {};
}

void lptim_v2::interest_changed()
{
  catch_up(now_ps());
  refresh();
  for(std::size_t channel = 0; channel < outputs(); ++channel)
  {
    listened_[channel] = out_[channel].wanted();
  }
  reschedule();
}

auto lptim_v2::produces(std::string_view signal) const -> bool
{
  return std::ranges::any_of(out_, [&](auto const& out) { return out.signal() == signal; });
}

auto lptim_v2::level(std::string_view signal) const -> std::optional<bool>
{
  for(std::size_t channel = 0; channel < outputs(); ++channel)
  {
    if(out_[channel].signal() == signal)
    {
      return drive(raw_at(now_ps(), channel), channel);
    }
  }
  return std::nullopt;
}

auto lptim_v2::next_term() const -> std::optional<due_term>
{
  auto best = std::optional<due_term>{};
  if(state_.hz == 0)
  {
    if(state_.input_due != 0)
    {
      best = due_term{state_.input_due, term::input};
    }
    if(state_.input2_due != 0 and (not best or state_.input2_due < best->due))
    {
      best = due_term{state_.input2_due, term::input2};
    }
    if(state_.trigger_due != 0 and (not best or state_.trigger_due < best->due))
    {
      best = due_term{state_.trigger_due, term::trigger};
    }
    offer_captures(best);
    return best;
  }
  auto offer = [&](std::optional<std::uint64_t> due, term which) {
    if(due and *due != 0 and (not best or *due < best->due))
    {
      best = due_term{*due, which};
    }
  };
  if((state_.isr & compare_match) == 0 and not capturing(0))
  {
    offer(match_due(state_.compare), term::compare);
  }
  if(variant_.channels != 0
     and get(regs_, channel_select_[1]) == 0
     and (state_.isr & compare2_match) == 0)
  {
    offer(match_due(state_.compare2), term::compare2);
  }
  if((state_.isr & reload_match) == 0)
  {
    offer(match_due(state_.reload), term::reload);
  }
  offer(state_.compare_due, term::compare_written);
  offer(state_.compare2_due, term::compare2_written);
  offer(state_.reload_due, term::reload_written);
  offer(state_.interrupts_due, term::interrupts_written);
  offer(state_.repetition_due, term::repetition_written);
  offer(state_.reset_due, term::counter_reset);
  offer(state_.update_flag_due, term::update_flag);
  if(state_.single != 0 or state_.preloaded != 0 or wants_update_events())
  {
    offer(update_due(), term::update);
  }
  offer(state_.switch_due, term::mode_switch);
  offer(out_due(), term::output);
  offer(state_.input_due, term::input);
  offer(state_.input2_due, term::input2);
  offer(state_.trigger_due, term::trigger);
  offer_captures(best);
  return best;
}

void lptim_v2::offer_captures(std::optional<due_term>& best) const
{
  auto const offer = [&](std::uint64_t due, term which) {
    if(due != 0 and (not best or due < best->due))
    {
      best = due_term{due, which};
    }
  };
  offer(state_.capture_filter_due[0], term::capture1_input);
  offer(state_.capture_filter_due[1], term::capture2_input);
  if(not state_.captures[0].empty())
  {
    offer(state_.captures[0].front(), term::capture1);
  }
  if(not state_.captures[1].empty())
  {
    offer(state_.captures[1].front(), term::capture2);
  }
  if(auto const due = source_due())
  {
    offer(*due, term::capture2_clock);
  }
}

void lptim_v2::raise_flag(std::uint32_t flag)
{
  state_.isr |= flag;
  if((state_.interrupts & flag) != 0)
  {
    state_.armed |= flag;
  }
  if(flag == update_event)
  {
    raise_request(update_request);
  }
}

auto lptim_v2::request_wanted(std::size_t request) const -> bool
{
  if(variant_.channels == 0 or not enabled())
  {
    return false;
  }
  if(request == update_request)
  {
    return (state_.interrupts & update_dma_enable) != 0
       and (get(regs_, channel_enable_[0]) != 0 or get(regs_, channel_enable_[1]) != 0);
  }
  return (state_.interrupts & capture_dma_enables[request]) != 0
     and get(regs_, channel_enable_[request]) != 0;
}

auto lptim_v2::wants_update_events() const -> bool
{
  return (state_.isr & update_event) == 0
      or (state_.requested[update_request] == 0 and request_wanted(update_request));
}

void lptim_v2::raise_request(std::size_t request)
{
  if(state_.requested[request] != 0 or not request_wanted(request))
  {
    return;
  }
  state_.requested[request] = 1;
  requests_[request].set(true);
}

void lptim_v2::drop_request(std::size_t request)
{
  if(std::exchange(state_.requested[request], std::uint8_t{0}) != 0)
  {
    requests_[request].set(false);
  }
}

void lptim_v2::sync_requests()
{
  for(std::size_t request = 0; request < state_.requested.size(); ++request)
  {
    if(state_.requested[request] != 0 and not request_wanted(request))
    {
      drop_request(request);
    }
  }
}

void lptim_v2::acknowledged(dma_controller const& by, std::uint32_t request)
{
  catch_up(now_ps());
  for(std::size_t index = 0; index < requests_.size(); ++index)
  {
    if(requests_[index].carries(by, request))
    {
      drop_request(index);
    }
  }
  refresh();
  reschedule();
}

void lptim_v2::transfer_completed(dma_controller const&, std::uint32_t)
{}

void lptim_v2::on_core_hold(bool held)
{
  if(held)
  {
    if(not debug_frozen(freeze_logged_) or state_.held != 0)
    {
      return;
    }
  }
  else if(state_.held == 0)
  {
    return;
  }
  auto const t = now_ps();
  catch_up(t);
  state_.held = held ? 1 : 0;
  rebase(t, kernel_hz());
  refresh();
  reschedule();
}

void lptim_v2::apply(due_term const& next)
{
  if(next.which == term::compare or next.which == term::compare2 or next.which == term::reload)
  {
    raise_flag(
        next.which == term::compare    ? compare_match
        : next.which == term::compare2 ? compare2_match
                                       : reload_match);
    return;
  }
  if(next.which == term::update)
  {
    if(auto const ticks = ticks_at(next.due))
    {
      reanchor(next.due, *ticks);
    }
    if(wants_update_events() and state_.update_flag_due == 0)
    {
      state_.update_flag_due = ceil_ps(wide{next.due} * state_.hz + tick_unit());
    }
    transfer_preloaded();
    if(state_.single != 0)
    {
      stop();
    }
    return;
  }
  if(next.which == term::update_flag)
  {
    state_.update_flag_due = 0;
    raise_flag(update_event);
    return;
  }
  if(next.which == term::counter_reset)
  {
    state_.reset_due = 0;
    regs_.poke(cr_.offset, regs_.peek(cr_.offset) & ~counter_reset);
    restart_count(next.due);
    return;
  }
  if(next.which == term::repetition_written)
  {
    state_.repetition_due = 0;
    raise_flag(repetition_ok);
    auto const underflow     = settle_to(next.due);
    state_.repetition_reload = state_.repetition_pending;
    if(underflow and state_.running != 0)
    {
      state_.repetition = state_.repetition_reload;
    }
    return;
  }
  if(next.which == term::output)
  {
    return;
  }
  if(next.which == term::trigger)
  {
    state_.trigger_due      = 0;
    state_.trigger_filtered = state_.trigger_pending;
    trigger_fired(next.due, state_.trigger_pending != 0);
    return;
  }
  if(next.which == term::input)
  {
    state_.input_due      = 0;
    state_.input_filtered = state_.input_pending;
    input_edge(next.due, state_.input_pending != 0);
    return;
  }
  if(next.which == term::capture1_input or next.which == term::capture2_input)
  {
    auto const channel                 = next.which == term::capture1_input ? 0uz : 1uz;
    state_.capture_filter_due[channel] = 0;
    state_.capture_filtered[channel]   = state_.capture_pending[channel];
    capture_edge(channel, next.due, state_.capture_pending[channel] != 0);
    return;
  }
  if(next.which == term::capture2_clock)
  {
    state_.source_ps    = next.due;
    state_.source_level = state_.source_level != 0 ? 0 : 1;
    capture_changed(1, next.due, state_.source_level != 0);
    return;
  }
  if(next.which == term::capture1 or next.which == term::capture2)
  {
    apply_capture(next.which == term::capture1 ? 0uz : 1uz, next.due);
    return;
  }
  if(next.which == term::input2)
  {
    state_.input2_due      = 0;
    state_.input2_filtered = state_.input2_pending;
    input_edge(next.due, state_.input2_pending != 0, true);
    return;
  }
  if(next.which == term::mode_switch)
  {
    state_.switch_due = 0;
    if(state_.running != 0)
    {
      static_cast<void>(settle_to(next.due));
    }
    if(get(regs_, trigger_enable_) != 0)
    {
      arm(state_.switch_single != 0);
    }
    if(state_.running != 0)
    {
      state_.single = state_.switch_single;
    }
    else if(get(regs_, trigger_enable_) != 0)
    {
      return;
    }
    else if(state_.hz != 0)
    {
      begin(wide{next.due} * state_.hz, 0, state_.switch_single != 0);
    }
    else
    {
      start(next.due, state_.switch_single != 0);
      state_.paused_units = 0;
    }
    return;
  }
  if(next.which == term::interrupts_written)
  {
    auto const changed    = state_.interrupts ^ state_.interrupts_pending;
    state_.interrupts     = state_.interrupts_pending;
    state_.interrupts_due = 0;
    state_.isr &= ~changed;
    state_.armed &= ~changed;
    raise_flag(interrupts_ok);
    return;
  }
  auto const target = slot(next.which);
  raise_flag(target.flag);
  if(state_.running != 0 and preload())
  {
    state_.preloaded |= target.bit;
    target.preload = target.pending;
    target.due     = 0;
    return;
  }
  auto const flag = next.which == term::compare_written  ? compare_match
                  : next.which == term::compare2_written ? compare2_match
                                                         : reload_match;
  auto const second_off =
      (next.which == term::compare2_written and get(regs_, channel_select_[1]) != 0)
      or (next.which == term::compare_written and capturing(0));
  if(not second_off and (state_.isr & flag) == 0 and in_flight(target.value, next.due))
  {
    raise_flag(flag);
  }
  static_cast<void>(settle_to(next.due));
  target.value = target.pending;
  target.due   = 0;
}

auto lptim_v2::slot(term which) -> value_slot
{
  switch(which)
  {
    case term::compare_written:
      return {state_.compare,
              state_.compare_pending,
              state_.compare_preload,
              state_.compare_due,
              compare_ok,
              1};
    case term::compare2_written:
      return {
          state_.compare2,
          state_.compare2_pending,
          state_.compare2_preload,
          state_.compare2_due,
          compare2_ok,
          4};
    default:
      return {state_.reload,
              state_.reload_pending,
              state_.reload_preload,
              state_.reload_due,
              reload_ok,
              2};
  }
}

auto lptim_v2::in_flight(std::uint32_t value, std::uint64_t ps) const -> bool
{
  auto const due = match_due(value);
  return due and *due > ps and wide{*due} * state_.hz - tick_unit() <= wide{ps} * state_.hz;
}

auto lptim_v2::settle_to(std::uint64_t ps) -> bool
{
  if(update_due() == ps)
  {
    apply(due_term{ps, term::update});
    return true;
  }
  if(auto const ticks = ticks_at(ps))
  {
    reanchor(ps, *ticks);
  }
  return false;
}

void lptim_v2::reanchor(std::uint64_t ps, std::uint64_t ticks)
{
  state_.repetition   = repetition_after(overflows(ticks));
  state_.anchor_count = position(ticks);
  state_.floor_ps     = ps;
  set_anchor(anchor() + wide{ticks} * tick_unit());
}

void lptim_v2::catch_up(std::uint64_t ps)
{
  std::array<bool, 2> wanted{};
  for(std::size_t channel = 0; channel < outputs(); ++channel)
  {
    wanted[channel]    = out_[channel].wanted();
    listened_[channel] = listened_[channel].value_or(wanted[channel]) and wanted[channel];
  }
  while(auto const next = next_term())
  {
    if(next->due > ps)
    {
      break;
    }
    apply(*next);
    sync_out(next->due);
  }
  state_.settled_ps = std::max(state_.settled_ps, ps);
  sync_out(ps);
  for(std::size_t channel = 0; channel < outputs(); ++channel)
  {
    listened_[channel] = wanted[channel];
  }
}

void lptim_v2::start(std::uint64_t ps, bool single)
{
  state_.divided = 0;
  if(state_.hz == 0)
  {
    state_.paused_counting = 0;
    state_.paused_units    = start_cycles * ps_per_second;
    state_.anchor_count    = 0;
    state_.repetition      = state_.repetition_reload;
    state_.floor_ps        = ps;
    state_.running         = 1;
    state_.single          = single ? 1 : 0;
    state_.down            = 0;
    return;
  }
  begin(wide{ps} * state_.hz + wide{start_cycles} * ps_per_second, ps, single);
}

void lptim_v2::begin(wide at, std::uint64_t ps, bool single)
{
  auto const ready = wide{state_.enable_ps} * state_.hz + 2 * tick_unit();
  set_anchor(std::max(at, ready));
  state_.anchor_count = 0;
  state_.repetition   = state_.repetition_reload;
  state_.floor_ps     = ps;
  state_.running      = 1;
  state_.single       = single ? 1 : 0;
  state_.down         = 0;
}

void lptim_v2::switch_mode(std::uint64_t ps, bool single)
{
  if(state_.switch_due != 0)
  {
    warn_once_or_refuse(
        switch_logged_, "CR", cr_.offset,
        "a CNTSTRT or SNGSTRT write before the previous one reaches the counter: the later one "
        "replaces it");
  }
  else if(state_.running != 0 and (state_.single != 0) == single)
  {
    return;
  }
  auto const cycles = wide{start_cycles} * ps_per_second;
  state_.switch_due =
      state_.hz == 0 ? static_cast<std::uint64_t>(cycles) : ceil_ps(wide{ps} * state_.hz + cycles);
  state_.switch_single = single ? 1 : 0;
}

void lptim_v2::stop()
{
  if(state_.update_pending != 0)
  {
    state_.update_pending = 0;
    raise_flag(update_event);
  }
  transfer_preloaded();
  state_.running      = 0;
  state_.single       = 0;
  state_.anchor_count = 0;
}

void lptim_v2::transfer_preloaded()
{
  if((state_.preloaded & 1u) != 0)
  {
    state_.compare = state_.compare_preload;
  }
  if((state_.preloaded & 2u) != 0)
  {
    state_.reload = state_.reload_preload;
  }
  if((state_.preloaded & 4u) != 0)
  {
    state_.compare2 = state_.compare2_preload;
  }
  state_.preloaded = 0;
}

void lptim_v2::restart_count(std::uint64_t ps)
{
  if(external())
  {
    state_.anchor_count = 0;
    state_.divided      = 0;
    return;
  }
  auto const ticks = ticks_at(ps);
  if(not ticks)
  {
    return;
  }
  reanchor(ps, *ticks);
  set_anchor(wide{ps} * state_.hz);
  state_.anchor_count = 0;
}

void lptim_v2::unmodelled(use which, std::string_view reg_name)
{
  auto const index = static_cast<std::size_t>(which);
  warn_once_or_refuse(
      unmodelled_logged_[index], reg_name, reg(reg_name).offset,
      std::string{unmodelled_table[index]});
}

void lptim_v2::refuse_channels()
{
  if(variant_.channels == 0)
  {
    return;
  }
  if((capturing(0) or capturing(1)) and external())
  {
    unmodelled(use::capture, "CCMR1");
  }
  for(std::size_t channel = 0; channel < 2; ++channel)
  {
    if(capturing(channel) and get(regs_, channel_polarity_[channel]) == 2)
    {
      warn_once_or_refuse(
          capture_polarity_logged_[channel], "CCMR1", reg("CCMR1").offset,
          std::format(
              "CC{}P 10 on a capture channel, which RM0456 58.7.17 reserves: both edges capture",
              channel + 1));
    }
  }
}

void lptim_v2::refuse_clock()
{
  auto const polarity = get(regs_, clock_polarity_);
  auto const stopped  = ctx_.clock.hz(kernel_) == 0;
  auto const refusals = std::to_array<std::pair<bool, std::string_view>>({
      {external() and get(regs_, prescaler_) != 0 and not clocked_by_input(),
       "PRESC not 0 with COUNTMODE 1 or ENC 1, which RM0456 58.4.12 and 58.4.15 forbid: every "
       "input edge counts"},
      {external() and polarity == 3,
       "CKPOL 11, which RM0456 58.7.10 forbids: both input 1 edges count"},
      {clocked_by_input() and polarity == 2,
       "CKPOL 10 with CKSEL 1, which RM0456 58.4.12 forbids: both input 1 edges count"},
      {external() and get(regs_, clock_filter_) != 0 and stopped,
       "CKFLT not 0 with the kernel clock stopped, which RM0456 58.4.5 forbids: the filter passes "
       "no input edge"},
      {get(regs_, trigger_enable_) != 0 and get(regs_, trigger_filter_) != 0 and stopped,
       "TRGFLT not 0 with the kernel clock stopped, which RM0456 58.4.5 forbids: the filter passes "
       "no trigger"},
      {variant_.encoder and get(regs_, encoder_) != 0 and clocked_by_input(),
       "ENC 1 with CKSEL 1, which RM0456 58.4.15 forbids: input 1 clocks the counter"},
  });
  static_assert(
      std::tuple_size_v<decltype(refusals)> == std::tuple_size_v<decltype(clock_logged_)>);
  for(std::size_t index = 0; index < refusals.size(); ++index)
  {
    if(refusals[index].first)
    {
      warn_once_or_refuse(
          clock_logged_[index], "CFGR", cfgr_.offset,
          std::format("{} at a counter start", refusals[index].second));
    }
  }
}

void lptim_v2::refuse_at_start()
{
  if(selects_unmodelled_input())
  {
    unmodelled(use::input_source, "CFGR2");
  }
  refuse_channels();
  refuse_clock();
}

void lptim_v2::refresh()
{
  sync_out(now_ps());
  sync_requests();
  regs_.poke(isr_.offset, state_.isr);
  regs_.poke(cnt_.offset, count_at(now_ps()));
  if(state_.armed != 0)
  {
    line_->raise();
  }
  else
  {
    line_->lower();
  }
}

void lptim_v2::reschedule()
{
  sync_source(now_ps());
  if(auto const seq = std::exchange(state_.queued, 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
  auto const next = next_term();
  if(not next)
  {
    return;
  }
  state_.queued =
      ctx_.events.schedule_at(vtime{next->due}, tick_event, ++state_.generation, ctx_.clock) + 1;
}

void lptim_v2::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void lptim_v2::on_bus_reset()
{
  if(auto const seq = std::exchange(state_.queued, 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
  for(std::size_t channel = 0; channel < outputs(); ++channel)
  {
    if(((state_.out >> channel) & 1u) != 0 and out_[channel].wanted())
    {
      out_[channel].edge(trigger_edge::falling, now_ps());
    }
  }
  for(std::size_t request = 0; request < state_.requested.size(); ++request)
  {
    drop_request(request);
  }
  auto const held = state_.held;
  restore_reset_values();
  state_.held = held;
  state_.hz   = kernel_hz();
  take_input_level();
}

void lptim_v2::take_input_level()
{
  auto const level       = input_now() ? 1 : 0;
  state_.input_level     = static_cast<std::uint8_t>(level);
  state_.input_filtered  = static_cast<std::uint8_t>(level);
  state_.input_pending   = static_cast<std::uint8_t>(level);
  state_.input_due       = 0;
  auto const level2      = input_now(true) ? 1 : 0;
  state_.input2_level    = static_cast<std::uint8_t>(level2);
  state_.input2_filtered = static_cast<std::uint8_t>(level2);
  state_.input2_pending  = static_cast<std::uint8_t>(level2);
  state_.input2_due      = 0;
  auto const trigger     = std::ranges::any_of(ctx_.peers.all(), [&](model const* peer) {
    auto const* const port = dynamic_cast<gpio_port const*>(peer);
    return port != nullptr and port->signal_level(etr_).value_or(false);
  });
  for(std::size_t channel = 0; channel < 2; ++channel)
  {
    auto const level                   = capture_input_now(channel) ? 1 : 0;
    state_.capture_level[channel]      = static_cast<std::uint8_t>(level);
    state_.capture_filtered[channel]   = static_cast<std::uint8_t>(level);
    state_.capture_pending[channel]    = static_cast<std::uint8_t>(level);
    state_.capture_filter_due[channel] = 0;
  }
  state_.trigger_level    = trigger ? 1 : 0;
  state_.trigger_filtered = state_.trigger_level;
  state_.trigger_pending  = state_.trigger_level;
  state_.trigger_due      = 0;
}

void lptim_v2::arm(bool single)
{
  state_.armed_mode = single ? 2 : 1;
}

auto lptim_v2::filtered_change(
    filter_line   line,
    std::uint64_t ps,
    bool          level,
    std::uint32_t code,
    bool          live) -> bool
{
  auto const was = line.level != 0;
  line.level     = level ? 1 : 0;
  if(level == was)
  {
    return false;
  }
  if(not live)
  {
    line.filtered = line.level;
    line.pending  = line.level;
    line.due      = 0;
    return false;
  }
  if(code == 0)
  {
    line.filtered = line.level;
    return true;
  }
  auto const sampling = ctx_.clock.hz(kernel_);
  line.due            = 0;
  line.pending        = line.level;
  if(level == (line.filtered != 0) or sampling == 0)
  {
    line.filtered = line.level;
    return false;
  }
  auto const samples = wide{1u << code} * ps_per_second;
  line.due = static_cast<std::uint64_t>((wide{ps} * sampling + samples + sampling - 1) / sampling);
  return false;
}

void lptim_v2::trigger_changed(std::uint64_t ps, bool level)
{
  auto const live =
      enabled() and get(regs_, trigger_enable_) != 0 and get(regs_, trigger_select_) == 0;
  if(filtered_change(
         {state_.trigger_level, state_.trigger_filtered, state_.trigger_pending,
          state_.trigger_due},
         ps, level, get(regs_, trigger_filter_), live))
  {
    trigger_fired(ps, level);
    sync_out(ps);
  }
}

void lptim_v2::trigger_fired(std::uint64_t ps, bool rising)
{
  auto const polarity = get(regs_, trigger_enable_);
  if((polarity == 1 and not rising) or (polarity == 2 and rising))
  {
    return;
  }
  if(state_.held != 0 or (state_.hz == 0 and not clocked_by_input()))
  {
    return;
  }
  if(state_.running != 0)
  {
    if(get(regs_, timeout_) == 0)
    {
      return;
    }
    raise_flag(trigger_event);
    restart_on_trigger(ps, state_.single != 0);
    return;
  }
  if(state_.armed_mode == 0 or (wave() and state_.started_once != 0))
  {
    return;
  }
  raise_flag(trigger_event);
  restart_on_trigger(ps, state_.armed_mode == 2);
  state_.started_once = 1;
}

void lptim_v2::restart_on_trigger(std::uint64_t ps, bool single)
{
  if(state_.hz == 0)
  {
    start(ps, single);
    state_.paused_units = static_cast<std::uint64_t>(2 * tick_unit());
    return;
  }
  begin(wide{ps} * state_.hz + 2 * tick_unit(), ps, single);
  state_.divided = 0;
}

void lptim_v2::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation = state_.generation + 1;
  state_                = {};
  state_.generation     = generation;
  state_.reload         = describe("ARR").reset;
  state_.hz             = kernel_hz();
  state_.settled_ps     = now_ps();
  line_->lower();
}

void lptim_v2::rebase(std::uint64_t ps, std::uint64_t hz)
{
  auto       dues = std::array{&state_.compare_due,    &state_.compare2_due,   &state_.reload_due,
                               &state_.interrupts_due, &state_.repetition_due, &state_.reset_due,
                               &state_.switch_due,     &state_.update_flag_due};
  auto const old  = state_.hz;
  if(old != 0)
  {
    auto const at = wide{ps} * old;
    for(auto* const due : dues)
    {
      if(*due != 0)
      {
        *due = static_cast<std::uint64_t>(std::max(wide{*due} * old, at) - at);
      }
    }
    state_.paused_counting = 0;
    state_.paused_units    = 0;
    if(state_.running != 0 and at >= anchor() and external())
    {
      state_.paused_counting = 1;
    }
    else if(state_.running != 0 and at >= anchor())
    {
      auto const since = at - anchor();
      state_.repetition =
          repetition_after(overflows(static_cast<std::uint64_t>(since / tick_unit())));
      state_.anchor_count    = position(static_cast<std::uint64_t>(since / tick_unit()));
      state_.paused_counting = 1;
      state_.paused_units    = static_cast<std::uint64_t>(since % tick_unit());
    }
    else if(state_.running != 0)
    {
      state_.paused_units = static_cast<std::uint64_t>(anchor() - at);
    }
  }
  state_.hz = hz;
  if(hz == 0)
  {
    return;
  }
  auto const at = wide{ps} * hz;
  for(auto* const due : dues)
  {
    if(*due != 0)
    {
      *due = std::max<std::uint64_t>(ceil_ps(at + *due), ps + 1);
    }
  }
  if(state_.running != 0)
  {
    auto const units = wide{state_.paused_units};
    set_anchor(state_.paused_counting != 0 ? (at > units ? at - units : 0) : at + units);
    if(state_.paused_counting != 0)
    {
      state_.floor_ps = ps;
    }
  }
  state_.paused_counting = 0;
  state_.paused_units    = 0;
}

void lptim_v2::on_clock_change()
{
  auto const hz = kernel_hz();
  if(hz == state_.hz)
  {
    if(state_.source_live != 0 or (variant_.capture and variant_.channels != 0))
    {
      catch_up(now_ps());
      refresh();
      reschedule();
    }
    return;
  }
  auto const t = now_ps();
  catch_up(t);
  rebase(t, hz);
  refresh();
  reschedule();
}

void lptim_v2::plain_written(std::uint32_t, std::uint32_t)
{}

void lptim_v2::clear_written(std::uint32_t, std::uint32_t now)
{
  catch_up(now_ps());
  for(std::size_t channel = 0; channel < 2 and variant_.capture and variant_.channels != 0;
      ++channel)
  {
    if((now & (channel == 0 ? overcapture1 : overcapture2)) != 0)
    {
      overcapture_cleared(channel, now_ps());
    }
  }
  state_.isr &= ~now;
  state_.armed &= ~now;
  regs_.poke(icr_.offset, 0);
  refresh();
  reschedule();
}

void lptim_v2::interrupts_written(std::uint32_t old, std::uint32_t now)
{
  if(auto const kept = now & variant_.interrupt_mask; kept != now)
  {
    now = kept;
    regs_.poke(dier_.offset, kept);
  }
  if(not enabled())
  {
    regs_.poke(dier_.offset, old);
    warn_once_or_refuse(
        locked_logged_[0], variant_.interrupts, dier_.offset,
        "a DIER write with CR.ENABLE 0: the register keeps its value");
    return;
  }
  auto const t = now_ps();
  catch_up(t);
  if(state_.interrupts_due != 0)
  {
    regs_.poke(dier_.offset, old);
    warn_once_or_refuse(
        pending_logged_[0], variant_.interrupts, dier_.offset,
        "a DIER write before DIEROK: the new value replaces the pending one");
    regs_.poke(dier_.offset, now);
  }
  state_.interrupts_pending = now;
  state_.interrupts_due     = kernel_due(t, ok_cycles);
  if((now & update_dma_enable) == 0)
  {
    drop_request(update_request);
  }
  for(std::size_t channel = 0; channel < 2; ++channel)
  {
    if((now & capture_dma_enables[channel]) == 0)
    {
      drop_request(channel);
    }
  }
  refresh();
  reschedule();
}

void lptim_v2::configuration_written(std::uint32_t old, std::uint32_t now)
{
  if(not enabled())
  {
    if(old != now)
    {
      auto const t = now_ps();
      regs_.poke(cfgr_.offset, old);
      catch_up(t);
      regs_.poke(cfgr_.offset, now);
      if(kernel_hz() != state_.hz)
      {
        rebase(t, kernel_hz());
      }
      refresh();
      reschedule();
    }
    return;
  }
  if(old == now)
  {
    return;
  }
  regs_.poke(cfgr_.offset, old);
  warn_once_or_refuse(
      locked_logged_[1], "CFGR", cfgr_.offset,
      "a CFGR write with CR.ENABLE 1: the register keeps its value");
}

void lptim_v2::control_written(std::uint32_t old, std::uint32_t now)
{
  auto const t = now_ps();
  catch_up(t);
  auto const was_on         = (old & enable_bit) != 0;
  auto const is_on          = (now & enable_bit) != 0;
  auto const rstare_allowed = was_on ? reset_after_read : (old & reset_after_read);
  if(was_on and is_on and (now & counter_reset) != 0 and state_.reset_due == 0)
  {
    state_.reset_due = kernel_due(t, reset_cycles);
  }
  if(not was_on and is_on)
  {
    state_.enable_ps   = t;
    state_.enable_left = clocked_by_input() ? enable_edges : 0;
  }
  else if(was_on and not is_on)
  {
    state_.switch_due   = 0;
    state_.reset_due    = 0;
    state_.latched      = 0;
    state_.started_once = 0;
    state_.armed_mode   = 0;
    drop_captures(0);
    drop_captures(1);
    state_.overcapture_lost = {};
    state_.capture_off_ps   = {};
    stop();
  }
  regs_.poke(
      cr_.offset,
      (now & (enable_bit | rstare_allowed)) | (state_.reset_due != 0 ? counter_reset : 0u));
  auto const starting = now & (single_start | continuous_run);
  auto       started  = false;
  auto       armed    = false;
  if(was_on and is_on and starting != 0)
  {
    auto const single = (starting & continuous_run) == 0;
    if(state_.running != 0 or state_.switch_due != 0)
    {
      switch_mode(t, single);
    }
    else if(get(regs_, trigger_enable_) != 0)
    {
      arm(single);
      armed = true;
    }
    else if(not(wave() and state_.started_once != 0))
    {
      start(t, single);
      state_.started_once = 1;
      started             = true;
    }
  }
  refresh();
  reschedule();
  if(auto const source = get(regs_, trigger_select_); armed and source != 0)
  {
    warn_once_or_refuse(
        trigger_logged_, "CFGR", cfgr_.offset,
        std::format(
            "TRIGSEL {}, {}, with TRIGEN not 00 at a CNTSTRT or SNGSTRT write: not modelled, "
            "no model produces this trigger input and the counter stays stopped",
            source, variant_.triggers[source]));
  }
  if(started or armed)
  {
    refuse_at_start();
  }
  if(was_on and is_on and starting != 0 and (starting & continuous_run) == 0 and encoder())
  {
    warn_once_or_refuse(
        single_logged_, "CR", cr_.offset,
        "SNGSTRT with ENC 1, which RM0456 58.4.15 runs in continuous mode only: the one-shot stop "
        "applies at a wrap");
  }
}

void lptim_v2::channels_written(std::uint32_t old, std::uint32_t now)
{
  auto const at = reg("CCMR1").offset;
  regs_.poke(at, old);
  catch_up(now_ps());
  auto const taking = [&](std::size_t channel) {
    return capturing(channel) and get(regs_, channel_enable_[channel]) != 0;
  };
  auto const before = std::array{taking(0), taking(1)};
  regs_.poke(at, now);
  for(std::size_t channel = 0; channel < 2; ++channel)
  {
    if(before[channel] and not taking(channel))
    {
      drop_captures(channel);
    }
    if(before[channel] != taking(channel))
    {
      capture_enable_changed(channel, taking(channel), now_ps());
    }
  }
  refresh();
  reschedule();
  if(state_.running != 0)
  {
    refuse_channels();
  }
  for(std::size_t channel = 0; channel < 2; ++channel)
  {
    if(before[channel] and ((old ^ now) & (capture_setup << (16 * channel))) != 0)
    {
      warn_once_or_refuse(
          capture_setup_logged_[channel], "CCMR1", at,
          std::format(
              "a CCMR1 write that changes CC{0}SEL, IC{0}F, CC{0}P or IC{0}PSC with CC{0}E 1, "
              "which RM0456 58.7.17 forbids: the new setting takes effect at the write",
              channel + 1));
    }
  }
}

void lptim_v2::repetition_written(std::uint32_t old, std::uint32_t now)
{
  if(not enabled())
  {
    regs_.poke(rcr_.offset, old);
    warn_once_or_refuse(
        locked_logged_[4], "RCR", rcr_.offset,
        "a RCR write with CR.ENABLE 0: the register keeps its value");
    return;
  }
  auto const t = now_ps();
  catch_up(t);
  if(state_.repetition_due != 0)
  {
    regs_.poke(rcr_.offset, old);
    warn_once_or_refuse(
        pending_logged_[3], "RCR", rcr_.offset,
        "a RCR write before REPOK: the new value replaces the pending one");
    regs_.poke(rcr_.offset, now);
  }
  state_.repetition_pending = now & 0xFFu;
  state_.repetition_due     = repetition_ok_due(t);
  refresh();
  reschedule();
}

template <term Which> void lptim_v2::value_written(std::uint32_t old, std::uint32_t now)
{
  constexpr auto reload     = Which == term::reload_written;
  constexpr auto second     = Which == term::compare2_written;
  auto const     at         = reload ? arr_ : second ? ccr2_ : ccr1_;
  auto const     name       = reload ? std::string_view{"ARR"}
                            : second ? std::string_view{"CCR2"}
                                     : std::string_view{"CCR1"};
  auto const     slot_index = reload ? 2uz : second ? 4uz : 1uz;
  if(not reload and capturing(second ? 1 : 0))
  {
    regs_.poke(at.offset, old);
    return;
  }
  if(reload)
  {
    catch_up(now_ps());
    state_.isr &= ~update_event;
    state_.armed &= ~update_event;
    refresh();
    reschedule();
  }
  if(not enabled())
  {
    regs_.poke(at.offset, old);
    warn_once_or_refuse(
        locked_logged_[slot_index + 1], name, at.offset,
        std::format("a {} write with CR.ENABLE 0: the register keeps its value", name));
    return;
  }
  auto const t = now_ps();
  catch_up(t);
  auto const target = slot(Which);
  if(target.due != 0)
  {
    regs_.poke(at.offset, old);
    warn_once_or_refuse(
        pending_logged_[slot_index], name, at.offset,
        std::format("a {} write before its OK flag: the new value replaces the pending one", name));
    regs_.poke(at.offset, now);
  }
  target.pending = now & counter_top;
  target.due     = kernel_due(t, ok_cycles);
  refresh();
  reschedule();
}

auto lptim_v2::on_read(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    access_ctx     access) -> std::expected<std::uint32_t, bus_error>
{
  auto const t = now_ps();
  catch_up(t);
  refresh();
  auto const value = dispatch_model::on_read(regs, offset, size, access);
  for(std::size_t channel = 0; channel < 2; ++channel)
  {
    if(capturing(channel)
       and access.by != initiator::debugger
       and (offset & ~3u) == (channel == 0 ? ccr1_ : ccr2_).offset)
    {
      auto const flag = channel == 0 ? compare_match : compare2_match;
      state_.isr &= ~flag;
      state_.armed &= ~flag;
      refresh();
    }
  }
  if((offset & ~3u) == cnt_.offset
     and access.by != initiator::debugger
     and (regs_.peek(cr_.offset) & reset_after_read) != 0
     and state_.running != 0)
  {
    restart_count(t);
    refresh();
    reschedule();
  }
  return value;
}

auto lptim_v2::storage_only(std::uint32_t) const -> bool
{
  return false;
}

void lptim_v2::inputs_written(std::uint32_t old, std::uint32_t now)
{
  auto const changed = old ^ now;
  if((changed & (input1_select | input2_select | capture1_select | capture2_select)) == 0)
  {
    return;
  }
  auto const t = now_ps();
  regs_.poke(inputs_.offset, old);
  catch_up(t);
  regs_.poke(inputs_.offset, now);
  if((changed & input1_select) != 0)
  {
    input_changed(t, input_now(), false);
  }
  if((changed & input2_select) != 0 and variant_.encoder)
  {
    input_changed(t, input_now(true), true);
  }
  for(std::size_t channel = 0; channel < 2; ++channel)
  {
    if((changed & (channel == 0 ? capture1_select : capture2_select)) != 0
       and not capture_inputs_[channel].empty())
    {
      capture_changed(channel, t, capture_input_now(channel));
    }
  }
  catch_up(t);
  refresh();
  reschedule();
  if(state_.running != 0 and selects_unmodelled_input())
  {
    unmodelled(use::input_source, "CFGR2");
  }
}

auto lptim_v2::listens(std::string_view) const -> bool
{
  return false;
}

auto lptim_v2::can_listen(std::string_view) const -> bool
{
  return false;
}

auto lptim_v2::takes_pin(std::string_view signal) const -> bool
{
  return signal == in1_
      or signal == etr_
      or (not in2_.empty() and signal == in2_)
      or std::ranges::any_of(
             capture_inputs_, [&](auto const& one) { return not one.empty() and one == signal; });
}

void lptim_v2::triggered(std::string_view signal, trigger_edge edge, trigger_time at)
{
  for(std::size_t channel = 0; channel < 2; ++channel)
  {
    if(capture_inputs_[channel].empty() or signal != capture_inputs_[channel])
    {
      continue;
    }
    if((regs_.peek(inputs_.offset) & (channel == 0 ? capture1_select : capture2_select)) != 0)
    {
      return;
    }
    auto const t  = now_ps();
    auto const ps = std::min(at.ps, t);
    catch_up(ps);
    capture_changed(channel, ps, edge == trigger_edge::rising);
    catch_up(t);
    refresh();
    reschedule();
    return;
  }
  auto const pin    = signal == etr_;
  auto const second = not in2_.empty() and signal == in2_;
  if(not pin and not second and signal != in1_)
  {
    return;
  }
  if(not pin and (regs_.peek(inputs_.offset) & (second ? input2_select : input1_select)) != 0)
  {
    return;
  }
  auto const t  = now_ps();
  auto const ps = std::min(at.ps, t);
  catch_up(ps);
  if(pin)
  {
    trigger_changed(ps, edge == trigger_edge::rising);
  }
  else
  {
    input_changed(ps, edge == trigger_edge::rising, second);
  }
  catch_up(t);
  refresh();
  reschedule();
}

void lptim_v2::capture_changed(std::size_t channel, std::uint64_t ps, bool level)
{
  if(level != (state_.capture_level[channel] != 0))
  {
    state_.capture_edge_ps[channel] = ps;
  }
  auto const live = enabled() and capturing(channel);
  if(filtered_change(capture_line(channel), ps, level, get(regs_, capture_filter_[channel]), live))
  {
    capture_edge(channel, ps, level);
  }
}

void lptim_v2::capture_edge(std::size_t channel, std::uint64_t ps, bool rising)
{
  auto const polarity = get(regs_, channel_polarity_[channel]);
  if((polarity == 0 and not rising) or (polarity == 1 and rising))
  {
    return;
  }
  if(not enabled()
     or not capturing(channel)
     or get(regs_, channel_enable_[channel]) == 0
     or state_.held != 0
     or (state_.hz == 0 and not external()))
  {
    return;
  }
  if(++state_.capture_divided[channel] < (1u << get(regs_, capture_prescaler_[channel])))
  {
    return;
  }
  state_.capture_divided[channel] = 0;
  auto const due =
      external() ? ps : std::max(capture_due(state_.capture_edge_ps[channel], channel), ps);
  auto&      queue = state_.captures[channel];
  auto const place = std::ranges::lower_bound(queue, due);
  if(place == queue.end() or *place != due)
  {
    queue.insert(place, due);
  }
}

void lptim_v2::apply_capture(std::size_t channel, std::uint64_t due)
{
  auto& queue = state_.captures[channel];
  queue.erase(queue.begin());
  auto const value                                  = count_at(due);
  auto const flag                                   = channel == 0 ? compare_match : compare2_match;
  (channel == 0 ? state_.compare : state_.compare2) = value;
  regs_.poke((channel == 0 ? ccr1_ : ccr2_).offset, value);
  if((state_.isr & flag) != 0 and state_.overcapture_lost[channel] == 0)
  {
    raise_flag(channel == 0 ? overcapture1 : overcapture2);
  }
  raise_flag(flag);
  raise_request(channel);
  state_.capture_last_ps[channel] = due;
}

void lptim_v2::drop_captures(std::size_t channel)
{
  state_.captures[channel].clear();
  state_.capture_divided[channel] = 0;
}

auto lptim_v2::source_hz() const -> std::uint64_t
{
  auto const select = (regs_.peek(inputs_.offset) & capture2_select) >> 20;
  if(select == 0 or variant_.capture2_inputs[select].domain.empty())
  {
    return 0;
  }
  return ctx_.clock.hz(source_domains_[select - 1]);
}

auto lptim_v2::source_wanted() const -> bool
{
  return enabled() and capturing(1) and get(regs_, channel_enable_[1]) != 0 and source_hz() != 0;
}

auto lptim_v2::source_due() const -> std::optional<std::uint64_t>
{
  if(state_.source_live == 0)
  {
    return std::nullopt;
  }
  auto const select = (regs_.peek(inputs_.offset) & capture2_select) >> 20;
  auto const hz     = source_hz();
  if(hz == 0)
  {
    return std::nullopt;
  }
  auto const half = (wide{ps_per_second} * variant_.capture2_inputs[select].divider + 2 * hz - 1)
                  / (2 * wide{hz});
  return state_.source_ps + static_cast<std::uint64_t>(half);
}

void lptim_v2::sync_source(std::uint64_t ps)
{
  auto const wanted = variant_.capture and variant_.channels != 0 and source_wanted();
  if(wanted and state_.source_live == 0)
  {
    state_.source_ps    = ps;
    state_.source_level = state_.capture_level[1];
  }
  state_.source_live = wanted ? 1 : 0;
}

auto lptim_v2::in_capture_pulse(std::size_t channel, std::uint64_t ps) const -> bool
{
  auto const near = [&](std::uint64_t due) {
    return state_.hz != 0 and wide{due} * state_.hz <= wide{ps} * state_.hz + tick_unit();
  };
  auto const& queue = state_.captures[channel];
  return (state_.capture_last_ps[channel] == ps and ps != 0)
      or (not queue.empty() and near(queue.front()));
}

void lptim_v2::overcapture_cleared(std::size_t channel, std::uint64_t ps)
{
  if(in_capture_pulse(channel, ps))
  {
    state_.overcapture_lost[channel] = 1;
  }
}

void lptim_v2::capture_enable_changed(std::size_t channel, bool on, std::uint64_t ps)
{
  if(not on)
  {
    state_.capture_off_ps[channel] = ps + 1;
    return;
  }
  auto const off = std::exchange(state_.capture_off_ps[channel], 0);
  auto const hz  = ctx_.clock.hz(kernel_);
  if(off == 0 or hz == 0)
  {
    return;
  }
  auto const needed = wide{3u << get(regs_, prescaler_)} * ps_per_second;
  if(wide{ps - (off - 1)} * hz >= needed)
  {
    state_.overcapture_lost[channel] = 0;
  }
}

void lptim_v2::input_changed(std::uint64_t ps, bool level, bool second)
{
  auto const live = enabled() and (second ? encoder() : external());
  if(filtered_change(line_of(second), ps, level, get(regs_, clock_filter_), live))
  {
    input_edge(ps, level, second);
    sync_out(ps);
  }
}

void lptim_v2::input_edge(std::uint64_t ps, bool rising, bool second)
{
  if(state_.held != 0 or not active(rising))
  {
    return;
  }
  if(clocked_by_input())
  {
    clock_edge(ps);
    return;
  }
  if(state_.hz == 0 or state_.running == 0 or wide{ps} * state_.hz < anchor())
  {
    return;
  }
  if(encoder())
  {
    encoder_edge(second, rising);
    return;
  }
  count_edge();
}

void lptim_v2::encoder_edge(bool second, bool rising)
{
  auto const opposite = (second ? state_.input_filtered : state_.input2_filtered) != 0;
  auto const up       = second ? rising == opposite : rising != opposite;
  if(up == (state_.down != 0))
  {
    state_.down = up ? 0 : 1;
    raise_flag(up ? counting_up : counting_down);
  }
  count_edge(up);
}

void lptim_v2::clock_edge(std::uint64_t ps)
{
  if(state_.enable_left != 0)
  {
    --state_.enable_left;
    return;
  }
  auto const dues  = std::to_array<std::pair<std::uint64_t*, term>>({
      {&state_.compare_due,    term::compare_written   },
      {&state_.compare2_due,   term::compare2_written  },
      {&state_.reload_due,     term::reload_written    },
      {&state_.interrupts_due, term::interrupts_written},
      {&state_.repetition_due, term::repetition_written},
      {&state_.reset_due,      term::counter_reset     },
      {&state_.switch_due,     term::mode_switch       },
  });
  auto       ready = std::array<bool, dues.size()>{};
  for(std::size_t index = 0; index < dues.size(); ++index)
  {
    auto& due = *dues[index].first;
    if(due != 0)
    {
      due -= std::min<std::uint64_t>(due, ps_per_second);
      ready[index] = due == 0;
    }
  }
  if(state_.running != 0 and state_.paused_units != 0)
  {
    state_.paused_units -= std::min<std::uint64_t>(state_.paused_units, ps_per_second);
  }
  else if(state_.running != 0 and ++state_.divided >= (1u << get(regs_, prescaler_)))
  {
    state_.divided = 0;
    count_edge();
  }
  for(std::size_t index = 0; index < dues.size(); ++index)
  {
    if(ready[index])
    {
      apply(due_term{ps, dues[index].second});
    }
  }
}

void lptim_v2::count_edge(bool up)
{
  if(state_.update_pending != 0)
  {
    state_.update_pending = 0;
    raise_flag(update_event);
  }
  auto const count   = state_.anchor_count;
  auto const leaving = [&](std::uint32_t value, std::uint32_t flag, bool gate) {
    if(gate and count == value and value <= state_.reload and (state_.isr & flag) == 0)
    {
      raise_flag(flag);
    }
  };
  leaving(state_.compare, compare_match, not capturing(0));
  leaving(
      state_.compare2, compare2_match,
      variant_.channels != 0 and get(regs_, channel_select_[1]) == 0);
  leaving(state_.reload, reload_match, true);
  auto const wrap = up ? count == state_.reload or count == counter_top : count == 0;
  if(not wrap)
  {
    state_.anchor_count = up ? count + 1u : count - 1u;
    return;
  }
  auto const update = state_.repetition == 0;
  if(update)
  {
    state_.repetition     = state_.repetition_reload;
    state_.update_pending = 1;
    transfer_preloaded();
  }
  else
  {
    --state_.repetition;
  }
  state_.anchor_count = up ? 0u : state_.reload;
  if(update and state_.single != 0)
  {
    stop();
  }
}

void lptim_v2::on_event(std::uint32_t kind, std::uint64_t payload)
{
  if(kind != tick_event or payload != state_.generation)
  {
    return;
  }
  state_.queued = 0;
  catch_up(now_ps());
  refresh();
  reschedule();
}
}

auto lptim_v2_factory(lptim_v2_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<lptim_v2>(ctx, desc, variant);
  };
}

namespace
{
constexpr auto lptim1_instance    = std::to_array<std::string_view>({"LPTIM1"});
constexpr auto lptim2_instance    = std::to_array<std::string_view>({"LPTIM2"});
constexpr auto lptim3_instance    = std::to_array<std::string_view>({"LPTIM3"});
constexpr auto lptim4_instance    = std::to_array<std::string_view>({"LPTIM4"});
constexpr auto lptim356_instances = std::to_array<std::string_view>({"LPTIM3", "LPTIM5", "LPTIM6"});
constexpr auto lptim_v2_table     = std::to_array<builtin>({
    row_of<^^lptim_v2_factory, ^^u5_lptim1>(
        {.ip              = "LPTIM",
         .ip_version_glob = "U5_lptimer1_v2*",
         .instance_globs  = lptim1_instance,
         .families        = family::u5}),
    row_of<^^lptim_v2_factory, ^^u5_lptim2>(
        {.ip              = "LPTIM",
         .ip_version_glob = "U5_lptimer1_v2*",
         .instance_globs  = lptim2_instance,
         .families        = family::u5}),
    row_of<^^lptim_v2_factory, ^^u5_lptim3>(
        {.ip              = "LPTIM",
         .ip_version_glob = "U5_lptimer1_v2*",
         .instance_globs  = lptim3_instance,
         .families        = family::u5}),
    row_of<^^lptim_v2_factory, ^^u5_lptim4>(
        {.ip              = "LPTIM",
         .ip_version_glob = "U5_lptimer1_v2*",
         .instance_globs  = lptim4_instance,
         .families        = family::u5}),
    row_of<^^lptim_v2_factory, ^^h5_lptim1>(
        {.ip              = "LPTIM",
         .ip_version_glob = "H5_lptimer1_v2*",
         .instance_globs  = lptim1_instance,
         .families        = family::h5}),
    row_of<^^lptim_v2_factory, ^^h5_lptim2>(
        {.ip              = "LPTIM",
         .ip_version_glob = "H5_lptimer1_v2*",
         .instance_globs  = lptim2_instance,
         .families        = family::h5}),
    row_of<^^lptim_v2_factory, ^^h5_lptim_full>(
        {.ip              = "LPTIM",
         .ip_version_glob = "H5_lptimer1_v2*",
         .instance_globs  = lptim356_instances,
         .families        = family::h5}),
    row_of<^^lptim_v2_factory, ^^h5_lptim4>(
        {.ip              = "LPTIM",
         .ip_version_glob = "H5_lptimer1_v2*",
         .instance_globs  = lptim4_instance,
         .families        = family::h5}),
});
}

auto lptim_v2_rows() -> std::span<builtin const>
{
  return lptim_v2_table;
}
}
