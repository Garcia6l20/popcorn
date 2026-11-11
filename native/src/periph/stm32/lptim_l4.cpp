#include "lptim_l4_rows.hpp"
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

namespace popcorn::periph::stm32
{
namespace
{
using wide = unsigned __int128;

constexpr std::uint32_t compare_match    = 1u << 0;
constexpr std::uint32_t reload_match     = 1u << 1;
constexpr std::uint32_t compare_ok       = 1u << 3;
constexpr std::uint32_t external_trigger = 1u << 2;
constexpr std::uint32_t reload_ok        = 1u << 4;
constexpr std::uint32_t counting_up      = 1u << 5;
constexpr std::uint32_t counting_down    = 1u << 6;
constexpr std::uint32_t counter_top      = 0xFFFF;
constexpr std::uint32_t enable_bit       = 1u << 0;
constexpr std::uint32_t single_start     = 1u << 1;
constexpr std::uint32_t continuous_run   = 1u << 2;
constexpr std::uint32_t tick_event       = 0;
constexpr std::size_t   unmodelled_uses  = 1;
constexpr std::uint8_t  compare_loaded   = 1u << 0;
constexpr std::uint8_t  reload_loaded    = 1u << 1;

enum class locked : std::uint8_t
{
  interrupts,
  configuration,
  option,
};

enum class term : std::uint8_t
{
  compare_written,
  reload_written,
  mode_switch,
  compare,
  reload,
  wrap,
  output,
  trigger,
};

struct lptim_state
{
  std::uint64_t anchor_ps{};
  std::uint64_t anchor_frac{};
  std::uint64_t hz{};
  std::uint64_t enable_ps{};
  std::uint64_t settled_ps{};
  std::uint64_t floor_ps{};
  std::uint64_t compare_held{};
  std::uint64_t reload_held{};
  std::uint64_t wrap_held{};
  std::uint64_t compare_due{};
  std::uint64_t reload_due{};
  std::uint64_t switch_due{};
  std::uint64_t paused_units{};
  std::uint64_t generation{};
  std::uint64_t queued{};
  std::uint64_t out_ps{};
  std::uint64_t trigger_due{};
  std::uint32_t anchor_count{};
  std::uint32_t isr{};
  std::uint32_t armed{};
  std::uint32_t compare{};
  std::uint32_t reload{1};
  std::uint32_t compare_pending{};
  std::uint32_t reload_pending{};
  std::uint32_t compare_preload{};
  std::uint32_t reload_preload{};
  std::uint8_t  running{};
  std::uint8_t  single{};
  std::uint8_t  paused_counting{};
  std::uint8_t  switch_single{};
  std::uint8_t  preloaded{};
  std::uint8_t  out{};
  std::uint8_t  polarity{};
  std::uint8_t  latched{};
  std::uint8_t  started_once{};
  std::uint8_t  held{};
  std::uint8_t  armed_mode{};
  std::uint8_t  trigger_level{};
  std::uint8_t  trigger_rising{};
  std::uint8_t  divided{};
  std::uint16_t enable_left{};
  std::uint8_t  in1_level{};
  std::uint8_t  in2_level{};
  std::uint8_t  down{};

  friend auto operator==(lptim_state const&, lptim_state const&) -> bool = default;
};

struct unmodelled_use
{
  std::string_view reg;
  std::string_view field;
  std::string_view what;
};

constexpr std::array<unmodelled_use, unmodelled_uses> unmodelled_table{
    {
     {"OR", "", "OR_0 or OR_1, the COMP1 and COMP2 inputs"},
     }
};

/**
 * @brief LPTIM of the STM32L4, RM0351 section 34, clocked by RCC's `lptim1` or `lptim2` domain
 *        from CCIPR. The 16-bit counter is derived from the kernel clock, the prescaler and the
 *        virtual time of its last anchor, never ticked. CNTSTRT and SNGSTRT written with ENABLE
 *        already 1 start it from 0 at the later of the write plus the variant's start cycles,
 *        RM0351 34.4.6, and ENABLE plus two prescaled clocks, RM0351 34.4.12, the second term and
 *        its prescaled reading inferred. A write setting them with ENABLE 0 is discarded, RM0351
 *        34.4.6, also when the same write sets ENABLE, inferred. They read 0 at once, inferred.
 *        Continuous mode counts 0 to ARR and wraps, one-shot mode stops at 0 at the wrap after
 *        ARR, RM0351 34.4.7. CMPM and ARRM set the variant's event delay after the counter
 *        reaches CMP and ARR, one kernel clock on the STM32L4 by ES0250 2.18.3, also with PRESC
 *        above 0, inferred. CMP above ARR never matches, and a counter above a lowered ARR runs
 *        to 0xFFFF before it wraps, inferred. A CMP or ARR write with ENABLE 1 sets CMPOK or
 *        ARROK, and takes effect, the variant's OK cycles later, 3 kernel clocks inferred:
 *        RM0351 34.4.10 states a latency with no number. Counter ticks up to the OK compare with
 *        the old values, and a match of such a tick still sets its flag at its due after the OK,
 *        inferred. An OK at the one-shot wrap keeps the stop at the wrap, inferred. Terms due at
 *        the same time apply in the order CMPOK, ARROK, CMPM, ARRM, wrap, mode switch,
 *        inferred.
 *        A flag that sets while its IER bit is 1 arms the LPTIM IRQ until ICR clears it, so an
 *        IER bit set after its flag raises nothing, RM0351 34.6. ENABLE written to 0 stops the
 *        counter at 0 and keeps the flags and a pending CMP or ARR write, inferred.
 *        RM0351 34.4.12 and 34.7 allow CFGR, IER and OR writes only with ENABLE 0 and CMP and ARR
 *        writes only with ENABLE 1, and RM0351 34.4.10 a second CMP or ARR write only after its
 *        OK flag. The model keeps the register for a write outside its rule, the second write
 *        excepted, which replaces the pending value: each logs one model_warning and raises
 *        model_error under strict_peripherals, inferred. With PRELOAD 1 and the counter
 *        started, a CMP or ARR value takes effect at the first wrap at or after its OK flag,
 *        RM0351 34.4.10, the wrap being the tick after ARR, inferred. The counter counts as
 *        started from the CNTSTRT or SNGSTRT write, its start latency included, inferred. A
 *        later write waits for its own OK and wrap. ENABLE written to 0 and the one-shot stop
 *        apply such a value at once, inferred. SNGSTRT written while the counter
 *        runs in continuous mode makes it stop at a later wrap, and CNTSTRT written in one-shot
 *        mode makes it go on counting, RM0351 34.4.7. The new mode reaches the counter the
 *        variant's start cycles after the write, inferred, so a wrap before then keeps the old
 *        mode, and a CNTSTRT reaching a counter that the one-shot wrap stopped starts it. The
 *        same mode written again while the counter runs is discarded, inferred. A second
 *        CNTSTRT or SNGSTRT before the first reaches the counter replaces it, logs one
 *        model_warning and raises model_error under strict_peripherals, inferred. A start with
 *        a CFGR or OR function the model lacks, the COMP inputs, the RTC and COMP triggers of
 *        TRIGSEL above 0, a PRESC above 0 with COUNTMODE 1 or ENC 1, which RM0351 34.4.11 and
 *        34.4.13 forbid, ENC 1 with CKSEL 1, and with CKSEL 1, COUNTMODE 1 or ENC 1 CKPOL 11,
 *        CKPOL 10 with CKSEL 1 or a CKFLT above 0, logs once each and raises under
 *        strict_peripherals: the counter then counts the kernel clock, or input 1 edges as
 *        configured. With CKSEL 1 or COUNTMODE 1 the counter counts the active edges CKPOL
 *        selects on the `<instance>_in1` pin signal instead of the kernel clock, RM0351 34.4.11.
 *        With CKSEL 1 the active edges of input 1 clock the kernel logic, RM0351 34.4.3 and
 *        34.4.11: CMPOK, ARROK, the start, a mode switch and the trigger filter and start count
 *        input 1 edges, the ENABLE latency 2 edges, the OK and start latencies 3 edges and a
 *        trigger start 2 counter clocks, inferred counts that give the five lost edges of RM0351
 *        34.4.11 after an ENABLE and a CNTSTRT for every PRESC. PRESC divides the edges before
 *        the counter, RM0351 34.4.5. A flag sets at the edge, inferred. With
 *        COUNTMODE 1 every edge counts and a flag sets the variant's event delay after the edge,
 *        the sampling on the kernel clock taken as the edge itself, inferred, and an edge with
 *        the kernel clock stopped is dropped, inferred. An edge during a debug hold is dropped
 *        and the output follows CNT at each edge, inferred. With ENC 1, on LPTIM1 only, RM0351
 *        34.3, the counter counts the `<instance>_in1` and `<instance>_in2` edges CKPOL selects
 *        up or down by the level of the other input, RM0351 34.4.13 Table 220, between 0 and
 *        ARR: counting down from 0 gives ARR. A change of direction sets DOWN or UP, RM0351
 *        34.7.1, at the edge, and the count starts upwards, inferred. CMPM and ARRM set the
 *        variant's event delay after the edge that reaches CMP or ARR in either direction, the
 *        sampling taken as the edge itself, inferred. An edge during a debug hold or with the
 *        kernel clock stopped is dropped, inferred. LPTIMxRST takes the input pins' levels.
 *        With PRELOAD 1 a pending CMP or ARR applies at a down wrap from 0 as at an up wrap,
 *        and that wrap loads the new ARR, inferred: RM0351 34.4.10 names only the end of the
 *        period. SNGSTRT with ENC 1 logs one model_warning and raises model_error under
 *        strict_peripherals, RM0351 34.4.13 asking for continuous mode, and the one-shot stop
 *        then applies at an up wrap.
 *        With TRIGEN not 00 and TRIGSEL 0 the `<instance>_etr` pin signal starts the counter on
 *        the edges TRIGEN selects, RM0351 34.4.6: CNTSTRT or SNGSTRT arm the start and its mode,
 *        EXTTRIG sets at the edge and the count starts two prescaled clocks later. With TRGFLT
 *        not 00 a level change counts once the pin has held it for 2, 4 or 8 kernel clocks, a
 *        shorter pulse is rejected, RM0351 34.4.4 and 34.7.4, the sampling phase taken as the
 *        edge itself, inferred. An edge during a debug hold or with the kernel clock stopped
 *        starts nothing, and the level it leaves becomes the filtered level, inferred.
 *        LPTIMxRST takes the pin's level as the filtered level, inferred. A trigger while
 *        counting is ignored with no flag, RM0351 34.4.6, or with TIMOUT 1 sets EXTTRIG and
 *        restarts the count from 0 two prescaled clocks later, RM0351 34.4.8, the latency
 *        inferred. After a one-shot stop the next trigger starts a new run, RM0351 34.4.7, only
 *        the first with WAVE 1. A trigger before the arm does nothing, and a trigger term
 *        applies after any other term due at the same time, inferred.
 *        A kernel rate change keeps CNT, the kernel clocks already spent in the current tick or
 *        the start latency, and the kernel clocks left before each pending CMPOK, ARROK, match
 *        delay, mode switch and one-shot stop, inferred. A stopped kernel clock freezes all of
 *        them until it runs again, inferred. LPTIMxRST resets the registers and the state and
 *        withdraws the pending event.
 *        The output drives the `<instance>_out` trigger signal, `lptim1_out` and `lptim2_out`, and
 *        no pin. With WAVE 0 it rises one kernel clock after CNT reaches CMP and falls one kernel
 *        clock after CNT reaches ARR, as ES0250 2.18.3 Figure 3 draws it, with PRESC above 0
 *        inferred, RM0351 34.4.9 reads "exceeds". A one-shot run gives one such pulse, RM0351
 *        34.4.9. With WAVE 1 it rises once and keeps its level until ENABLE is written to 0, and a
 *        start after the first one is discarded until then, RM0351 34.4.7, the software start
 *        reading inferred. CMP at or above ARR keeps it inactive, inferred. WAVPOL inverts it at
 *        the CFGR write, also with ENABLE 0, RM0351 34.4.9. ENABLE written to 0 and LPTIMxRST drop
 *        it to the inactive level at the write, inferred. A sink that starts listening sees the
 *        edges from then on. After a snapshot restore the output asks the sinks again at its first
 *        catch-up. While the machine holds its core with DBG_LPTIM1_STOP or DBG_LPTIM2_STOP
 *        set, its counter clock stops, RM0351 34.4.14, 48.16.4 and 48.16.5, and the model
 *        freezes it as a stopped kernel clock: CNT, the kernel clocks spent in the current tick
 *        and those left before each pending term are kept and resume at the release, the
 *        latencies inferred. A kernel rate change during the hold applies from the release,
 *        inferred. The hold is kept across LPTIMxRST, RM0351 34.4.14. At a machine reset the
 *        LPTIM reads DBG_LPTIMx_STOP again, which a system reset keeps and a power-on reset
 *        clears, RM0351 48.16.4 and 48.16.5.
 *        Deviation: CNT reads exact, without the double-read rule of RM0351 34.7.8.
 *
 * @limits lptim LPTIM
 *
 * @group l4 rule "STM32L4 LPTIM1 and LPTIM2, RM0351 section 34"
 * STM32L4 LPTIM1 and LPTIM2, RM0351 section 34, count the kernel clock of RCC_CCIPR LPTIM1SEL or
 * LPTIM2SEL through the prescaler, started by CNTSTRT or SNGSTRT.
 *
 * - The counter starts at the later of the CNTSTRT or SNGSTRT write plus 3 kernel clocks, RM0351
 *   34.4.6, and the ENABLE write plus two prescaled clocks, inferred from RM0351 34.4.12. A start
 *   write made with ENABLE 0 is discarded, also when the same write sets ENABLE, inferred. CNTSTRT
 *   and SNGSTRT read 0 at once, inferred.
 * - CMPM and ARRM set one kernel clock after the counter reaches CMP or ARR, ES0250 2.18.3, also
 *   with PRESC above 0, inferred. CMP above ARR never matches, and a counter above a lowered ARR
 *   counts to 0xFFFF before it wraps, inferred.
 * - CMPOK and ARROK set 3 kernel clocks after the CMP or ARR write, and the value applies then,
 *   inferred: RM0351 34.4.10 states a latency with no number. Counter ticks up to the OK compare
 *   with the old values, and a match of such a tick still sets its flag at its due after the OK,
 *   inferred. An OK at the one-shot wrap keeps the stop at the wrap, inferred. Terms due at the
 *   same time apply in the order CMPOK, ARROK, CMPM, ARRM, wrap, mode switch, inferred.
 * - With PRELOAD 1 and the counter started, a CMP or ARR value takes effect at the first wrap at or
 *   after its OK flag, RM0351 34.4.10, the wrap being the tick after ARR, inferred. The counter
 *   counts as started from the CNTSTRT or SNGSTRT write, its start latency included, inferred. A
 *   later write waits for its own OK and wrap. ENABLE written to 0 and the one-shot stop apply such
 *   a value at once, inferred.
 * - SNGSTRT written while the counter runs in continuous mode makes it stop at a later wrap, and
 *   CNTSTRT written in one-shot mode makes it go on counting, RM0351 34.4.7. The new mode reaches
 *   the counter 3 kernel clocks after the write, inferred, so a wrap before then keeps the old
 *   mode, and a CNTSTRT reaching a counter that the one-shot wrap stopped starts it. The same mode
 *   written again while the counter runs is discarded, inferred. A second CNTSTRT or SNGSTRT before
 *   the first reaches the counter replaces it, logs one `model_warning` and raises `ModelError`
 *   under `strict`, inferred.
 * - ENABLE written to 0 stops the counter at 0 and keeps the flags and a pending CMP or ARR write,
 *   inferred.
 * - A flag that sets while its IER bit is 1 arms the LPTIM IRQ until ICR clears it, so an IER bit
 *   set after its flag raises nothing, RM0351 34.6.
 * - An IER, CFGR or OR write with ENABLE 1, a CMP or ARR write with ENABLE 0, and a second CMP or
 *   ARR write before its OK flag log one `model_warning` each and raise `ModelError` under
 *   `strict`. The register keeps its value, except the second write, whose value replaces the
 *   pending one, inferred.
 * - A start with OR_0 or OR_1 set, TRIGSEL not 0 with TRIGEN not 00, PRESC above 0 with COUNTMODE 1
 *   or ENC 1, which RM0351 34.4.11 and 34.4.13 forbid, ENC 1 with CKSEL 1, or with CKSEL 1,
 *   COUNTMODE 1 or ENC 1 CKPOL 11, CKPOL 10 with CKSEL 1 or a CKFLT above 0, logs one
 *   `model_warning` each and raises `ModelError` under `strict`. The counter then counts the kernel
 *   clock, or input 1 edges as configured, and with TRIGSEL not 0 no trigger starts it.
 * - The COMP inputs of OR_0 and OR_1 and the RTC and COMP trigger inputs of TRIGSEL above 0 are
 *   not modelled, each logged once and raised under `strict` at the use.
 * - With CKSEL 1 or COUNTMODE 1 the counter counts the active edges CKPOL selects on the
 *   `lptim1_in1` and `lptim2_in1` pin signals, PB5 or PC0 AF1 and PB1 or PC0 AF14 on the L476,
 *   RM0351 34.4.11 and 34.7.4. With CKSEL 1 the input 1 edges clock the kernel logic, RM0351 34.4.3
 *   and 34.4.11, so CMPOK, ARROK, the start, a mode switch and a trigger count edges and no
 *   internal clock: the ENABLE latency 2 edges, the OK and start latencies 3 edges and a trigger
 *   start 2 counter clocks, inferred counts that give the five lost edges of RM0351 34.4.11 after
 *   an ENABLE and a CNTSTRT for every PRESC. PRESC divides the edges before the counter, RM0351
 *   34.4.5, and CMPM and ARRM set at the edge, inferred. With COUNTMODE 1 every active edge counts
 *   and the flags set one kernel clock after it, the kernel clock sampling taken as the edge
 *   itself, inferred, and an edge with the kernel clock stopped is dropped, inferred. An edge
 *   during a debug hold is dropped, and `lptim1_out` follows CNT at each edge, inferred.
 * - With ENC 1, LPTIM1 only, RM0351 34.3 Table 216, the counter counts the `lptim1_in1` and
 *   `lptim1_in2` edges CKPOL selects, PB5 or PC0 and PB7 or PC2 AF1 on the L476, up or down by the
 *   level of the other input, RM0351 34.4.13 Table 220, between 0 and ARR: counting down from 0
 *   gives ARR. A change of direction sets DOWN or UP, RM0351 34.7.1, at the edge, and the count
 *   starts upwards, inferred. CMPM and ARRM set one kernel clock after the edge that reaches CMP or
 *   ARR in either direction, inferred. An edge during a debug hold or with the kernel clock stopped
 *   is dropped, inferred. LPTIMxRST takes the input pins' levels. With PRELOAD 1 a pending CMP or
 *   ARR applies at a down wrap from 0 as at an up wrap, and that wrap loads the new ARR, inferred:
 *   RM0351 34.4.10 names only the end of the period. SNGSTRT with ENC 1 logs one `model_warning`
 *   and raises `ModelError` under `strict`, RM0351 34.4.13 asking for continuous mode, and the
 *   one-shot stop then applies at an up wrap.
 * - With TRIGEN not 00 and TRIGSEL 0, the `lptim1_etr` and `lptim2_etr` pin signals, PB6 or PC3 AF1
 *   and PA5 or PC3 AF14 on the L476, start the counter on the edge TRIGEN selects, RM0351 34.4.6
 *   and 34.7.4. CNTSTRT or SNGSTRT arm the start and choose the mode. EXTTRIG sets at the edge, and
 *   the counter starts two prescaled clocks after it, RM0351 34.4.6. With TRGFLT 01, 10 or 11 a
 *   level change counts once the pin has held it for 2, 4 or 8 kernel clocks, and a shorter pulse
 *   is rejected, RM0351 34.4.4 and 34.7.4: EXTTRIG and the start move by that many clocks, the
 *   sampling phase taken as the edge itself, inferred. An edge during a debug hold or with the
 *   kernel clock stopped starts nothing, and the level it leaves becomes the filtered level, since
 *   the filter samples on the kernel clock, inferred. LPTIMxRST takes the pin's level as the
 *   filtered level, inferred. A trigger while the counter runs is ignored and sets no EXTTRIG,
 *   RM0351 34.4.6 and 34.7.1, unless TIMOUT is 1: it then sets EXTTRIG and restarts the count from
 *   0 two prescaled clocks later, RM0351 34.4.8, the latency inferred. In one-shot mode each
 *   trigger after the stop starts a new run, RM0351 34.4.7, and with WAVE 1 only the first trigger
 *   starts one. A trigger before CNTSTRT or SNGSTRT starts nothing and sets no EXTTRIG, inferred. A
 *   trigger at the time of another term applies after it, inferred.
 * - CNT reads the exact count, so the double read of RM0351 34.7.8 always agrees.
 * - A kernel rate change, such as an LPTIM1SEL write or a PPRE1 change under PCLK1, keeps CNT, the
 *   kernel clocks already spent in the current tick or the start latency, and the kernel clocks
 *   left before each pending CMPOK, ARROK, CMPM, ARRM, mode switch and one-shot stop, inferred. A
 *   stopped kernel clock, LPTIMxEN 0 included, freezes all of them until it runs again, inferred.
 * - LPTIM1RST and LPTIM2RST reset the registers and the state of their LPTIM.
 * - The output drives the `lptim1_out` and `lptim2_out` trigger signals and no pin, so ES0250
 *   2.18.4, LPTIM1 outputs not open-drain, is outside the model. With WAVE 0 it rises one kernel
 *   clock after CNT reaches CMP and falls one kernel clock after CNT reaches ARR, as ES0250 2.18.3
 *   Figure 3 draws it, with PRESC above 0 inferred, RM0351 34.4.9 reads "exceeds". A one-shot run
 *   gives one such pulse, RM0351 34.4.9. With WAVE 1 it rises once and keeps its level until ENABLE
 *   is written to 0, and a start after the first one is discarded until then, RM0351 34.4.7, the
 *   software start reading inferred. CMP at or above ARR keeps it inactive, inferred. WAVPOL
 *   inverts it at the CFGR write, also with ENABLE 0, RM0351 34.4.9. ENABLE written to 0 and
 *   LPTIMxRST drop it to the inactive level at the write, inferred.
 * - While the core is halted with DBG_LPTIM1_STOP or DBG_LPTIM2_STOP set, that LPTIM's counter
 *   clock stops, RM0351 34.4.14, 48.16.4 and 48.16.5. The model freezes it as a stopped kernel
 *   clock: CNT, the kernel clocks spent in the current tick and those left before each pending
 *   CMPOK, ARROK, match delay, mode switch and stop resume at the release, the latencies inferred.
 *   A kernel rate change during the hold applies from the release, inferred. The hold is kept
 *   across LPTIMxRST, RM0351 34.4.14. At a machine reset the LPTIM reads DBG_LPTIMx_STOP again,
 *   which a system reset keeps and a power-on reset clears, RM0351 48.16.4 and 48.16.5.
 * - EXTI direct lines 32 and 33 stay Stop wake-up gates: the LPTIM raises LPTIM1_IRQn and
 *   LPTIM2_IRQn itself, and Stop is not modelled. ES0250 2.18.1 and 2.18.2, an LPTIM interrupt
 *   stuck at Stop entry or at a flag clear racing a new event, are outside the model.
 */
class lptim final : public dispatch_model<lptim, lptim_state>,
                    public clock_listener,
                    public bus_reset_listener,
                    public core_hold_listener,
                    public trigger_source,
                    public trigger_sink
{
public:
  lptim(model_context& ctx, peripheral_desc const& desc, lptim_variant const& variant);

  void               reset(register_file& regs, reset_kind kind) override;
  void               load(std::span<std::byte const> in) override;
  void               on_bus_reset() override;
  void               on_clock_change() override;
  void               on_core_hold(bool held) override;
  void               interest_changed() override;
  [[nodiscard]] auto produces(std::string_view signal) const -> bool override;
  [[nodiscard]] auto listens(std::string_view signal) const -> bool override;
  [[nodiscard]] auto can_listen(std::string_view signal) const -> bool override;
  [[nodiscard]] auto takes_pin(std::string_view signal) const -> bool override;
  void triggered(std::string_view signal, trigger_edge edge, trigger_time at) override;
  [[nodiscard]] auto level(std::string_view signal) const -> std::optional<bool> override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
      -> std::expected<std::uint32_t, bus_error> override;

  void                         plain_written(std::uint32_t old, std::uint32_t now);
  void                         clear_written(std::uint32_t old, std::uint32_t now);
  template <locked Which> void locked_written(std::uint32_t old, std::uint32_t now);
  void                         control_written(std::uint32_t old, std::uint32_t now);
  template <term Which> void   value_written(std::uint32_t old, std::uint32_t now);

private:
  struct due_term
  {
    std::uint64_t due;
    term          which;
  };

  [[nodiscard]] auto now_ps() const -> std::uint64_t;
  [[nodiscard]] auto kernel_hz() const -> std::uint64_t;
  [[nodiscard]] auto enabled() const -> bool;
  [[nodiscard]] auto anchor() const -> wide;
  [[nodiscard]] auto tick_unit() const -> wide;
  [[nodiscard]] auto first_wrap() const -> std::uint64_t;
  [[nodiscard]] auto position(std::uint64_t ticks) const -> std::uint32_t;
  [[nodiscard]] auto ticks_at(std::uint64_t ps) const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto count_at(std::uint64_t ps) const -> std::uint32_t;
  [[nodiscard]] auto ceil_ps(wide at) const -> std::uint64_t;
  [[nodiscard]] auto ticks_to(std::uint32_t value) const -> std::uint64_t;
  [[nodiscard]] auto match_at(std::uint32_t value) const -> std::optional<wide>;
  [[nodiscard]] auto wave() const -> bool;
  [[nodiscard]] auto raw_at(std::uint64_t ps) const -> bool;
  [[nodiscard]] auto out_due() const -> std::optional<std::uint64_t>;
  [[nodiscard]] auto match_due(std::uint32_t value, std::uint64_t held) const
      -> std::optional<std::uint64_t>;
  [[nodiscard]] auto next_term() const -> std::optional<due_term>;

  void               set_anchor(wide at);
  void               hold_in_flight(std::uint64_t ps);
  void               reanchor(std::uint64_t ps, std::uint64_t ticks);
  void               align_wrap(std::uint64_t ps);
  void               catch_up(std::uint64_t ps);
  void               apply(due_term const& next);
  void               raise_flag(std::uint32_t flag);
  [[nodiscard]] auto preload() const -> bool;
  [[nodiscard]] auto trigger_mode() const -> bool;
  [[nodiscard]] auto pin_level(std::string_view signal) const -> bool;
  [[nodiscard]] auto external() const -> bool;
  void               input_edge(std::uint64_t ps, bool rising);
  void               clock_edge(std::uint64_t ps);
  void               count_edge(std::uint64_t ps, bool sampled, bool up = true);
  [[nodiscard]] auto encoder() const -> bool;
  void               encoder_edge(std::uint64_t ps, bool second, bool rising);
  void               edge_start(std::uint64_t units, bool single);
  void               refuse_clock();
  void               start(std::uint64_t ps, bool single);
  void               begin(wide at, std::uint64_t ps, bool single);
  void               switch_mode(std::uint64_t ps, bool single);
  void               transfer_preloaded();
  void               stop();
  void               refuse_unmodelled();
  void               refresh();
  void               reschedule();
  void               rebase(std::uint64_t ps, std::uint64_t hz);
  void               restore_reset_values();
  void               sync_out(std::uint64_t ps);

  lptim_variant                     variant_;
  register_file&                    regs_;
  reg_ref                           isr_;
  reg_ref                           ier_;
  reg_ref                           cfgr_;
  reg_ref                           cr_;
  reg_ref                           cmp_;
  reg_ref                           arr_;
  reg_ref                           cnt_;
  field_ref                         prescaler_;
  field_ref                         clock_select_;
  field_ref                         count_mode_;
  field_ref                         clock_polarity_;
  field_ref                         clock_filter_;
  field_ref                         encoder_;
  field_ref                         preload_;
  field_ref                         wave_;
  field_ref                         polarity_;
  field_ref                         trigger_enable_;
  field_ref                         trigger_select_;
  field_ref                         trigger_filter_;
  field_ref                         timeout_;
  core::clock_domain                kernel_;
  irq_line*                         line_ = nullptr;
  trigger_output                    out_;
  std::string                       etr_;
  std::string                       in1_;
  std::string                       in2_;
  std::array<bool, unmodelled_uses> unmodelled_logged_{};
  std::array<bool, 3>               locked_logged_{};
  bool                              value_logged_   = false;
  bool                              pending_logged_ = false;
  bool                              switch_logged_  = false;
  bool                              single_logged_  = false;
  bool                              freeze_logged_  = false;
  bool                              select_logged_  = false;
  std::array<bool, 5>               clock_logged_{};
  std::optional<bool>               listened_;
};

constexpr std::array<write_entry<lptim>, 8> lptim_writes{
    {
     {"ISR", &lptim::plain_written},
     {"ICR", &lptim::clear_written},
     {"IER", &lptim::locked_written<locked::interrupts>},
     {"CFGR", &lptim::locked_written<locked::configuration>},
     {"CR", &lptim::control_written},
     {"CMP", &lptim::value_written<term::compare_written>},
     {"ARR", &lptim::value_written<term::reload_written>},
     {"CNT", &lptim::plain_written},
     }
};

constexpr std::array<write_entry<lptim>, 1> lptim_option_writes{
    {{"OR", &lptim::locked_written<locked::option>}}};

lptim::lptim(model_context& ctx, peripheral_desc const& desc, lptim_variant const& variant)
    : dispatch_model{ctx, desc, "LPTIM", lptim_writes}, variant_{variant}, regs_{ctx.regs},
      isr_{reg("ISR")}, ier_{reg("IER")}, cfgr_{reg("CFGR")}, cr_{reg("CR")}, cmp_{reg("CMP")},
      arr_{reg("ARR")}, cnt_{reg("CNT")}, prescaler_{field("CFGR", "PRESC")},
      clock_select_{field("CFGR", "CKSEL")}, count_mode_{field("CFGR", "COUNTMODE")},
      clock_polarity_{field("CFGR", "CKPOL")}, clock_filter_{field("CFGR", "CKFLT")},
      encoder_{variant.encoder ? field("CFGR", "ENC") : field_ref{}},
      preload_{field("CFGR", "PRELOAD")}, wave_{field("CFGR", "WAVE")},
      polarity_{field("CFGR", "WAVPOL")}, trigger_enable_{field("CFGR", "TRIGEN")},
      trigger_select_{field("CFGR", "TRIGSEL")}, trigger_filter_{field("CFGR", "TRGFLT")},
      timeout_{field("CFGR", "TIMOUT")}, kernel_{ctx.clock.domain(lower(desc.name))},
      line_{&irq(desc.name + "_IRQn")}, out_{ctx.peers, lower(desc.name) + "_out"},
      etr_{lower(desc.name) + "_etr"}, in1_{lower(desc.name) + "_in1"},
      in2_{variant.encoder ? lower(desc.name) + "_in2" : std::string{}}
{
  expect_register("OR", variant.option_register);
  expect_field("CFGR", "ENC", variant.encoder);
  expect_field("ISR", "UP", variant.encoder);
  expect_field("ISR", "DOWN", variant.encoder);
  expect_field("IER", "UPIE", variant.encoder);
  expect_field("IER", "DOWNIE", variant.encoder);
  expect_field("ICR", "UPCF", variant.encoder);
  expect_field("ICR", "DOWNCF", variant.encoder);
  if(variant.option_register)
  {
    bind(lptim_option_writes);
  }
  state_.reload = describe("ARR").reset;
  state_.hz     = ctx.clock.hz(kernel_);
}

auto lptim::now_ps() const -> std::uint64_t
{
  return ctx_.clock.time().ps;
}

auto lptim::kernel_hz() const -> std::uint64_t
{
  return state_.held != 0 or get(regs_, clock_select_) != 0 ? 0 : ctx_.clock.hz(kernel_);
}

auto lptim::enabled() const -> bool
{
  return (regs_.peek(cr_.offset) & enable_bit) != 0;
}

auto lptim::anchor() const -> wide
{
  return wide{state_.anchor_ps} * state_.hz + state_.anchor_frac;
}

auto lptim::tick_unit() const -> wide
{
  return (wide{1} << get(regs_, prescaler_)) * ps_per_second;
}

void lptim::set_anchor(wide at)
{
  state_.anchor_ps   = static_cast<std::uint64_t>(at / state_.hz);
  state_.anchor_frac = static_cast<std::uint64_t>(at % state_.hz);
}

auto lptim::first_wrap() const -> std::uint64_t
{
  auto const from = state_.anchor_count;
  return from <= state_.reload ? state_.reload + 1u - from : counter_top + 1u - from;
}

auto lptim::position(std::uint64_t ticks) const -> std::uint32_t
{
  auto const wrap = first_wrap();
  if(ticks < wrap)
  {
    return static_cast<std::uint32_t>(state_.anchor_count + ticks);
  }
  return static_cast<std::uint32_t>((ticks - wrap) % (std::uint64_t{state_.reload} + 1u));
}

auto lptim::ticks_at(std::uint64_t ps) const -> std::optional<std::uint64_t>
{
  auto const at = wide{ps} * state_.hz;
  if(state_.running == 0 or state_.hz == 0 or external() or at < anchor())
  {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>((at - anchor()) / tick_unit());
}

auto lptim::count_at(std::uint64_t ps) const -> std::uint32_t
{
  auto const ticks = ticks_at(ps);
  return ticks ? position(*ticks) : state_.anchor_count;
}

auto lptim::ceil_ps(wide at) const -> std::uint64_t
{
  return static_cast<std::uint64_t>((at + state_.hz - 1) / state_.hz);
}

auto lptim::ticks_to(std::uint32_t value) const -> std::uint64_t
{
  auto const from   = state_.anchor_count;
  auto const period = std::uint64_t{state_.reload} + 1u;
  return from <= state_.reload ? (value >= from ? value - from : period - from + value)
                               : first_wrap() + value;
}

auto lptim::match_at(std::uint32_t value) const -> std::optional<wide>
{
  if(state_.running == 0 or state_.hz == 0 or external() or value > state_.reload)
  {
    return std::nullopt;
  }
  auto const first = ticks_to(value);
  auto const step  = wide{std::uint64_t{state_.reload} + 1u} * tick_unit();
  auto const floor = wide{state_.floor_ps} * state_.hz;
  auto       tick  = anchor() + wide{first} * tick_unit();
  if(tick <= floor)
  {
    tick += ((floor - tick) / step + 1) * step;
  }
  auto const settled = wide{state_.settled_ps} * state_.hz;
  auto       at      = tick + wide{variant_.event_delay_cycles} * ps_per_second;
  if(at <= settled)
  {
    at += ((settled - at) / step + 1) * step;
  }
  return at;
}

auto lptim::match_due(std::uint32_t value, std::uint64_t held) const -> std::optional<std::uint64_t>
{
  auto const at  = match_at(value);
  auto const due = at ? std::optional{ceil_ps(*at)} : std::nullopt;
  if(held != 0 and (not due or held < *due))
  {
    return held;
  }
  return due;
}

auto lptim::wave() const -> bool
{
  return get(regs_, wave_) != 0;
}

auto lptim::raw_at(std::uint64_t ps) const -> bool
{
  if(state_.latched != 0)
  {
    return true;
  }
  if(state_.running == 0)
  {
    return false;
  }
  if(external())
  {
    return state_.compare <= state_.anchor_count and state_.anchor_count < state_.reload;
  }
  if(state_.hz == 0)
  {
    return (state_.out ^ state_.polarity) != 0;
  }
  auto const at    = wide{ps} * state_.hz;
  auto const delay = wide{variant_.event_delay_cycles} * ps_per_second;
  if(at < anchor() + delay and state_.anchor_count == 0)
  {
    return false;
  }
  auto const count =
      at < anchor() + delay
          ? state_.anchor_count - 1
          : position(static_cast<std::uint64_t>((at - delay - anchor()) / tick_unit()));
  return state_.compare <= count and count < state_.reload;
}

auto lptim::out_due() const -> std::optional<std::uint64_t>
{
  if(state_.running == 0
     or state_.latched != 0
     or external()
     or not(listened_.value_or(false) or wave()))
  {
    return std::nullopt;
  }
  auto const step  = wide{std::uint64_t{state_.reload} + 1u} * tick_unit();
  auto const after = wide{state_.out_ps} * state_.hz;
  auto       best  = std::optional<std::uint64_t>{};
  for(auto const value : {state_.compare, state_.reload})
  {
    if(value > state_.reload)
    {
      continue;
    }
    auto at = anchor()
            + wide{ticks_to(value)} * tick_unit()
            + wide{variant_.event_delay_cycles} * ps_per_second;
    if(at <= after)
    {
      at += ((after - at) / step + 1) * step;
    }
    best = std::min(best.value_or(ceil_ps(at)), ceil_ps(at));
  }
  return best;
}

void lptim::sync_out(std::uint64_t ps)
{
  auto const raw = raw_at(ps);
  if(raw and wave() and state_.running != 0)
  {
    state_.latched = 1;
  }
  auto const level = static_cast<std::uint8_t>((raw ? 1u : 0u) ^ state_.polarity);
  if(level != state_.out)
  {
    state_.out = level;
    if(listened_.value_or(false))
    {
      out_.edge(level != 0 ? trigger_edge::rising : trigger_edge::falling, ps);
    }
  }
  state_.out_ps = std::max(state_.out_ps, ps);
}

auto lptim::next_term() const -> std::optional<due_term>
{
  auto best = std::optional<due_term>{};
  if(state_.hz == 0)
  {
    return best;
  }
  auto offer = [&](std::optional<std::uint64_t> due, term which) {
    if(due and *due != 0 and (not best or *due < best->due))
    {
      best = due_term{*due, which};
    }
  };
  offer(state_.compare_due, term::compare_written);
  offer(state_.reload_due, term::reload_written);
  if((state_.isr & compare_match) == 0)
  {
    offer(match_due(state_.compare, state_.compare_held), term::compare);
  }
  if((state_.isr & reload_match) == 0)
  {
    offer(match_due(state_.reload, state_.reload_held), term::reload);
  }
  if(state_.running != 0 and not external() and (state_.single != 0 or state_.preloaded != 0))
  {
    offer(
        state_.wrap_held != 0 ? state_.wrap_held
                              : ceil_ps(anchor() + wide{first_wrap()} * tick_unit()),
        term::wrap);
  }
  offer(state_.switch_due, term::mode_switch);
  offer(out_due(), term::output);
  offer(state_.trigger_due, term::trigger);
  return best;
}

void lptim::raise_flag(std::uint32_t flag)
{
  state_.isr |= flag;
  if(flag == compare_match)
  {
    state_.compare_held = 0;
  }
  if(flag == reload_match)
  {
    state_.reload_held = 0;
  }
  if((regs_.peek(ier_.offset) & flag) != 0)
  {
    state_.armed |= flag;
  }
}

void lptim::hold_in_flight(std::uint64_t ps)
{
  auto const now   = wide{ps} * state_.hz;
  auto const delay = wide{variant_.event_delay_cycles} * ps_per_second;
  auto const held  = [&](std::uint32_t flag, std::uint32_t value, std::uint64_t& out) {
    if((state_.isr & flag) != 0)
    {
      return;
    }
    if(auto const at = match_at(value); at and *at - delay <= now)
    {
      out = ceil_ps(*at);
    }
  };
  held(compare_match, state_.compare, state_.compare_held);
  held(reload_match, state_.reload, state_.reload_held);
}

void lptim::apply(due_term const& next)
{
  if(next.which == term::output)
  {
    return;
  }
  if(next.which == term::trigger)
  {
    state_.trigger_due   = 0;
    state_.trigger_level = state_.trigger_rising;
    auto const polarity  = get(regs_, trigger_enable_);
    auto const rising    = state_.trigger_rising != 0;
    if((polarity == 1 and not rising) or (polarity == 2 and rising))
    {
      return;
    }
    auto const at      = wide{next.due} * state_.hz + 2 * tick_unit();
    auto const restart = [&](bool single) {
      if(state_.hz == 0)
      {
        edge_start(static_cast<std::uint64_t>(2 * tick_unit()), single);
      }
      else
      {
        begin(at, next.due, single);
      }
    };
    if(state_.running != 0)
    {
      if(get(regs_, timeout_) == 0)
      {
        return;
      }
      raise_flag(external_trigger);
      hold_in_flight(next.due);
      restart(state_.single != 0);
      return;
    }
    if(state_.armed_mode == 0 or (wave() and state_.started_once != 0))
    {
      return;
    }
    raise_flag(external_trigger);
    restart(state_.armed_mode == 2);
    state_.started_once = 1;
    return;
  }
  if(next.which == term::compare or next.which == term::reload)
  {
    raise_flag(next.which == term::compare ? compare_match : reload_match);
    return;
  }
  if(next.which == term::wrap)
  {
    if(state_.single != 0)
    {
      stop();
      return;
    }
    set_anchor(anchor() + wide{first_wrap()} * tick_unit());
    state_.anchor_count = 0;
    transfer_preloaded();
    return;
  }
  if(next.which == term::mode_switch)
  {
    state_.switch_due = 0;
    if(trigger_mode())
    {
      state_.armed_mode = state_.switch_single != 0 ? 2 : 1;
    }
    if(state_.running != 0)
    {
      if(auto const ticks = ticks_at(next.due))
      {
        reanchor(next.due, *ticks);
      }
      state_.single = state_.switch_single;
    }
    else if(not trigger_mode())
    {
      if(not(wave() and state_.started_once != 0))
      {
        if(state_.hz == 0)
        {
          edge_start(0, state_.switch_single != 0);
        }
        else
        {
          begin(wide{next.due} * state_.hz, 0, state_.switch_single != 0);
        }
      }
    }
    return;
  }
  auto const compare = next.which == term::compare_written;
  raise_flag(compare ? compare_ok : reload_ok);
  if(state_.running != 0 and preload())
  {
    if(state_.preloaded == 0)
    {
      align_wrap(next.due);
    }
    state_.preloaded |= compare ? compare_loaded : reload_loaded;
    (compare ? state_.compare_preload : state_.reload_preload) =
        compare ? state_.compare_pending : state_.reload_pending;
    (compare ? state_.compare_due : state_.reload_due) = 0;
    return;
  }
  if(auto const ticks = ticks_at(next.due); ticks and state_.single != 0 and *ticks >= first_wrap())
  {
    hold_in_flight(next.due);
    state_.wrap_held = next.due;
    state_.floor_ps  = next.due;
  }
  else if(ticks)
  {
    reanchor(next.due, *ticks);
  }
  if(compare)
  {
    state_.compare     = state_.compare_pending;
    state_.compare_due = 0;
  }
  else
  {
    state_.reload     = state_.reload_pending;
    state_.reload_due = 0;
  }
}

void lptim::align_wrap(std::uint64_t ps)
{
  auto const ticks = ticks_at(ps);
  auto const wrap  = first_wrap();
  if(not ticks or *ticks < wrap)
  {
    return;
  }
  auto const period = std::uint64_t{state_.reload} + 1u;
  auto       count  = (*ticks - wrap) / period;
  if(ceil_ps(anchor() + wide{wrap + count * period} * tick_unit()) < ps)
  {
    ++count;
  }
  if(count == 0)
  {
    return;
  }
  set_anchor(anchor() + wide{wrap + (count - 1) * period} * tick_unit());
  state_.anchor_count = 0;
}

void lptim::reanchor(std::uint64_t ps, std::uint64_t ticks)
{
  hold_in_flight(ps);
  state_.anchor_count = position(ticks);
  state_.floor_ps     = ps;
  set_anchor(anchor() + wide{ticks} * tick_unit());
}

void lptim::catch_up(std::uint64_t ps)
{
  auto const wanted = out_.wanted();
  listened_         = listened_.value_or(wanted) and wanted;
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
  listened_ = wanted;
}

auto lptim::preload() const -> bool
{
  return get(regs_, preload_) != 0;
}

void lptim::start(std::uint64_t ps, bool single)
{
  if(state_.hz == 0)
  {
    state_.paused_counting = 0;
    state_.paused_units    = std::uint64_t{variant_.start_cycles} * ps_per_second;
    state_.anchor_count    = 0;
    state_.floor_ps        = ps;
    state_.running         = 1;
    state_.single          = single ? 1 : 0;
    return;
  }
  begin(wide{ps} * state_.hz + wide{variant_.start_cycles} * ps_per_second, ps, single);
}

void lptim::begin(wide at, std::uint64_t ps, bool single)
{
  auto const ready = wide{state_.enable_ps} * state_.hz + 2 * tick_unit();
  set_anchor(std::max(at, ready));
  state_.anchor_count = 0;
  state_.floor_ps     = ps;
  state_.running      = 1;
  state_.single       = single ? 1 : 0;
}

void lptim::switch_mode(std::uint64_t ps, bool single)
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
  auto const cycles = wide{variant_.start_cycles} * ps_per_second;
  state_.switch_due =
      state_.hz == 0 ? static_cast<std::uint64_t>(cycles) : ceil_ps(wide{ps} * state_.hz + cycles);
  state_.switch_single = single ? 1 : 0;
}

void lptim::transfer_preloaded()
{
  if((state_.preloaded & compare_loaded) != 0)
  {
    state_.compare = state_.compare_preload;
  }
  if((state_.preloaded & reload_loaded) != 0)
  {
    state_.reload = state_.reload_preload;
  }
  state_.preloaded = 0;
}

void lptim::stop()
{
  transfer_preloaded();
  state_.running      = 0;
  state_.single       = 0;
  state_.anchor_count = 0;
  state_.compare_held = 0;
  state_.reload_held  = 0;
  state_.wrap_held    = 0;
}

void lptim::refuse_unmodelled()
{
  refuse_clock();
  if(trigger_mode() and get(regs_, trigger_select_) != 0)
  {
    warn_once_or_refuse(
        select_logged_, "CFGR", cfgr_.offset,
        "TRIGSEL not 0 with TRIGEN not 00, the RTC and COMP trigger inputs, at a counter start: "
        "not modelled, no trigger starts the counter");
  }
  for(std::size_t index = 0; index < unmodelled_table.size(); ++index)
  {
    auto const& use = unmodelled_table[index];
    if(not has_register(use.reg) or (not use.field.empty() and not has_field(use.reg, use.field)))
    {
      continue;
    }
    auto const at = reg(use.reg);
    auto const set =
        use.field.empty() ? regs_.peek(at.offset) != 0 : get(regs_, field(use.reg, use.field)) != 0;
    if(set)
    {
      warn_once_or_refuse(
          unmodelled_logged_[index], use.reg, at.offset,
          std::format(
              "{} at a counter start: not modelled, the counter counts the kernel clock",
              use.what));
    }
  }
}

void lptim::refresh()
{
  sync_out(now_ps());
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

void lptim::reschedule()
{
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

void lptim::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void lptim::load(std::span<std::byte const> in)
{
  dispatch_model::load(in);
  listened_.reset();
}

void lptim::on_bus_reset()
{
  if(auto const seq = std::exchange(state_.queued, 0); seq != 0)
  {
    ctx_.events.cancel(seq - 1);
  }
  if(state_.out != 0 and out_.wanted())
  {
    out_.edge(trigger_edge::falling, now_ps());
  }
  auto const held = state_.held;
  restore_reset_values();
  state_.held          = held;
  state_.hz            = kernel_hz();
  state_.trigger_level = pin_level(etr_) ? 1 : 0;
  state_.in1_level     = pin_level(in1_) ? 1 : 0;
  state_.in2_level     = pin_level(in2_) ? 1 : 0;
}

void lptim::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  auto const generation = state_.generation + 1;
  state_                = {};
  state_.generation     = generation;
  state_.reload         = describe("ARR").reset;
  state_.hz             = ctx_.clock.hz(kernel_);
  state_.settled_ps     = now_ps();
  line_->lower();
}

void lptim::rebase(std::uint64_t ps, std::uint64_t hz)
{
  auto dues =
      std::array{&state_.compare_due, &state_.reload_due, &state_.compare_held, &state_.reload_held,
                 &state_.wrap_held,   &state_.switch_due, &state_.trigger_due};
  auto const old = state_.hz;
  if(old != 0)
  {
    hold_in_flight(ps);
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
    auto const counting    = state_.running != 0 and not external();
    if(counting and at >= anchor())
    {
      auto const since       = at - anchor();
      state_.anchor_count    = position(static_cast<std::uint64_t>(since / tick_unit()));
      state_.paused_counting = 1;
      state_.paused_units    = static_cast<std::uint64_t>(since % tick_unit());
    }
    else if(counting)
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
  if(state_.running != 0 and not external())
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

void lptim::on_clock_change()
{
  auto const hz = kernel_hz();
  if(hz == state_.hz)
  {
    return;
  }
  auto const t = now_ps();
  catch_up(t);
  rebase(t, hz);
  refresh();
  reschedule();
}

void lptim::on_core_hold(bool held)
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

void lptim::interest_changed()
{
  catch_up(now_ps());
  refresh();
  listened_ = out_.wanted();
  reschedule();
}

auto lptim::produces(std::string_view signal) const -> bool
{
  return signal == out_.signal();
}

auto lptim::listens(std::string_view) const -> bool
{
  return false;
}

auto lptim::can_listen(std::string_view) const -> bool
{
  return false;
}

auto lptim::takes_pin(std::string_view signal) const -> bool
{
  return signal == etr_ or signal == in1_ or (not in2_.empty() and signal == in2_);
}

auto lptim::external() const -> bool
{
  return get(regs_, clock_select_) != 0 or get(regs_, count_mode_) != 0 or encoder();
}

auto lptim::encoder() const -> bool
{
  return variant_.encoder and get(regs_, encoder_) != 0;
}

void lptim::refuse_clock()
{
  if(not external())
  {
    return;
  }
  auto const polarity = get(regs_, clock_polarity_);
  auto const refusals = std::array<std::pair<bool, std::string_view>, 5>{
      {
       {get(regs_, prescaler_) != 0 and get(regs_, clock_select_) == 0,
           "PRESC not 0 with COUNTMODE 1 or ENC 1, which RM0351 34.4.11 and 34.4.13 forbid"},
       {polarity == 3, "CKPOL 11, a reserved polarity"},
       {get(regs_, clock_select_) != 0 and polarity == 2,
           "CKPOL 10 with CKSEL 1, both edges of an external clock"},
       {get(regs_, clock_filter_) != 0, "CKFLT not 0, the input 1 filter"},
       {encoder() and get(regs_, clock_select_) != 0, "ENC 1 with CKSEL 1"},
       }
  };
  for(std::size_t index = 0; index < refusals.size(); ++index)
  {
    if(refusals[index].first)
    {
      warn_once_or_refuse(
          clock_logged_[index], "CFGR", cfgr_.offset,
          std::format(
              "{} at a counter start: not modelled, input 1 edges count as configured",
              refusals[index].second));
    }
  }
}

void lptim::encoder_edge(std::uint64_t ps, bool second, bool rising)
{
  auto const polarity = get(regs_, clock_polarity_);
  if((polarity == 0 and not rising)
     or (polarity == 1 and rising)
     or state_.held != 0
     or state_.hz == 0)
  {
    return;
  }
  catch_up(ps);
  if(state_.running == 0 or ps < state_.anchor_ps)
  {
    return;
  }
  auto const opposite = (second ? state_.in1_level : state_.in2_level) != 0;
  auto const up       = second ? rising == opposite : rising != opposite;
  if(up != (state_.down == 0))
  {
    state_.down = up ? 0 : 1;
    raise_flag(up ? counting_up : counting_down);
  }
  count_edge(ps, true, up);
}

void lptim::count_edge(std::uint64_t ps, bool sampled, bool up)
{
  auto const count = state_.anchor_count;
  auto const wrap  = up ? count == state_.reload or count == counter_top : count == 0;
  if(wrap and state_.single != 0)
  {
    stop();
    return;
  }
  if(wrap)
  {
    transfer_preloaded();
  }
  auto const next     = wrap ? (up ? 0u : state_.reload) : (up ? count + 1u : count - 1u);
  state_.anchor_count = next;
  auto const reached  = [&](std::uint32_t flag, std::uint64_t& held) {
    if((state_.isr & flag) != 0)
    {
      return;
    }
    if(not sampled)
    {
      raise_flag(flag);
      return;
    }
    held = ceil_ps(wide{ps} * state_.hz + wide{variant_.event_delay_cycles} * ps_per_second);
  };
  if(next == state_.compare and state_.compare <= state_.reload)
  {
    reached(compare_match, state_.compare_held);
  }
  if(next == state_.reload)
  {
    reached(reload_match, state_.reload_held);
  }
}

void lptim::edge_start(std::uint64_t units, bool single)
{
  state_.running         = 1;
  state_.single          = single ? 1 : 0;
  state_.anchor_count    = 0;
  state_.paused_counting = 0;
  state_.paused_units    = units;
  state_.divided         = 0;
}

void lptim::clock_edge(std::uint64_t ps)
{
  if(state_.enable_left != 0)
  {
    --state_.enable_left;
    return;
  }
  auto const dues = std::array<std::pair<std::uint64_t*, term>, 4>{
      {{&state_.compare_due, term::compare_written},
       {&state_.reload_due, term::reload_written},
       {&state_.switch_due, term::mode_switch},
       {&state_.trigger_due, term::trigger}}
  };
  auto ready = std::array<bool, 4>{};
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
    count_edge(ps, false);
  }
  for(std::size_t index = 0; index < dues.size(); ++index)
  {
    if(ready[index])
    {
      apply(due_term{ps, dues[index].second});
    }
  }
}

void lptim::input_edge(std::uint64_t ps, bool rising)
{
  auto const polarity = get(regs_, clock_polarity_);
  if((polarity == 0 and not rising) or (polarity == 1 and rising) or state_.held != 0)
  {
    return;
  }
  auto const sampled = get(regs_, clock_select_) == 0;
  if(sampled and state_.hz == 0)
  {
    return;
  }
  catch_up(ps);
  if(not sampled)
  {
    clock_edge(ps);
    return;
  }
  if(state_.running == 0 or ps < state_.anchor_ps)
  {
    return;
  }
  count_edge(ps, true);
}

auto lptim::trigger_mode() const -> bool
{
  return get(regs_, trigger_enable_) != 0;
}

auto lptim::pin_level(std::string_view signal) const -> bool
{
  return not signal.empty() and std::ranges::any_of(ctx_.peers.all(), [&](model const* peer) {
    auto const* const port = dynamic_cast<gpio_port const*>(peer);
    return port != nullptr and port->signal_level(signal).value_or(false);
  });
}

void lptim::triggered(std::string_view signal, trigger_edge edge, trigger_time at)
{
  if(signal == in1_ or (not in2_.empty() and signal == in2_))
  {
    auto const second = signal == in2_;
    auto const rising = edge == trigger_edge::rising;
    if(enabled() and external())
    {
      auto const t  = now_ps();
      auto const ps = std::min(at.ps, t);
      if(encoder())
      {
        encoder_edge(ps, second, rising);
      }
      else if(not second)
      {
        input_edge(ps, rising);
      }
      catch_up(t);
      refresh();
      reschedule();
    }
    (second ? state_.in2_level : state_.in1_level) = rising ? 1 : 0;
    return;
  }
  if(signal != etr_)
  {
    return;
  }
  auto const rising = edge == trigger_edge::rising;
  auto const t      = now_ps();
  catch_up(std::min(at.ps, t));
  auto const frozen = state_.hz == 0 and (get(regs_, clock_select_) == 0 or state_.held != 0);
  if(not trigger_mode() or not enabled() or get(regs_, trigger_select_) != 0 or frozen)
  {
    state_.trigger_level = rising ? 1 : 0;
    state_.trigger_due   = 0;
    return;
  }
  state_.trigger_due = 0;
  if(rising != (state_.trigger_level != 0))
  {
    auto const code       = get(regs_, trigger_filter_);
    auto const samples    = code == 0 ? 0u : 1u << code;
    state_.trigger_rising = rising ? 1 : 0;
    if(state_.hz != 0)
    {
      state_.trigger_due = std::max(
          ceil_ps(wide{at.ps} * state_.hz + wide{samples} * ps_per_second), state_.settled_ps);
    }
    else if(samples != 0)
    {
      state_.trigger_due = std::uint64_t{samples} * ps_per_second;
    }
    else
    {
      apply(due_term{std::min(at.ps, t), term::trigger});
    }
  }
  catch_up(t);
  refresh();
  reschedule();
}

auto lptim::level(std::string_view signal) const -> std::optional<bool>
{
  if(signal != out_.signal())
  {
    return std::nullopt;
  }
  return (static_cast<std::uint8_t>(raw_at(now_ps()) ? 1u : 0u) ^ state_.polarity) != 0;
}

void lptim::plain_written(std::uint32_t, std::uint32_t)
{}

void lptim::clear_written(std::uint32_t, std::uint32_t now)
{
  catch_up(now_ps());
  state_.isr &= ~now;
  state_.armed &= ~now;
  regs_.poke(reg("ICR").offset, 0);
  refresh();
  reschedule();
}

template <locked Which> void lptim::locked_written(std::uint32_t old, std::uint32_t now)
{
  if(Which == locked::configuration and not enabled())
  {
    auto const t = now_ps();
    catch_up(t);
    state_.polarity = static_cast<std::uint8_t>(get(regs_, polarity_));
    if(auto const hz = kernel_hz(); hz != state_.hz)
    {
      rebase(t, hz);
    }
    refresh();
    reschedule();
    return;
  }
  if(not enabled() or old == now)
  {
    return;
  }
  constexpr auto name = Which == locked::interrupts    ? std::string_view{"IER"}
                      : Which == locked::configuration ? std::string_view{"CFGR"}
                                                       : std::string_view{"OR"};
  auto const     at   = reg(name);
  regs_.poke(at.offset, old);
  warn_once_or_refuse(
      locked_logged_[static_cast<std::size_t>(Which)], name, at.offset,
      std::format("a {} write with CR.ENABLE 1: the register keeps its value", name));
}

void lptim::control_written(std::uint32_t old, std::uint32_t now)
{
  if((old & now & enable_bit) != 0
     and (now & single_start) != 0
     and (now & continuous_run) == 0
     and encoder())
  {
    regs_.poke(cr_.offset, old & enable_bit);
    warn_once_or_refuse(
        single_logged_, "CR", cr_.offset,
        "SNGSTRT with ENC 1, which RM0351 34.4.13 runs in continuous mode only: the one-shot "
        "stop applies at an up wrap");
    regs_.poke(cr_.offset, now);
  }
  auto const t = now_ps();
  catch_up(t);
  regs_.poke(cr_.offset, now & enable_bit);
  auto const was_on = (old & enable_bit) != 0;
  auto const is_on  = (now & enable_bit) != 0;
  if(not was_on and is_on)
  {
    state_.enable_ps   = t;
    state_.enable_left = 2;
  }
  else if(was_on and not is_on)
  {
    state_.switch_due   = 0;
    state_.trigger_due  = 0;
    state_.armed_mode   = 0;
    state_.latched      = 0;
    state_.started_once = 0;
    stop();
  }
  auto const starting = now & (single_start | continuous_run);
  if(was_on and is_on and starting != 0)
  {
    auto const single = (starting & continuous_run) == 0;
    if(state_.running != 0 or state_.switch_due != 0)
    {
      switch_mode(t, single);
    }
    else if(trigger_mode())
    {
      refuse_unmodelled();
      state_.armed_mode = single ? 2 : 1;
    }
    else if(not(wave() and state_.started_once != 0))
    {
      refuse_unmodelled();
      start(t, single);
      state_.started_once = 1;
    }
  }
  refresh();
  reschedule();
}

template <term Which> void lptim::value_written(std::uint32_t old, std::uint32_t now)
{
  constexpr auto compare = Which == term::compare_written;
  auto const     at      = compare ? cmp_ : arr_;
  auto const     name    = compare ? std::string_view{"CMP"} : std::string_view{"ARR"};
  if(not enabled())
  {
    regs_.poke(at.offset, old);
    warn_once_or_refuse(
        value_logged_, name, at.offset,
        std::format("a {} write with CR.ENABLE 0: the register keeps its value", name));
    return;
  }
  auto const t = now_ps();
  catch_up(t);
  auto& due = compare ? state_.compare_due : state_.reload_due;
  if(due != 0)
  {
    regs_.poke(at.offset, old);
    warn_once_or_refuse(
        pending_logged_, name, at.offset,
        std::format("a {} write before its OK flag: the new value replaces the pending one", name));
    regs_.poke(at.offset, now);
  }
  (compare ? state_.compare_pending : state_.reload_pending) = now & counter_top;
  auto const cycles = wide{variant_.ok_cycles} * ps_per_second;
  due = state_.hz == 0 ? static_cast<std::uint64_t>(cycles) : ceil_ps(wide{t} * state_.hz + cycles);
  refresh();
  reschedule();
}

auto lptim::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx access)
    -> std::expected<std::uint32_t, bus_error>
{
  auto const listened = listened_;
  catch_up(now_ps());
  refresh();
  if(listened_ != listened)
  {
    reschedule();
  }
  return dispatch_model::on_read(regs, offset, size, access);
}

void lptim::on_event(std::uint32_t kind, std::uint64_t payload)
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

auto lptim_factory(lptim_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<lptim>(ctx, desc, variant);
  };
}

namespace
{
constexpr auto lptim1_instances = std::to_array<std::string_view>({"LPTIM1"});
constexpr auto lptim2_instances = std::to_array<std::string_view>({"LPTIM2"});
constexpr auto lptim_table      = std::to_array<builtin>({
    row_of<^^lptim_factory, ^^l4_lptim1>(
        {.ip              = "LPTIM",
         .ip_version_glob = "L4_lptimer1_v1*",
         .instance_globs  = lptim1_instances,
         .families        = family::l4}),
    row_of<^^lptim_factory, ^^l4_lptim2>(
        {.ip              = "LPTIM",
         .ip_version_glob = "L4_lptimer1_v1*",
         .instance_globs  = lptim2_instances,
         .families        = family::l4}),
});
}

auto lptim_rows() -> std::span<builtin const>
{
  return lptim_table;
}
}
