#include "exti_classic.hpp"
#include "factories.hpp"
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

struct exti_classic_state
{
  friend auto operator==(exti_classic_state const&, exti_classic_state const&) -> bool = default;
};

static_assert(std::is_empty_v<exti_classic_state>);

/**
 * @brief Classic EXTI of the STM32L1, RM0038 section 10.2, one register per role and one bit per
 *        line, 23 or 24 lines from the variant. Lines 0 to 15 take the pin of the GPIO port the
 *        SYSCFG peer selects in `SYSCFG_EXTICRn`, lines 16 and up the sources that call
 *        exti_line_input::line_changed(). A rising edge of a line with its EXTI_RTSR bit set, or
 *        a falling edge with its EXTI_FTSR bit set, sets its EXTI_PR bit while its EXTI_IMR bit
 *        is set, RM0038 10.2.4. A write of 1 to an EXTI_SWIER bit that reads 0 sets the EXTI_PR
 *        bit of an unmasked line, the SWIER bit reads 1 until a write of 1 to the PR bit, RM0038
 *        10.3.5. A write of 1 to an EXTI_PR bit clears it and the SWIER bit of the line, whether
 *        the PR bit was set or not. Each NVIC line of the variant is raised while the PR bit of
 *        one of its lines is set, the pending register feeding the NVIC, RM0038 Figure 32. A line
 *        in no NVIC group of the variant sets its PR bit and raises nothing. EXTI_EMR is storage
 *        the model leaves unclaimed, and an EXTI event wakes no WFE. Every reset brings the
 *        registers to their description reset, PR 0. Each GPIO line n drives the trigger signal
 *        `extin`, the input of the line before its edge detection: an edge at each change of the
 *        selected pin, at the time of the pin edge, and at a `SYSCFG_EXTICRn` write that moves the
 *        line to a pin of another level, whatever IMR, RTSR and FTSR hold, inferred. Its level is
 *        the selected pin's. RM0038 Tables 61 and 62 name lines 11 and 15 as ADC triggers. The
 *        model is stateless.
 */
class exti_classic final : public dispatch_model<exti_classic, exti_classic_state>,
                           public exti_input,
                           public exti_line_input,
                           public trigger_source
{
public:
  exti_classic(
      model_context&              ctx,
      peripheral_desc const&      desc,
      exti_classic_variant const& variant);

  void               reset(register_file& regs, reset_kind kind) override;
  void               on_event(std::uint32_t kind, std::uint64_t payload) override;
  void               pin_changed(std::uint8_t port, std::uint8_t pin, bool level) override;
  void               line_changed(std::uint8_t line, bool level) override;
  [[nodiscard]] auto stateless() const -> bool override;
  void               interest_changed() override;
  [[nodiscard]] auto produces(std::string_view signal) const -> bool override;
  [[nodiscard]] auto level(std::string_view signal) const -> std::optional<bool> override;

  void software_written(std::uint32_t old, std::uint32_t now);
  void pending_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto line_field(std::string_view reg, std::string_view name) const -> field_ref;
  void               interrupts();
  [[nodiscard]] auto output_of(std::string_view signal) const -> std::optional<std::uint8_t>;

  register_file&                                   regs_;
  std::uint32_t                                    lines_;
  field_ref                                        unmasked_;
  field_ref                                        rising_;
  field_ref                                        falling_;
  field_ref                                        software_;
  field_ref                                        pending_;
  std::vector<std::pair<std::uint32_t, irq_line*>> irqs_;
  std::vector<trigger_output>                      outputs_;
  std::optional<std::uint64_t>                     edge_ps_;
};

constexpr std::array<write_entry<exti_classic>, 5> exti_classic_writes{
    {
     {"IMR", nullptr},
     {"RTSR", nullptr},
     {"FTSR", nullptr},
     {"SWIER", &exti_classic::software_written},
     {"PR", &exti_classic::pending_written},
     }
};

exti_classic::exti_classic(
    model_context&              ctx,
    peripheral_desc const&      desc,
    exti_classic_variant const& variant)
    : dispatch_model{ctx, desc, "EXTI", exti_classic_writes}, regs_{ctx.regs},
      lines_{(std::uint32_t{1} << variant.lines) - 1u}, unmasked_{line_field("IMR", "MR")},
      rising_{line_field("RTSR", "TR")}, falling_{line_field("FTSR", "TR")},
      software_{line_field("SWIER", "SWIER")}, pending_{line_field("PR", "PR")}
{
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
    irqs_.emplace_back(((std::uint32_t{1} << width) - 1u) << group.first, &irq(named->name));
  }
  for(std::uint8_t line = 0; line < gpio_lines; ++line)
  {
    outputs_.emplace_back(ctx.peers, std::format("exti{}", line));
  }
}

auto exti_classic::line_field(std::string_view reg, std::string_view name) const -> field_ref
{
  auto const bits = field(reg, name);
  if(bits.lsb % 32u != 0 or bits.width != std::popcount(lines_))
  {
    throw config_error(
        ctx_.part.part, 0,
        std::format("peripherals.{}.registers.{}.fields.{}", desc_.name, reg, name),
        std::format("one bit per line from bit 0, {} lines", std::popcount(lines_)));
  }
  return bits;
}

void exti_classic::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  interrupts();
}

void exti_classic::on_event(std::uint32_t, std::uint64_t)
{}

void exti_classic::interrupts()
{
  auto const pending = get(regs_, pending_);
  for(auto const& [lines, line] : irqs_)
  {
    if((pending & lines) != 0)
    {
      line->raise();
    }
    else
    {
      line->lower();
    }
  }
}

void exti_classic::pin_changed(std::uint8_t port, std::uint8_t pin, bool level)
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

void exti_classic::line_changed(std::uint8_t line, bool level)
{
  auto const bit = std::uint32_t{1} << line;
  if(line >= 32 or (lines_ & bit) == 0)
  {
    return;
  }
  if(line < gpio_lines)
  {
    outputs_[line].edge(
        level ? trigger_edge::rising : trigger_edge::falling,
        edge_ps_.value_or(ctx_.clock.time().ps));
  }
  auto const edge = get(regs_, level ? rising_ : falling_);
  if((edge & get(regs_, unmasked_) & bit) == 0)
  {
    return;
  }
  update(regs_, pending_, get(regs_, pending_) | bit);
  interrupts();
}

void exti_classic::software_written(std::uint32_t old, std::uint32_t now)
{
  auto const raised = software_.of(now) & ~software_.of(old) & get(regs_, unmasked_);
  update(regs_, pending_, get(regs_, pending_) | raised);
  interrupts();
}

void exti_classic::pending_written(std::uint32_t old, std::uint32_t now)
{
  auto const cleared = pending_.of(old) & pending_.of(now);
  regs_.poke(pending_.offset, pending_.with(old, pending_.of(old) & ~cleared));
  update(regs_, software_, get(regs_, software_) & ~pending_.of(now));
  interrupts();
}

auto exti_classic::stateless() const -> bool
{
  return true;
}

void exti_classic::interest_changed()
{}

auto exti_classic::output_of(std::string_view signal) const -> std::optional<std::uint8_t>
{
  auto const found = std::ranges::find(outputs_, signal, &trigger_output::signal);
  if(found == outputs_.end())
  {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(found - outputs_.begin());
}

auto exti_classic::produces(std::string_view signal) const -> bool
{
  return output_of(signal).has_value();
}

auto exti_classic::level(std::string_view signal) const -> std::optional<bool>
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

auto exti_classic_factory(exti_classic_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<exti_classic>(ctx, desc, variant);
  };
}
}
