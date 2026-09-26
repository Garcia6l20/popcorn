#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/periph/stm32/gpio.hpp>

#include <array>
#include <cstdint>
#include <memory>
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

private:
  void interrupts();

  register_file&                    regs_;
  std::array<field_ref, gpio_lines> rising_{};
  std::array<field_ref, gpio_lines> falling_{};
  std::array<field_ref, gpio_lines> rising_pending_{};
  std::array<field_ref, gpio_lines> falling_pending_{};
  std::array<field_ref, gpio_lines> software_{};
  std::array<field_ref, gpio_lines> unmasked_{};
  std::array<field_ref, gpio_lines> source_{};
  std::array<irq_line*, gpio_lines> lines_{};
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

exti::exti(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "EXTI", exti_writes}, regs_{ctx.regs}
{
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
}

auto exti_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<exti>(ctx, desc);
  };
}
}
