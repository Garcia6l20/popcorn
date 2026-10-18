#include "factories.hpp"
#include "gtzc.hpp"
#include "model_base.hpp"

#include <popcorn/periph/stm32/gpio.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::size_t gpio_lines = 16;

struct exti_state
{
  friend auto operator==(exti_state const&, exti_state const&) -> bool = default;
};

/**
 * @brief EXTI of the STM32U5, RM0456 section 23, lines 0 to 15 from the GPIO ports. The port
 *        EXTI_EXTICRn selects feeds each line, EXTI_RTSR1 and EXTI_FTSR1 pick the edges that set
 *        EXTI_RPR1 and EXTI_FPR1, EXTI_SWIER1 sets EXTI_RPR1. Line n raises the IRQ the
 *        description names EXTIn_IRQn while it is pending and unmasked in EXTI_IMR1. Lines 16
 *        and above are storage only.
 *
 *        TrustZone, RM0456 23.5 and Table 191: a Non-secure access reads the bits of a line whose
 *        `EXTI_SECCFGR1` bit is set as 0 and cannot write them, in RTSR1, FTSR1, SWIER1, RPR1,
 *        FPR1, IMR1, EMR1 and the line's EXTICRm field, an unprivileged access those of a line
 *        whose `EXTI_PRIVCFGR1` bit is set. `EXTI_SECCFGR1` and `EXTI_PRIVCFGR1` read for every
 *        transaction. A SECx bit takes Secure writes, privileged when PRIVx is set, a PRIVx bit
 *        privileged writes, Secure when SECx is set, RM0456 23.6.6 and 23.6.7. A Non-secure write
 *        to `EXTI_SECCFGR1` is ignored with an illegal access event. `EXTI_LOCKR` is Secure, a
 *        Non-secure access is RAZ/WI with the event, and its set-only LOCK freezes both until a
 *        reset. A line's interrupt still reaches the NVIC line, whose target state is its
 *        NVIC_ITNS bit. Under TZEN=0 these registers are storage.
 */
class exti final : public dispatch_model<exti, exti_state>, public exti_input
{
public:
  exti(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  void pin_changed(std::uint8_t port, std::uint8_t pin, bool level) override;

  template <bool Falling> void pending_written(std::uint32_t old, std::uint32_t now);
  void                         software_written(std::uint32_t old, std::uint32_t now);
  void                         mask_written(std::uint32_t old, std::uint32_t now);
  void                         lock_written(std::uint32_t old, std::uint32_t now);
  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;

private:
  void               interrupts();
  [[nodiscard]] auto line_bits(std::uint32_t word, std::uint32_t lines) const -> std::uint32_t;

  register_file&                    regs_;
  std::array<field_ref, gpio_lines> rising_{};
  std::array<field_ref, gpio_lines> falling_{};
  std::array<field_ref, gpio_lines> rising_pending_{};
  std::array<field_ref, gpio_lines> falling_pending_{};
  std::array<field_ref, gpio_lines> software_{};
  std::array<field_ref, gpio_lines> unmasked_{};
  std::array<field_ref, gpio_lines> source_{};
  std::array<irq_line*, gpio_lines> lines_{};
  std::uint32_t                     secure_{};
  std::uint32_t                     privileged_{};
  std::uint32_t                     lock_{};
  field_ref                         locked_;
  std::array<std::uint32_t, 7>      per_line_{};
  std::array<std::uint32_t, 4>      selectors_{};
  std::optional<std::size_t>        unit_;
  illegal_event                     illegal_;
};

constexpr std::array<write_entry<exti>, 8> exti_writes{
    {
     {"EXTI_SWIER1", &exti::software_written},
     {"EXTI_RPR1", &exti::pending_written<false>},
     {"EXTI_FPR1", &exti::pending_written<true>},
     {"EXTI_EXTICR1", &exti::mask_written},
     {"EXTI_EXTICR2", &exti::mask_written},
     {"EXTI_EXTICR3", &exti::mask_written},
     {"EXTI_EXTICR4", &exti::mask_written},
     {"EXTI_IMR1", &exti::mask_written},
     }
};

constexpr std::array<write_entry<exti>, 6> exti_trustzone_writes{
    {
     {"EXTI_RTSR1", nullptr},
     {"EXTI_FTSR1", nullptr},
     {"EXTI_EMR1", nullptr},
     {"EXTI_SECCFGR1", nullptr},
     {"EXTI_PRIVCFGR1", nullptr},
     {"EXTI_LOCKR", &exti::lock_written},
     }
};

exti::exti(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "EXTI", exti_writes, exti_trustzone_writes}, regs_{ctx.regs},
      unit_{unit_named(ctx.part, desc.name)}
{
  if(ctx.trustzone)
  {
    secure_          = reg("EXTI_SECCFGR1").offset;
    privileged_      = reg("EXTI_PRIVCFGR1").offset;
    lock_            = reg("EXTI_LOCKR").offset;
    locked_          = field("EXTI_LOCKR", "LOCK");
    auto const names = std::to_array<std::string_view>(
        {"EXTI_RTSR1", "EXTI_FTSR1", "EXTI_SWIER1", "EXTI_RPR1", "EXTI_FPR1", "EXTI_IMR1",
         "EXTI_EMR1"});
    for(std::size_t at = 0; at < names.size(); ++at)
    {
      per_line_[at] = reg(names[at]).offset;
    }
    for(std::size_t m = 0; m < selectors_.size(); ++m)
    {
      selectors_[m] = reg("EXTI_EXTICR" + std::to_string(m + 1)).offset;
    }
  }
  for(std::size_t line = 0; line < gpio_lines; ++line)
  {
    auto const n           = std::to_string(line);
    rising_[line]          = field("EXTI_RTSR1", "RT" + n);
    falling_[line]         = field("EXTI_FTSR1", "FT" + n);
    rising_pending_[line]  = field("EXTI_RPR1", "RPIF" + n);
    falling_pending_[line] = field("EXTI_FPR1", "FPIF" + n);
    software_[line]        = field("EXTI_SWIER1", "SWI" + n);
    unmasked_[line]        = field("EXTI_IMR1", "IM" + n);
    source_[line]          = field("EXTI_EXTICR" + std::to_string(line / 4 + 1), "EXTI" + n);
    lines_[line]           = &irq("EXTI" + n + "_IRQn");
  }
}

void exti::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  interrupts();
}

void exti::on_event(std::uint32_t, std::uint64_t)
{}

void exti::interrupts()
{
  for(std::size_t line = 0; line < gpio_lines; ++line)
  {
    auto const pending =
        get(regs_, rising_pending_[line]) != 0 or get(regs_, falling_pending_[line]) != 0;
    if(pending and get(regs_, unmasked_[line]) != 0)
    {
      lines_[line]->raise();
    }
    else
    {
      lines_[line]->lower();
    }
  }
}

void exti::pin_changed(std::uint8_t port, std::uint8_t pin, bool level)
{
  if(pin >= gpio_lines or get(regs_, source_[pin]) != port)
  {
    return;
  }
  if(level and get(regs_, rising_[pin]) != 0)
  {
    update(regs_, rising_pending_[pin], 1);
  }
  if(not level and get(regs_, falling_[pin]) != 0)
  {
    update(regs_, falling_pending_[pin], 1);
  }
  interrupts();
}

template <bool Falling> void exti::pending_written(std::uint32_t old, std::uint32_t now)
{
  auto const& pending = Falling ? falling_pending_ : rising_pending_;
  regs_.poke(pending[0].offset, old & ~now);
  interrupts();
}

void exti::software_written(std::uint32_t, std::uint32_t now)
{
  for(std::size_t line = 0; line < gpio_lines; ++line)
  {
    if(software_[line].of(now) != 0)
    {
      update(regs_, rising_pending_[line], 1);
    }
  }
  regs_.poke(software_[0].offset, 0);
  interrupts();
}

void exti::mask_written(std::uint32_t, std::uint32_t)
{
  interrupts();
}

void exti::lock_written(std::uint32_t old, std::uint32_t now)
{
  regs_.poke(lock_, old | now);
}

auto exti::line_bits(std::uint32_t word, std::uint32_t lines) const -> std::uint32_t
{
  if(std::ranges::contains(per_line_, word))
  {
    return lines;
  }
  auto const selector = std::ranges::find(selectors_, word);
  if(selector == selectors_.end())
  {
    return 0;
  }
  auto const    first = static_cast<std::uint32_t>(selector - selectors_.begin()) * 4u;
  std::uint32_t bits  = 0;
  for(std::uint32_t k = 0; k < 4; ++k)
  {
    if((lines >> (first + k) & 1u) != 0)
    {
      bits |= 0xffu << (8u * k);
    }
  }
  return bits;
}

auto exti::guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard
{
  if(not ctx_.trustzone)
  {
    return {};
  }
  auto const secure     = regs_.peek(secure_);
  auto const privileged = regs_.peek(privileged_);
  auto const locked     = locked_.of(regs_.peek(lock_)) != 0;
  auto const all        = ~std::uint32_t{};
  if(word == lock_ or word == secure_)
  {
    if(not ctx.secure)
    {
      if(write and unit_)
      {
        illegal_.raise(ctx_.peers, *unit_);
      }
      return word == lock_ ? bus_guard::both(all) : bus_guard{.wi = all};
    }
    if(word == lock_)
    {
      return {};
    }
    return {.wi = locked ? all : ctx.privileged ? 0 : privileged};
  }
  if(word == privileged_)
  {
    return {.wi = locked or not ctx.privileged ? all : ctx.secure ? 0 : secure};
  }
  auto const lines = (ctx.secure ? 0 : secure) | (ctx.privileged ? 0 : privileged);
  return bus_guard::both(lines == 0 ? 0 : line_bits(word, lines));
}
}

auto exti_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<exti>(ctx, desc);
  };
}
}
