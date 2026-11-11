#include "exti_l1l4.hpp"
#include "exti_rows.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/periph/stm32/gpio.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint8_t gpio_lines = 16;

struct exti_l1l4_state
{
  friend auto operator==(exti_l1l4_state const&, exti_l1l4_state const&) -> bool = default;
};

static_assert(std::is_empty_v<exti_l1l4_state>);

/**
 * @brief Classic EXTI of the STM32L1, RM0038 section 10.2, and of the STM32L47x/L48x, RM0351
 *        section 14, one register per role and bank and one bit per line, the banks from the
 *        variant: `IMR`, `RTSR`, `FTSR`, `SWIER` and `PR` on the STM32L1, `IMR1` to `PR1` for lines
 *        0 to 31 and `IMR2` to `PR2` for lines 32 to 39 on the STM32L4. Lines 0 to 15 take the pin
 *        of the GPIO port the SYSCFG peer selects in `SYSCFG_EXTICRn`, lines 16 and up the sources
 *        that call exti_line_input::line_changed(). A rising edge of a line with its RTSR bit set,
 *        or a falling edge with its FTSR bit set, sets its PR bit while its IMR bit is set, RM0038
 *        10.2.4. A write of 1 to a SWIER bit that reads 0 sets the PR bit of an unmasked line, the
 *        SWIER bit reads 1 until a write of 1 to the PR bit, RM0038 10.3.5. A write of 1 to a PR
 *        bit clears it and the SWIER bit of the line, whether the PR bit was set or not. Each NVIC
 *        line of the variant is raised while the PR bit of one of its lines is set, in either bank,
 *        the pending register feeding the NVIC, RM0038 Figure 32. A line in no NVIC group of the
 *        variant sets its PR bit and raises nothing. A direct line of the STM32L4, 17, 23 to 34 and
 *        39, RM0351 Table 62, has an IMR and an EMR bit only, storage with the reset value: it
 *        latches nothing and raises no NVIC line, as RM0351 14.3 gives for a running system, and
 *        its source raises its own NVIC line. A deep-sleep WFI idles as a plain WFI, so a direct
 *        line wakes nothing. The description must give each RTSR, FTSR, SWIER and PR register
 *        exactly the bank's lines without the direct lines, and IMR the bank's lines. EMR is
 *        storage the model leaves unclaimed, and an EXTI event wakes no WFE. Every reset brings the
 *        registers to their description reset, PR 0. Each GPIO line n drives the trigger signal
 *        `extin`, the input of the line before its edge detection: an edge at each change of the
 *        selected pin, at the time of the pin edge, and at a `SYSCFG_EXTICRn` write that moves the
 *        line to a pin of another level, whatever IMR, RTSR and FTSR hold, inferred. Its level is
 *        the selected pin's. RM0038 Tables 61 and 62 name lines 11 and 15 as ADC triggers. The
 *        model is stateless.
 *
 * @limits exti EXTI
 *
 * @group l1 rule
 * - STM32L1: `EXTI_EMR` is storage, and an EXTI event wakes no WFE.
 * - STM32L1: lines 16, 17, 18, 20 and 23 set their PR bit only, from `EXTI_SWIER`. The description
 *   gives `PVD_IRQn`, `RTC_Alarm_IRQn`, `USB_FS_WKUP_IRQn`, `RTC_WKUP_IRQn` and `COMP_ACQ_IRQn` to
 *   PWR, RTC, USB and COMP. Lines 16 to 23 take their edges from `EXTI_SWIER` only, the PVD, the
 *   RTC, the USB and the comparators drive none.
 * - STM32L1: lines 0 to 15 drive the trigger signals `exti0` to `exti15` the ADC selects, the input
 *   of the line before its edge detection: an edge at each change of the selected pin, at the pin
 *   edge's own time, and at a `SYSCFG_EXTICRn` write that moves the line to a pin of another level,
 *   whatever `EXTI_IMR`, `RTSR` and `FTSR` hold, inferred.
 * - STM32L1: line 23 exists on Cat.3 to Cat.6, so `EXTI_IMR` takes 0x00FFFFFF there and 0x007FFFFF
 *   on Cat.1 and Cat.2, inferred from RM0038 chapter 10.
 * - STM32L1, inferred rules: an edge or a SWIER write sets PR only while the `EXTI_IMR` bit is set,
 *   the NVIC line follows PR whatever IMR holds, a SWIER write of 0 clears the SWIER bit, a write
 *   of 1 to a PR bit clears the SWIER bit of the line whether PR was set or not, a `SYSCFG_EXTICRn`
 *   write that moves a line to a pin of another level is an edge on the line, and a selector code
 *   RM0038 8.5.3 to 8.5.6 does not list selects no pin.
 *
 * @group l4 rule
 * - STM32L4: lines 0 to 31 sit in `EXTI_IMR1` to `EXTI_PR1`, lines 32 to 39 in `EXTI_IMR2` to
 *   `EXTI_PR2`, RM0351 14.5. Both banks follow the STM32L1 rules above, the inferred ones inferred
 *   on the STM32L4 too.
 * - STM32L4: the direct lines 17, 23 to 34 and 39, RM0351 Table 62, have an `EXTI_IMRx` and an
 *   `EXTI_EMRx` bit, storage with the reset value. A direct line sets no PR bit and raises no NVIC
 *   line, as RM0351 14.3 gives while the system runs. Its source raises its own NVIC line. A
 *   deep-sleep WFI idles as a plain WFI, so a direct line wakes nothing.
 * - STM32L4: lines 16 and 35 to 38, the PVD and PVM1 to PVM4, raise `PVD_PVM_IRQn`, lines 18 to 22
 *   `RTC_Alarm_IRQn`, `TAMP_STAMP_IRQn`, `RTC_WKUP_IRQn` and `COMP_IRQn`. These lines take their
 *   edges from `EXTI_SWIERx` only.
 * - STM32L4: `EXTI_EMR1` and `EXTI_EMR2` are storage, and an EXTI event wakes no WFE.
 */
class exti_l1l4 final : public dispatch_model<exti_l1l4, exti_l1l4_state>,
                        public exti_input,
                        public exti_line_input,
                        public trigger_source
{
public:
  exti_l1l4(model_context& ctx, peripheral_desc const& desc, exti_l1l4_variant const& variant);

  void               reset(register_file& regs, reset_kind kind) override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  void               pin_changed(std::uint8_t port, std::uint8_t pin, bool level) override;
  void               line_changed(std::uint8_t line, bool level) override;
  [[nodiscard]] auto stateless() const -> bool override;
  void               interest_changed() override;
  [[nodiscard]] auto produces(std::string_view signal) const -> bool override;
  [[nodiscard]] auto level(std::string_view signal) const -> std::optional<bool> override;

  template <std::size_t Bank> void software_written(std::uint32_t old, std::uint32_t now);
  template <std::size_t Bank> void pending_written(std::uint32_t old, std::uint32_t now);

private:
  struct bank_regs
  {
    std::uint8_t  first{};
    std::uint32_t lines{};
    std::uint32_t edges{};
    std::uint32_t imr{};
    std::uint32_t rtsr{};
    std::uint32_t ftsr{};
    std::uint32_t swier{};
    std::uint32_t pr{};
  };

  [[nodiscard]] auto bank_of(exti_bank const& bank) const -> bank_regs;
  [[nodiscard]] auto line_register(std::string_view name, std::uint32_t lines) const
      -> std::uint32_t;
  [[nodiscard]] auto pending() const -> std::uint64_t;
  void               interrupts();
  [[nodiscard]] auto output_of(std::string_view signal) const -> std::optional<std::uint8_t>;

  register_file&                                   regs_;
  std::vector<bank_regs>                           banks_;
  std::vector<std::pair<std::uint64_t, irq_line*>> irqs_;
  std::vector<trigger_output>                      outputs_;
  std::optional<std::uint64_t>                     edge_ps_;
};

constexpr std::size_t max_banks = 2;

auto exti_l1l4_writes(
    model_context&           ctx,
    peripheral_desc const&   desc,
    exti_l1l4_variant const& variant) -> std::vector<write_entry<exti_l1l4>>
{
  constexpr std::array software{&exti_l1l4::software_written<0>, &exti_l1l4::software_written<1>};
  constexpr std::array pending{&exti_l1l4::pending_written<0>, &exti_l1l4::pending_written<1>};
  if(variant.banks.empty() or variant.banks.size() > max_banks)
  {
    throw config_error(
        ctx.part.part, 0, std::format("peripherals.{}", desc.name),
        std::format("one or two EXTI banks, the variant gives {}", variant.banks.size()));
  }
  std::vector<write_entry<exti_l1l4>> out;
  for(std::size_t at = 0; at < variant.banks.size(); ++at)
  {
    auto const& bank = variant.banks[at];
    for(auto const& [role, handler] : {
            std::pair{std::string_view{"IMR"},   decltype(software)::value_type{}},
            std::pair{std::string_view{"RTSR"},  decltype(software)::value_type{}},
            std::pair{std::string_view{"FTSR"},  decltype(software)::value_type{}},
            std::pair{std::string_view{"SWIER"}, software[at]                    },
            std::pair{std::string_view{"PR"},    pending[at]                     },
    })
    {
      auto const  name  = std::format("{}{}", role, bank.suffix);
      auto const* found = find_register(desc, name);
      if(found == nullptr)
      {
        throw config_error(
            ctx.part.part, 0, std::format("peripherals.{}.registers.{}", desc.name, name),
            "a register of the EXTI bank");
      }
      out.push_back({found->name, handler});
    }
  }
  return out;
}

exti_l1l4::exti_l1l4(
    model_context&           ctx,
    peripheral_desc const&   desc,
    exti_l1l4_variant const& variant)
    : dispatch_model{ctx, desc, "EXTI", exti_l1l4_writes(ctx, desc, variant)}, regs_{ctx.regs}
{
  for(auto const& bank : variant.banks)
  {
    banks_.push_back(bank_of(bank));
  }
  for(auto const& group : variant.irqs)
  {
    auto const width = group.last - group.first + 1u;
    auto const named = std::ranges::find(ctx.part.irqs, group.irq, &irq_desc::number);
    if(named == ctx.part.irqs.end())
    {
      throw config_error(
          ctx.part.part, 0, std::format("peripherals.{}.irqs", desc.name),
          std::format("NVIC line {}, which the EXTI model raises", group.irq));
    }
    auto const lines = ((std::uint64_t{1} << width) - 1u) << group.first;
    auto*      line  = &irq(named->name);
    auto const same  = std::ranges::find(irqs_, line, &decltype(irqs_)::value_type::second);
    if(same != irqs_.end())
    {
      same->first |= lines;
    }
    else
    {
      irqs_.emplace_back(lines, line);
    }
  }
  for(std::uint8_t line = 0; line < gpio_lines; ++line)
  {
    outputs_.emplace_back(ctx.peers, std::format("exti{}", line));
  }
}

auto exti_l1l4::line_register(std::string_view name, std::uint32_t lines) const -> std::uint32_t
{
  auto const&   described = describe(name);
  std::uint32_t mask      = 0;
  for(auto const& bits : described.fields)
  {
    mask |= static_cast<std::uint32_t>(((std::uint64_t{1} << bits.width) - 1u) << bits.lsb);
  }
  if(mask != lines)
  {
    throw config_error(
        ctx_.part.part, 0, std::format("peripherals.{}.registers.{}.fields", desc_.name, name),
        std::format(
            "one bit per line, line mask {:#010x}, the description gives {:#010x}", lines, mask));
  }
  return reg(name).offset;
}

auto exti_l1l4::bank_of(exti_bank const& bank) const -> bank_regs
{
  auto const lines = static_cast<std::uint32_t>((std::uint64_t{1} << bank.lines) - 1u);
  auto const edges = lines & ~bank.direct;
  auto const named = [&](std::string_view role) { return std::format("{}{}", role, bank.suffix); };
  return {
      .first = bank.first,
      .lines = lines,
      .edges = edges,
      .imr   = line_register(named("IMR"), lines),
      .rtsr  = line_register(named("RTSR"), edges),
      .ftsr  = line_register(named("FTSR"), edges),
      .swier = line_register(named("SWIER"), edges),
      .pr    = line_register(named("PR"), edges)};
}

void exti_l1l4::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  interrupts();
}

void exti_l1l4::on_event(std::uint32_t, std::uint64_t)
{}

auto exti_l1l4::pending() const -> std::uint64_t
{
  std::uint64_t out = 0;
  for(auto const& bank : banks_)
  {
    out |= std::uint64_t{regs_.peek(bank.pr) & bank.edges} << bank.first;
  }
  return out;
}

void exti_l1l4::interrupts()
{
  auto const lines_pending = pending();
  for(auto const& [lines, line] : irqs_)
  {
    if((lines_pending & lines) != 0)
    {
      line->raise();
    }
    else
    {
      line->lower();
    }
  }
}

void exti_l1l4::pin_changed(std::uint8_t port, std::uint8_t pin, bool level)
{
  if(pin >= gpio_lines)
  {
    return;
  }
  auto const* const selector = dynamic_cast<exti_selector const*>(ctx_.peers.find("SYSCFG"));
  if(selector != nullptr and selector->selected_port(pin) == port)
  {
    auto const* const clock = dynamic_cast<pin_edge_clock const*>(
        ctx_.peers.find(std::format("GPIO{}", static_cast<char>('A' + port))));
    edge_ps_ = clock != nullptr ? clock->edge_ps() : ctx_.clock.time().ps;
    try
    {
      line_changed(pin, level);
    }
    catch(...)
    {
      edge_ps_.reset();
      throw;
    }
    edge_ps_.reset();
  }
}

void exti_l1l4::line_changed(std::uint8_t line, bool level)
{
  auto const bank = std::ranges::find_if(banks_, [&](bank_regs const& b) {
    return line >= b.first and line - b.first < std::popcount(b.lines);
  });
  if(bank == banks_.end())
  {
    return;
  }
  auto const bit = std::uint32_t{1} << (line - bank->first);
  if((bank->edges & bit) == 0)
  {
    return;
  }
  if(line < gpio_lines)
  {
    outputs_[line].edge(
        level ? trigger_edge::rising : trigger_edge::falling,
        edge_ps_.value_or(ctx_.clock.time().ps));
  }
  auto const edge = regs_.peek(level ? bank->rtsr : bank->ftsr);
  if((edge & regs_.peek(bank->imr) & bit) == 0)
  {
    return;
  }
  regs_.poke(bank->pr, regs_.peek(bank->pr) | bit);
  interrupts();
}

template <std::size_t Bank> void exti_l1l4::software_written(std::uint32_t old, std::uint32_t now)
{
  auto const& bank   = banks_[Bank];
  auto const  raised = now & ~old & bank.edges & regs_.peek(bank.imr);
  regs_.poke(bank.pr, regs_.peek(bank.pr) | raised);
  interrupts();
}

template <std::size_t Bank> void exti_l1l4::pending_written(std::uint32_t old, std::uint32_t now)
{
  auto const& bank    = banks_[Bank];
  auto const  written = now & bank.edges;
  regs_.poke(bank.pr, old & ~written);
  regs_.poke(bank.swier, regs_.peek(bank.swier) & ~written);
  interrupts();
}

auto exti_l1l4::stateless() const -> bool
{
  return true;
}

void exti_l1l4::interest_changed()
{}

auto exti_l1l4::output_of(std::string_view signal) const -> std::optional<std::uint8_t>
{
  auto const found = std::ranges::find(outputs_, signal, &trigger_output::signal);
  if(found == outputs_.end())
  {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(found - outputs_.begin());
}

auto exti_l1l4::produces(std::string_view signal) const -> bool
{
  return output_of(signal).has_value();
}

auto exti_l1l4::level(std::string_view signal) const -> std::optional<bool>
{
  auto const line = output_of(signal);
  if(not line)
  {
    return std::nullopt;
  }
  auto const* const selector = dynamic_cast<exti_selector const*>(ctx_.peers.find("SYSCFG"));
  auto const        port     = selector != nullptr ? selector->selected_port(*line) : std::nullopt;
  if(not port)
  {
    return false;
  }
  auto const* const gpio = dynamic_cast<gpio_port const*>(
      ctx_.peers.find(std::format("GPIO{}", static_cast<char>('A' + *port))));
  return gpio != nullptr and gpio->level(*line);
}
}

auto exti_l1l4_factory(exti_l1l4_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<exti_l1l4>(ctx, desc, variant);
  };
}

namespace
{
constexpr auto exti_table = std::to_array<builtin>({
    row_of<^^exti_factory>(
        {.ip = "EXTI", .ip_version_glob = "", .families = family::u5 | family::h5}),
    row_of<^^exti_l1l4_factory, ^^l1_exti_24>(
        {.ip = "EXTI", .ip_version_glob = "", .families = family::l1}),
    row_of<^^exti_l1l4_factory, ^^l1_exti_23>(
        {.ip = "EXTI", .ip_version_glob = "STM32L1_exti_23", .families = family::l1}),
    row_of<^^exti_l1l4_factory, ^^l4_exti>(
        {.ip = "EXTI", .ip_version_glob = "", .families = family::l4}),
});
}

auto exti_rows() -> std::span<builtin const>
{
  return exti_table;
}
}
