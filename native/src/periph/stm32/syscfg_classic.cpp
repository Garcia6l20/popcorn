#include "exti_classic.hpp"
#include "factories.hpp"
#include "model_base.hpp"
#include "secure_config.hpp"

#include <popcorn/periph/stm32/gpio.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::size_t selected_lines = 16;

struct syscfg_classic_state
{
  friend auto operator==(syscfg_classic_state const&, syscfg_classic_state const&)
      -> bool = default;
};

static_assert(std::is_empty_v<syscfg_classic_state>);

/**
 * @brief Classic SYSCFG of the STM32L1, RM0038 section 8.5, its register roles from the traits.
 *        `SYSCFG_EXTICR1` to `SYSCFG_EXTICR4` select the GPIO port of the EXTI lines 0 to 15
 *        through the codes of the traits, a code with no port for that line selects none, and the
 *        EXTI peer reads them. A selector write that moves a line to a pin of another level hands
 *        the EXTI peer that edge, as the line input changes on the switch. The USB D+ pull-up
 *        enable, `SYSCFG_PMC.USB_PU`, is published through usb_pull_up_source. The registers the
 *        traits list as storage, `SYSCFG_MEMRMP` and `SYSCFG_PMC` on the STM32L1, are storage the
 *        model does not model: the machine boots from main flash, which BOOT_MODE and MEM_MODE 00
 *        give, and USB_PU drives no pull-up until a USB model reads it. A firmware write that lands
 *        on one logs one model_warning per register, and raises model_error under
 *        strict_peripherals before it is stored. Every reset brings the registers to their
 *        description reset, a SYSCFGRST pulse except the memory remap register, RM0038 8.5.1. The
 *        model is stateless.
 */
class syscfg_classic final : public dispatch_model<syscfg_classic, syscfg_classic_state>,
                             public exti_selector,
                             public usb_pull_up_source,
                             public bus_reset_listener
{
public:
  syscfg_classic(
      model_context&               ctx,
      peripheral_desc const&       desc,
      syscfg_classic_traits const& traits);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;

  [[nodiscard]] auto selected_port(std::uint8_t line) const -> std::optional<std::uint8_t> override;
  [[nodiscard]] auto usb_pull_up() const -> bool override;
  [[nodiscard]] auto stateless() const -> bool override;
  [[nodiscard]] auto storage_only(std::uint32_t offset) const -> bool override;
  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;

  template <std::size_t Register> void selector_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto port_of(std::uint8_t line, std::uint32_t code) const
      -> std::optional<std::uint8_t>;
  [[nodiscard]] auto level_of(std::optional<std::uint8_t> port, std::uint8_t line) const -> bool;
  void               reselect(std::uint32_t offset, std::uint32_t old, std::uint32_t now);
  void               restore_reset_values();

  register_file&                        regs_;
  syscfg_classic_traits                 traits_;
  std::array<field_ref, selected_lines> selectors_{};
  std::optional<field_ref>              pull_up_;
  std::optional<reg_ref>                remap_;
  std::vector<std::uint32_t>            storage_;
  storage_notes                         notes_;
};

auto syscfg_classic_writes(peripheral_desc const& desc) -> std::vector<write_entry<syscfg_classic>>
{
  constexpr std::array handlers{
      &syscfg_classic::selector_written<0>, &syscfg_classic::selector_written<1>,
      &syscfg_classic::selector_written<2>, &syscfg_classic::selector_written<3>};
  std::vector<write_entry<syscfg_classic>> out;
  for(auto const& described : desc.registers)
  {
    if(described.alternate_of)
    {
      continue;
    }
    write_entry<syscfg_classic> entry{described.name, nullptr};
    for(std::size_t at = 0; at < handlers.size(); ++at)
    {
      if(described.name == "EXTICR" + std::to_string(at + 1))
      {
        entry.on_write = handlers[at];
      }
    }
    out.push_back(entry);
  }
  return out;
}

syscfg_classic::syscfg_classic(
    model_context&               ctx,
    peripheral_desc const&       desc,
    syscfg_classic_traits const& traits)
    : dispatch_model{ctx, desc, "SYSCFG", syscfg_classic_writes(desc)}, regs_{ctx.regs},
      traits_{traits}
{
  for(std::size_t line = 0; line < selected_lines; ++line)
  {
    selectors_[line] =
        field("EXTICR" + std::to_string(line / 4 + 1), "EXTI" + std::to_string(line));
  }
  if(not traits.usb_pull_up_register.empty())
  {
    pull_up_ = field(traits.usb_pull_up_register, traits.usb_pull_up_field);
  }
  if(not traits.remap_register.empty())
  {
    remap_ = reg(traits.remap_register);
  }
  for(auto const name : traits.storage_registers)
  {
    storage_.push_back(reg(name).offset);
  }
}

void syscfg_classic::restore_reset_values()
{
  for(auto const& name : claims())
  {
    regs_.poke(reg(name).offset, describe(name).reset);
  }
}

void syscfg_classic::reset(register_file&, reset_kind)
{
  restore_reset_values();
}

void syscfg_classic::on_bus_reset()
{
  std::vector<std::pair<std::uint32_t, std::uint32_t>> selectors;
  for(auto const& bits : selectors_)
  {
    if(not std::ranges::contains(selectors, bits.offset, &decltype(selectors)::value_type::first))
    {
      selectors.emplace_back(bits.offset, regs_.peek(bits.offset));
    }
  }
  auto const remap = remap_ ? std::optional{regs_.peek(remap_->offset)} : std::nullopt;
  restore_reset_values();
  if(remap)
  {
    regs_.poke(remap_->offset, *remap);
  }
  for(auto const& [offset, old] : selectors)
  {
    reselect(offset, old, regs_.peek(offset));
  }
}

void syscfg_classic::on_event(std::uint32_t, std::uint64_t)
{}

auto syscfg_classic::port_of(std::uint8_t line, std::uint32_t code) const
    -> std::optional<std::uint8_t>
{
  auto const found = std::ranges::find(traits_.ports, code, &exti_port_code::code);
  if(found == traits_.ports.end() or ((found->lines >> line) & 1u) == 0)
  {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(found->port - 'A');
}

auto syscfg_classic::selected_port(std::uint8_t line) const -> std::optional<std::uint8_t>
{
  if(line >= selected_lines)
  {
    return std::nullopt;
  }
  return port_of(line, get(regs_, selectors_[line]));
}

auto syscfg_classic::level_of(std::optional<std::uint8_t> port, std::uint8_t line) const -> bool
{
  if(not port)
  {
    return false;
  }
  auto const        name = std::string{"GPIO"} + static_cast<char>('A' + *port);
  auto const* const gpio = dynamic_cast<gpio_port const*>(ctx_.peers.find(name));
  return gpio != nullptr and line < gpio->pin_count() and gpio->level(line);
}

void syscfg_classic::reselect(std::uint32_t offset, std::uint32_t old, std::uint32_t now)
{
  auto* const exti = dynamic_cast<exti_line_input*>(ctx_.peers.find("EXTI"));
  if(exti == nullptr)
  {
    return;
  }
  for(std::uint8_t line = 0; line < selected_lines; ++line)
  {
    auto const& bits = selectors_[line];
    if(bits.offset != offset)
    {
      continue;
    }
    auto const before = port_of(line, bits.of(old));
    auto const after  = port_of(line, bits.of(now));
    if(before == after)
    {
      continue;
    }
    auto const level = level_of(after, line);
    if(level_of(before, line) != level)
    {
      exti->line_changed(line, level);
    }
  }
}

template <std::size_t Register>
void syscfg_classic::selector_written(std::uint32_t old, std::uint32_t now)
{
  reselect(selectors_[Register * 4].offset, old, now);
}

auto syscfg_classic::usb_pull_up() const -> bool
{
  return pull_up_ and get(regs_, *pull_up_) != 0;
}

auto syscfg_classic::stateless() const -> bool
{
  return true;
}

auto syscfg_classic::storage_only(std::uint32_t offset) const -> bool
{
  return std::ranges::contains(storage_, offset);
}

auto syscfg_classic::guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard
{
  if(write and storage_only(word))
  {
    notes_.written(ctx_, desc_, "SYSCFG", word, ctx, {});
  }
  return {};
}
}

auto syscfg_classic_factory(syscfg_classic_traits traits) -> model_factory
{
  return [traits](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<syscfg_classic>(ctx, desc, traits);
  };
}
}
