#include "factories.hpp"
#include "model_base.hpp"

#include <popcorn/periph/stm32/gpio.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::size_t pins = 16;

struct gpio_state
{
  std::uint32_t driven{};
  std::uint32_t driven_high{};
  std::uint32_t lock_step{};
  std::uint32_t lock_value{};

  friend auto operator==(gpio_state const&, gpio_state const&) -> bool = default;
};

using listener_entry = std::pair<std::uint64_t, std::function<void(std::uint8_t, bool)>>;

struct listener_table
{
  std::uint64_t               next{};
  std::vector<listener_entry> entries;
};

/**
 * @brief GPIO port of the STM32U5, RM0456 section 13. Pin levels follow the output register,
 *        external drives and pulls. BSRR and BRR set and reset outputs, a set wins over a reset
 *        of the same pin. The LCKR key sequence freezes the configuration of the locked pins
 *        until reset. Level changes reach board listeners, then the EXTI model.
 */
class gpio final : public dispatch_model<gpio, gpio_state>, public gpio_port
{
public:
  gpio(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;

  void               drive(std::uint8_t pin, std::optional<bool> level) override;
  [[nodiscard]] auto level(std::uint8_t pin) const -> bool override;
  [[nodiscard]] auto mode(std::uint8_t pin) const -> pin_mode override;
  [[nodiscard]] auto alternate(std::uint8_t pin) const -> std::uint8_t override;
  [[nodiscard]] auto on_change(std::function<void(std::uint8_t pin, bool level)> listener)
      -> subscription override;

  template <std::size_t Which> void config_written(std::uint32_t old, std::uint32_t now);
  void                              output_written(std::uint32_t old, std::uint32_t now);
  void                              input_written(std::uint32_t old, std::uint32_t now);
  void                              set_reset_written(std::uint32_t old, std::uint32_t now);
  void                              reset_written(std::uint32_t old, std::uint32_t now);
  void                              lock_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto computed() const -> std::uint32_t;
  void               settle(bool notify);

  register_file&                  regs_;
  std::uint8_t                    port_;
  std::array<field_ref, pins>     mode_{};
  std::array<field_ref, pins>     type_{};
  std::array<field_ref, pins>     speed_{};
  std::array<field_ref, pins>     pull_{};
  std::array<field_ref, pins>     alternate_{};
  std::array<field_ref, pins>     input_{};
  std::array<field_ref, pins>     output_{};
  std::array<field_ref, pins>     set_{};
  std::array<field_ref, pins>     clear_{};
  std::array<field_ref, pins>     brr_{};
  std::array<field_ref, pins>     lock_{};
  field_ref                       lock_key_;
  exti_input*                     exti_{};
  std::uint32_t                   reported_{};
  std::uint32_t                   notifying_{};
  std::shared_ptr<listener_table> listeners_ = std::make_shared<listener_table>();
};

constexpr std::array<write_entry<gpio>, 11> gpio_writes{
    {
     {"GPIO_MODER", &gpio::config_written<0>},
     {"GPIO_OTYPER", &gpio::config_written<1>},
     {"GPIO_OSPEEDR", &gpio::config_written<2>},
     {"GPIO_PUPDR", &gpio::config_written<3>},
     {"GPIO_IDR", &gpio::input_written},
     {"GPIO_ODR", &gpio::output_written},
     {"GPIO_BSRR", &gpio::set_reset_written},
     {"GPIO_LCKR", &gpio::lock_written},
     {"GPIO_AFRL", &gpio::config_written<4>},
     {"GPIO_AFRH", &gpio::config_written<5>},
     {"GPIO_BRR", &gpio::reset_written},
     }
};

gpio::gpio(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "GPIO", gpio_writes}, regs_{ctx.regs},
      port_{static_cast<std::uint8_t>(desc.name.back() - 'A')},
      lock_key_{field("GPIO_LCKR", "LCKK")}
{
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    auto const n    = std::to_string(pin);
    mode_[pin]      = field("GPIO_MODER", "MODE" + n);
    type_[pin]      = field("GPIO_OTYPER", "OT" + n);
    speed_[pin]     = field("GPIO_OSPEEDR", "OSPEED" + n);
    pull_[pin]      = field("GPIO_PUPDR", "PUPD" + n);
    alternate_[pin] = field(pin < 8 ? "GPIO_AFRL" : "GPIO_AFRH", "AFSEL" + n);
    input_[pin]     = field("GPIO_IDR", "ID" + n);
    output_[pin]    = field("GPIO_ODR", "OD" + n);
    set_[pin]       = field("GPIO_BSRR", "BS" + n);
    clear_[pin]     = field("GPIO_BSRR", "BR" + n);
    brr_[pin]       = field("GPIO_BRR", "BR" + n);
    lock_[pin]      = field("GPIO_LCKR", "LCK" + n);
  }
}

void gpio::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  state_.lock_step  = 0;
  state_.lock_value = 0;
  exti_             = dynamic_cast<exti_input*>(ctx_.peers.find("EXTI"));
  settle(false);
}

void gpio::on_event(std::uint32_t, std::uint64_t)
{}

auto gpio::mode(std::uint8_t pin) const -> pin_mode
{
  return static_cast<pin_mode>(get(regs_, mode_[pin]));
}

auto gpio::alternate(std::uint8_t pin) const -> std::uint8_t
{
  return static_cast<std::uint8_t>(get(regs_, alternate_[pin]));
}

auto gpio::level(std::uint8_t pin) const -> bool
{
  return get(regs_, input_[pin]) != 0;
}

auto gpio::computed() const -> std::uint32_t
{
  std::uint32_t levels = 0;
  for(std::uint8_t pin = 0; pin < pins; ++pin)
  {
    auto const bit            = std::uint32_t{1} << pin;
    auto const driven         = (state_.driven & bit) != 0;
    auto const pulled         = get(regs_, pull_[pin]) == 1;
    auto const floating_level = driven ? (state_.driven_high & bit) != 0 : pulled;
    auto       high           = false;
    switch(mode(pin))
    {
      case pin_mode::output:
        if(get(regs_, output_[pin]) == 0)
        {
          high = false;
        }
        else
        {
          high = get(regs_, type_[pin]) == 0 or floating_level;
        }
        break;
      case pin_mode::input:
      case pin_mode::alternate:
        high = floating_level;
        break;
      case pin_mode::analog:
        high = false;
        break;
    }
    levels |= high ? bit : 0;
  }
  return levels;
}

void gpio::settle(bool notify)
{
  std::uint32_t before = 0;
  for(std::uint8_t pin = 0; pin < pins; ++pin)
  {
    before |= level(pin) ? std::uint32_t{1} << pin : 0;
  }
  auto const after = computed();
  for(std::uint8_t pin = 0; pin < pins; ++pin)
  {
    update(regs_, input_[pin], after >> pin & 1u);
  }
  if(not notify or before == after)
  {
    return;
  }
  if(notifying_ == 0)
  {
    reported_ = before;
  }
  ++notifying_;
  std::vector<std::uint64_t> ids;
  for(auto const& [id, listener] : listeners_->entries)
  {
    ids.push_back(id);
  }
  for(std::uint8_t pin = 0; pin < pins; ++pin)
  {
    auto const high = level(pin);
    if(((reported_ >> pin) & 1u) == (high ? 1u : 0u))
    {
      continue;
    }
    reported_ ^= std::uint32_t{1} << pin;
    for(auto const id : ids)
    {
      auto const found = std::ranges::find(listeners_->entries, id, &listener_entry::first);
      if(found != listeners_->entries.end())
      {
        auto const listener = found->second;
        listener(pin, high);
      }
    }
    if(exti_ != nullptr)
    {
      exti_->pin_changed(port_, pin, high);
    }
  }
  --notifying_;
}

template <std::size_t Which> void gpio::config_written(std::uint32_t old, std::uint32_t now)
{
  constexpr std::array<std::array<std::size_t, 2>, 6> spans{
      {
       {0, pins},
       {0, pins},
       {0, pins},
       {0, pins},
       {0, pins / 2},
       {pins / 2, pins},
       }
  };
  auto const* const fields =
      std::array{&mode_, &type_, &speed_, &pull_, &alternate_, &alternate_}[Which];
  auto const [first, last] = spans[Which];
  if(get(regs_, lock_key_) != 0)
  {
    for(auto pin = first; pin < last; ++pin)
    {
      if(get(regs_, lock_[pin]) != 0)
      {
        now = (*fields)[pin].with(now, (*fields)[pin].of(old));
      }
    }
    regs_.poke((*fields)[first].offset, now);
  }
  settle(true);
}

void gpio::output_written(std::uint32_t, std::uint32_t)
{
  settle(true);
}

void gpio::input_written(std::uint32_t old, std::uint32_t)
{
  regs_.poke(input_[0].offset, old);
}

void gpio::set_reset_written(std::uint32_t, std::uint32_t now)
{
  auto odr = regs_.peek(output_[0].offset);
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    if(clear_[pin].of(now) != 0)
    {
      odr = output_[pin].with(odr, 0);
    }
    if(set_[pin].of(now) != 0)
    {
      odr = output_[pin].with(odr, 1);
    }
  }
  regs_.poke(set_[0].offset, 0);
  regs_.poke(output_[0].offset, odr);
  settle(true);
}

void gpio::reset_written(std::uint32_t, std::uint32_t now)
{
  auto odr = regs_.peek(output_[0].offset);
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    if(brr_[pin].of(now) != 0)
    {
      odr = output_[pin].with(odr, 0);
    }
  }
  regs_.poke(brr_[0].offset, 0);
  regs_.poke(output_[0].offset, odr);
  settle(true);
}

void gpio::lock_written(std::uint32_t old, std::uint32_t now)
{
  if(lock_key_.of(old) != 0)
  {
    regs_.poke(lock_key_.offset, old);
    return;
  }
  std::uint32_t value = 0;
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    value |= lock_[pin].of(now) << pin;
  }
  auto const key = lock_key_.of(now) != 0;
  if(state_.lock_step == 1 and not key and value == state_.lock_value)
  {
    state_.lock_step = 2;
  }
  else if(state_.lock_step == 2 and key and value == state_.lock_value)
  {
    state_.lock_step = 0;
    regs_.poke(lock_key_.offset, lock_key_.with(now, 1));
    return;
  }
  else
  {
    state_.lock_step  = key ? 1 : 0;
    state_.lock_value = value;
  }
  regs_.poke(lock_key_.offset, lock_key_.with(now, 0));
}

void gpio::drive(std::uint8_t pin, std::optional<bool> level)
{
  auto const bit     = std::uint32_t{1} << pin;
  state_.driven      = level ? state_.driven | bit : state_.driven & ~bit;
  state_.driven_high = level and *level ? state_.driven_high | bit : state_.driven_high & ~bit;
  settle(true);
}

auto gpio::on_change(std::function<void(std::uint8_t pin, bool level)> listener) -> subscription
{
  auto const id = listeners_->next++;
  listeners_->entries.emplace_back(id, std::move(listener));
  return subscription{[table = std::weak_ptr<listener_table>{listeners_}, id] {
    if(auto const live = table.lock())
    {
      std::erase_if(live->entries, [id](auto const& entry) { return entry.first == id; });
    }
  }};
}
}

auto gpio_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    if(not desc.name.starts_with("GPIO"))
    {
      return nullptr;
    }
    return std::make_shared<gpio>(ctx, desc);
  };
}
}
