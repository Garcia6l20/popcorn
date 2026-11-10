#include "gtzc.hpp"
#include "lpgpio_rows.hpp"
#include "model_base.hpp"
#include "trigger_signal.hpp"

#include <popcorn/periph/stm32/gpio.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::size_t   pins      = 16;
constexpr std::uint8_t  lpgpio_af = 11;
constexpr std::uint32_t pin_mask  = 0xFFFF;

struct lpgpio_state
{
  friend auto operator==(lpgpio_state const&, lpgpio_state const&) -> bool = default;
};

struct pad
{
  std::string  port;
  std::uint8_t pin{};
  std::string  signal;
};

auto signal_name(std::size_t pin) -> std::string
{
  return std::format("lpgpio1_p{}", pin);
}

auto find_pads(part_description const& part) -> std::array<std::optional<pad>, pins>
{
  std::array<std::optional<pad>, pins> pads{};
  for(auto const& port : part.peripherals)
  {
    if(port.alias_of.has_value() or not port.alternates.has_value())
    {
      continue;
    }
    for(auto const& entry : *port.alternates)
    {
      if(entry.af != lpgpio_af or entry.pin >= pins)
      {
        continue;
      }
      for(std::size_t pin = 0; pin < pins; ++pin)
      {
        if(entry.signal == signal_name(pin))
        {
          pads[pin] =
              pad{.port   = port.name,
                  .pin    = static_cast<std::uint8_t>(entry.pin),
                  .signal = entry.signal};
        }
      }
    }
  }
  return pads;
}

/**
 * @brief Low-power GPIO of the STM32U5, RM0456 section 14, LPGPIO1.
 *        Pin y of the unit controls the pad whose description alternates carry `lpgpio1_p<y>` on
 *        AF11, the pad being a GPIO pin set to alternate function mode with AFSEL 11. LPGPIO_MODER
 *        0x00 sets pin y to input or output, LPGPIO_ODR 0x14 stores the output, LPGPIO_BSRR 0x18
 *        sets the ODR bits of its low half and resets those of its high half, a set winning over
 *        a reset of the same bit, and LPGPIO_BRR 0x28 resets bits. BSRR and BRR are write-only and
 *        read 0. An output pin drives its pad through gpio_port::drive_chip_digital(), the pad
 *        following the open-drain or push-pull type of the GPIO configuration, DS13737 3.14, and
 *        an input pin releases its pad. Inferred: the pull-up of the pad decides an open-drain
 *        high, and a push-pull level wins over a board drive of the pad as it does for a GPIO
 *        output. LPGPIO_IDR 0x10 reads the level of the pad as the GPIO port gives it to the
 *        signal `lpgpio1_p<y>`, which needs the pad in alternate function mode with AFSEL 11 and
 *        is 0 otherwise, inferred: RM0456 14.3.1 states the configuration needed and not the
 *        reading without it. The pad level is read for an output pin as well, inferred. A pin
 *        whose pad the package does not bond has no pad, it stores its MODER and ODR bits and
 *        reads 0 in IDR, inferred. An instance whose description has no `lpgpio1_p<y>` AF11
 *        alternate gets no model.
 *        Security, RM0456 14.3.5: a Non-secure access to a bit of pin y whose pad has its
 *        `GPIOx_SECCFGR` bit set reads 0 and cannot write it, with no bus error, in MODER, IDR,
 *        ODR, BRR and both halves of BSRR. Under TZEN=0 every access reaches every bit.
 *        secure_function() is true while any pad is secure, RM0456 14.3.6.
 *        The drive of a pad acts while the pad is in alternate function mode with another AFSEL
 *        than 11, the GPIO model acting on the mode alone, inferred. Every reset kind and the
 *        LPGPIO1RST pulse restore the registers and release the pads. The RCC clock gate is the
 *        generic one of the RCC bus clock enables.
 */
class lpgpio final : public dispatch_model<lpgpio, lpgpio_state>,
                     public trigger_sink,
                     public trustzone_aware,
                     public bus_reset_listener
{
public:
  lpgpio(
      model_context&                       ctx,
      peripheral_desc const&               desc,
      std::array<std::optional<pad>, pins> pads);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_event(std::uint32_t, std::uint64_t) override
  {}
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;

  [[nodiscard]] auto listens(std::string_view) const -> bool override
  {
    return false;
  }
  void triggered(std::string_view, trigger_edge, trigger_time) override
  {}
  [[nodiscard]] auto can_listen(std::string_view) const -> bool override
  {
    return false;
  }
  [[nodiscard]] auto takes_pin(std::string_view signal) const -> bool override;
  [[nodiscard]] auto secure_function() const -> bool override;
  [[nodiscard]] auto secure_word() const -> std::uint32_t override;
  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;

  void mode_written(std::uint32_t old, std::uint32_t now);
  void output_written(std::uint32_t old, std::uint32_t now);
  void set_reset_written(std::uint32_t old, std::uint32_t now);
  void reset_written(std::uint32_t old, std::uint32_t now);
  void stored(std::uint32_t, std::uint32_t)
  {}

private:
  void               restore_reset_values();
  void               release_pads();
  void               sync(std::uint32_t changed);
  [[nodiscard]] auto port_of(std::size_t pin) const -> gpio_port*;
  [[nodiscard]] auto levels() const -> std::uint32_t;

  register_file&                       regs_;
  reg_ref                              moder_;
  reg_ref                              idr_;
  reg_ref                              odr_;
  reg_ref                              bsrr_;
  reg_ref                              brr_;
  std::array<std::optional<pad>, pins> pads_{};
};

constexpr std::array<write_entry<lpgpio>, 5> lpgpio_writes{
    {
     {"MODER", &lpgpio::mode_written},
     {"IDR", &lpgpio::stored},
     {"ODR", &lpgpio::output_written},
     {"BSRR", &lpgpio::set_reset_written},
     {"BRR", &lpgpio::reset_written},
     }
};

lpgpio::lpgpio(
    model_context&                       ctx,
    peripheral_desc const&               desc,
    std::array<std::optional<pad>, pins> pads)
    : dispatch_model{ctx, desc, "LPGPIO", lpgpio_writes}, regs_{ctx.regs}, moder_{reg("MODER")},
      idr_{reg("IDR")}, odr_{reg("ODR")}, bsrr_{reg("BSRR")}, brr_{reg("BRR")},
      pads_{std::move(pads)}
{}

auto lpgpio::takes_pin(std::string_view signal) const -> bool
{
  return std::ranges::any_of(pads_, [&](auto const& p) { return p and p->signal == signal; });
}

auto lpgpio::port_of(std::size_t pin) const -> gpio_port*
{
  return pads_[pin] ? dynamic_cast<gpio_port*>(ctx_.peers.find(pads_[pin]->port)) : nullptr;
}

auto lpgpio::levels() const -> std::uint32_t
{
  std::uint32_t out = 0;
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    if(auto* const port = port_of(pin))
    {
      out |= port->signal_level(pads_[pin]->signal).value_or(false) ? 1u << pin : 0u;
    }
  }
  return out;
}

auto lpgpio::secure_word() const -> std::uint32_t
{
  std::uint32_t out = 0;
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    auto const* const aware =
        pads_[pin] ? dynamic_cast<trustzone_aware const*>(ctx_.peers.find(pads_[pin]->port))
                   : nullptr;
    if(aware != nullptr and (aware->secure_word() >> pads_[pin]->pin & 1u) != 0)
    {
      out |= 1u << pin;
    }
  }
  return out;
}

auto lpgpio::secure_function() const -> bool
{
  return secure_word() != 0;
}

auto lpgpio::guard(std::uint32_t word, access_ctx const& access, bool) -> bus_guard
{
  if(not ctx_.trustzone or access.secure)
  {
    return {};
  }
  auto const hidden = secure_word();
  if(hidden == 0)
  {
    return {};
  }
  if(word == bsrr_.offset)
  {
    return bus_guard::both(hidden | hidden << 16);
  }
  return bus_guard::both(hidden);
}

auto lpgpio::on_read(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    access_ctx     access) -> std::expected<std::uint32_t, bus_error>
{
  if((offset & ~3u) == idr_.offset)
  {
    return read_lanes(levels() & ~guard(idr_.offset, access, false).raz, offset, size);
  }
  return dispatch_model::on_read(regs, offset, size, access);
}

void lpgpio::sync(std::uint32_t changed)
{
  auto const mode   = regs_.peek(moder_.offset);
  auto const output = regs_.peek(odr_.offset);
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    if((changed >> pin & 1u) == 0)
    {
      continue;
    }
    if(auto* const port = port_of(pin))
    {
      port->drive_chip_digital(
          pads_[pin]->pin,
          (mode >> pin & 1u) != 0 ? std::optional<bool>{(output >> pin & 1u) != 0} : std::nullopt);
    }
  }
}

void lpgpio::mode_written(std::uint32_t old, std::uint32_t now)
{
  sync((old ^ now) & pin_mask);
}

void lpgpio::output_written(std::uint32_t old, std::uint32_t now)
{
  sync((old ^ now) & pin_mask & regs_.peek(moder_.offset));
}

void lpgpio::set_reset_written(std::uint32_t, std::uint32_t now)
{
  auto const old    = regs_.peek(odr_.offset);
  auto const set    = now & pin_mask;
  auto const clear  = now >> 16 & pin_mask;
  auto const output = (old & ~clear) | set;
  regs_.poke(bsrr_.offset, 0);
  regs_.poke(odr_.offset, output);
  sync((old ^ output) & regs_.peek(moder_.offset));
}

void lpgpio::reset_written(std::uint32_t, std::uint32_t now)
{
  auto const old    = regs_.peek(odr_.offset);
  auto const output = old & ~(now & pin_mask);
  regs_.poke(brr_.offset, 0);
  regs_.poke(odr_.offset, output);
  sync((old ^ output) & regs_.peek(moder_.offset));
}

void lpgpio::release_pads()
{
  for(std::size_t pin = 0; pin < pins; ++pin)
  {
    if(auto* const port = port_of(pin))
    {
      port->drive_chip_digital(pads_[pin]->pin, std::nullopt);
    }
  }
}

void lpgpio::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
  release_pads();
}

void lpgpio::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void lpgpio::on_bus_reset()
{
  restore_reset_values();
}
}

auto lpgpio_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    auto pads = find_pads(ctx.part);
    if(std::ranges::none_of(pads, [](auto const& p) { return p.has_value(); }))
    {
      return nullptr;
    }
    return std::make_shared<lpgpio>(ctx, desc, std::move(pads));
  };
}

namespace
{
constexpr auto lpgpio_instances = std::to_array<std::string_view>({"LPGPIO1"});
constexpr auto lpgpio_row_table = std::to_array<builtin>({
    row_of<^^lpgpio_factory>(
        {.ip              = "GPIO",
         .ip_version_glob = "STM32U5x_gpio_*",
         .instance_globs  = lpgpio_instances,
         .families        = family::u5}),
});
}

auto lpgpio_rows() -> std::span<builtin const>
{
  return lpgpio_row_table;
}
}
