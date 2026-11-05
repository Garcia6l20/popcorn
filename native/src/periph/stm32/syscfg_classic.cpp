#include "exti_classic.hpp"
#include "model_base.hpp"
#include "read_protection.hpp"
#include "secure_config.hpp"
#include "syscfg_rows.hpp"

#include <popcorn/periph/stm32/gpio.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::size_t   selected_lines = 16;
constexpr std::uint32_t erase_done     = 0;
constexpr std::uint32_t first_key      = 0xCA;
constexpr std::uint32_t second_key     = 0x53;

struct syscfg_classic_state
{
  std::uint8_t key_stage{};

  friend auto operator==(syscfg_classic_state const&, syscfg_classic_state const&)
      -> bool = default;
};

/**
 * @brief Classic SYSCFG of the STM32L1, RM0038 section 8.5, and of the STM32L47x/L48x, RM0351
 *        section 9.2, its register roles from the variant. `SYSCFG_EXTICR1` to `SYSCFG_EXTICR4`
 *        select the GPIO port of the EXTI lines 0 to 15 through the codes of the variant, a code
 *        with no port for that line selects none, and the EXTI peer reads them. A selector write
 *        that moves a line to a pin of another level hands the EXTI peer that edge, as the line
 *        input changes on the switch. The USB D+ pull-up enable, `SYSCFG_PMC.USB_PU`, is published
 *        through usb_pull_up_source, and every change of it, by a write or a reset, is told to the
 *        USB peer through usb_pull_up_listener. The other fields of its register, `LCD_CAPA` on the
 *        STM32L1, and the registers the variant lists as storage, `SYSCFG_MEMRMP` on the STM32L1,
 *        `MEMRMP`, `CFGR1`, `CFGR2` and `SWPR` on the STM32L4, are storage the model does not
 *        model: the machine boots from main flash, which BOOT_MODE and MEM_MODE 00 give. A firmware
 *        write that sets a bit of those fields or lands on such a register logs one model_warning
 *        per register, and raises model_error under strict_peripherals before it is stored. The
 *        STM32L4 SRAM2 erase, RM0351 9.2.7 and 9.2.10: `SYSCFG_SKR` written 0xCA then 0x53 unlocks
 *        `SCSR.SRAM2ER`, any other value locks it again, and SKR reads 0. A write of SRAM2ER 1
 *        while unlocked and no erase runs sets SRAM2ER and SRAM2BSY at the write and locks SRAM2ER
 *        again, inferred. After 8192 HCLK periods at the write's rate, one 32-bit word per cycle
 *        over the 32 KB, inferred, the model writes 0 to the whole SRAM2 as a DMA master and clears
 *        both bits. A locked write of SRAM2ER 1 leaves it 0, a write during the erase changes
 *        nothing. A CPU write to SRAM2 during the erase lands and is zeroed at its end, and a read
 *        returns the current content, inferred, ES0250 2.2.19. A read protection regression from
 *        level 1 to level 0 writes 0 to SRAM2 at once, RM0351 3.5.1, inferred without SRAM2BSY.
 *        Every reset brings the registers to their description reset and locks SRAM2ER. A SYSCFGRST
 *        pulse also locks it and keeps the registers the variant lists, `SYSCFG_MEMRMP` on the
 *        STM32L1, RM0038 8.5.1, `MEMRMP`, `SCSR`, `CFGR2` and `SWPR` on the STM32L4, inferred, and
 *        a running erase completes. The state, the key stage, is saved on a family with the erase
 *        only, the STM32L1 model is stateless.
 */
class syscfg_classic final : public dispatch_model<syscfg_classic, syscfg_classic_state>,
                             public exti_selector,
                             public usb_pull_up_source,
                             public bus_reset_listener,
                             public read_protection_listener
{
public:
  syscfg_classic(
      model_context&                ctx,
      peripheral_desc const&        desc,
      syscfg_classic_variant const& variant);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;

  [[nodiscard]] auto selected_port(std::uint8_t line) const -> std::optional<std::uint8_t> override;
  [[nodiscard]] auto usb_pull_up() const -> bool override;
  [[nodiscard]] auto stateless() const -> bool override;
  [[nodiscard]] auto storage_only(std::uint32_t offset) const -> bool override;
  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;
  auto               on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;

  void on_read_protection_regression() override;

  template <std::size_t Register> void selector_written(std::uint32_t old, std::uint32_t now);
  void                                 key_written(std::uint32_t old, std::uint32_t now);
  void                                 erase_written(std::uint32_t old, std::uint32_t now);

private:
  [[nodiscard]] auto port_of(std::uint8_t line, std::uint32_t code) const
      -> std::optional<std::uint8_t>;
  [[nodiscard]] auto level_of(std::optional<std::uint8_t> port, std::uint8_t line) const -> bool;
  void               reselect(std::uint32_t offset, std::uint32_t old, std::uint32_t now);
  void               restore_reset_values();
  void               tell_pull_up(bool before);
  [[nodiscard]] auto erasing() const -> bool;
  void               zero_erase_region();

  register_file&                        regs_;
  syscfg_classic_variant                variant_;
  std::array<field_ref, selected_lines> selectors_{};
  std::optional<field_ref>              pull_up_;
  std::uint32_t                         pull_up_storage_{};
  std::vector<std::uint32_t>            kept_;
  std::vector<std::uint32_t>            storage_;
  std::optional<field_ref>              erase_;
  std::optional<field_ref>              busy_;
  std::optional<reg_ref>                key_;
  region_desc const*                    region_{};
  core::clock_domain                    hclk_;
  storage_notes                         notes_;
};

auto syscfg_classic_writes(peripheral_desc const& desc, syscfg_classic_variant const& variant)
    -> std::vector<write_entry<syscfg_classic>>
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
    if(not variant.erase_register.empty() and described.name == variant.erase_register)
    {
      entry.on_write = &syscfg_classic::erase_written;
    }
    if(not variant.key_register.empty() and described.name == variant.key_register)
    {
      entry.on_write = &syscfg_classic::key_written;
    }
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
    model_context&                ctx,
    peripheral_desc const&        desc,
    syscfg_classic_variant const& variant)
    : dispatch_model{ctx, desc, "SYSCFG", syscfg_classic_writes(desc, variant)}, regs_{ctx.regs},
      variant_{variant}, hclk_{ctx.clock.domain("hclk")}
{
  for(std::size_t line = 0; line < selected_lines; ++line)
  {
    selectors_[line] =
        field("EXTICR" + std::to_string(line / 4 + 1), "EXTI" + std::to_string(line));
  }
  if(not variant.usb_pull_up_register.empty())
  {
    pull_up_ = field(variant.usb_pull_up_register, variant.usb_pull_up_field);
    for(auto const& other : describe(variant.usb_pull_up_register).fields)
    {
      if(other.name != variant.usb_pull_up_field)
      {
        pull_up_storage_ |= field(variant.usb_pull_up_register, other.name).mask();
      }
    }
  }
  for(auto const name : variant.kept_registers)
  {
    kept_.push_back(reg(name).offset);
  }
  for(auto const name : variant.storage_registers)
  {
    storage_.push_back(reg(name).offset);
  }
  if(not variant.erase_register.empty())
  {
    erase_ = field(variant.erase_register, variant.erase_field);
    busy_  = field(variant.erase_register, variant.busy_field);
    key_   = reg(variant.key_register);
    auto const found =
        std::ranges::find(ctx.part.regions, variant.erase_region, &region_desc::name);
    if(found == ctx.part.regions.end())
    {
      throw config_error(
          ctx.part.part, 0, std::format("regions.{}", variant.erase_region),
          "the memory region the SYSCFG erase writes");
    }
    region_ = &*found;
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
  auto const before = usb_pull_up();
  restore_reset_values();
  state_.key_stage = 0;
  tell_pull_up(before);
}

void syscfg_classic::tell_pull_up(bool before)
{
  if(usb_pull_up() == before)
  {
    return;
  }
  if(auto* const usb = dynamic_cast<usb_pull_up_listener*>(ctx_.peers.find(variant_.usb_peer)))
  {
    usb->usb_pull_up_changed();
  }
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
  std::vector<std::pair<std::uint32_t, std::uint32_t>> kept;
  for(auto const offset : kept_)
  {
    kept.emplace_back(offset, regs_.peek(offset));
  }
  auto const before = usb_pull_up();
  restore_reset_values();
  state_.key_stage = 0;
  for(auto const& [offset, value] : kept)
  {
    regs_.poke(offset, value);
  }
  for(auto const& [offset, old] : selectors)
  {
    reselect(offset, old, regs_.peek(offset));
  }
  tell_pull_up(before);
}

void syscfg_classic::on_event(std::uint32_t kind, std::uint64_t)
{
  if(kind != erase_done)
  {
    return;
  }
  zero_erase_region();
  regs_.poke(erase_->offset, 0);
}

auto syscfg_classic::erasing() const -> bool
{
  return busy_ and get(regs_, *busy_) != 0;
}

void syscfg_classic::zero_erase_region()
{
  std::vector<std::byte> const zeros(region_->size);
  if(not ctx_.services.write_block(region_->start, zeros, initiator::dma))
  {
    throw model_error(desc_.name, std::string{variant_.erase_region}, 0);
  }
}

void syscfg_classic::on_read_protection_regression()
{
  if(region_ != nullptr)
  {
    zero_erase_region();
  }
}

void syscfg_classic::key_written(std::uint32_t, std::uint32_t now)
{
  auto const key   = now & 0xFFu;
  state_.key_stage = key == first_key ? 1 : key == second_key and state_.key_stage == 1 ? 2 : 0;
  regs_.poke(key_->offset, 0);
}

void syscfg_classic::erase_written(std::uint32_t old, std::uint32_t now)
{
  if(erasing())
  {
    regs_.poke(erase_->offset, old);
    return;
  }
  if(erase_->of(now) == 0 or state_.key_stage != 2)
  {
    regs_.poke(erase_->offset, 0);
    return;
  }
  state_.key_stage = 0;
  regs_.poke(erase_->offset, erase_->mask() | busy_->mask());
  auto const hz = std::max<std::uint64_t>(ctx_.clock.hz(hclk_), 1);
  auto const ps = (std::uint64_t{region_->size / 4} * 1'000'000'000'000u + hz - 1) / hz;
  ctx_.events.schedule_at(vtime{ctx_.clock.time().ps + ps}, erase_done, 0, ctx_.clock);
}

auto syscfg_classic::port_of(std::uint8_t line, std::uint32_t code) const
    -> std::optional<std::uint8_t>
{
  auto const found = std::ranges::find(variant_.ports, code, &exti_port_code::code);
  if(found == variant_.ports.end() or ((found->lines >> line) & 1u) == 0)
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
  return variant_.erase_register.empty();
}

auto syscfg_classic::storage_only(std::uint32_t offset) const -> bool
{
  return std::ranges::contains(storage_, offset);
}

auto syscfg_classic::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  auto const word = offset & ~3u;
  if(pull_up_
     and word == pull_up_->offset
     and (merge_write(regs.peek(word), offset, size, value) & pull_up_storage_) != 0)
  {
    notes_.written(ctx_, desc_, "SYSCFG", word, ctx, {});
  }
  auto const before = usb_pull_up();
  auto       result = dispatch_model::on_write(regs, offset, size, old, value, ctx);
  tell_pull_up(before);
  return result;
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

auto syscfg_classic_factory(syscfg_classic_variant variant) -> model_factory
{
  return [variant](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<syscfg_classic>(ctx, desc, variant);
  };
}

namespace
{
constexpr auto syscfg_table = std::to_array<builtin>({
    row_of<^^syscfg_classic_factory, ^^l1_syscfg>(
        {.ip = "SYSCFG", .ip_version_glob = "", .families = family::l1}),
    row_of<^^syscfg_classic_factory, ^^l1_syscfg_cat12>(
        {.ip = "SYSCFG", .ip_version_glob = "STM32L1_exti_23", .families = family::l1}),
    row_of<^^syscfg_classic_factory, ^^l4_syscfg>(
        {.ip = "SYS", .ip_version_glob = "STM32L47x_sys_*", .families = family::l4}),
    row_of<^^syscfg_factory>(
        {.ip = "SYS", .ip_version_glob = "STM32U5x_sys_*", .families = family::u5}),
});
}

auto syscfg_rows() -> std::span<builtin const>
{
  return syscfg_table;
}
}
