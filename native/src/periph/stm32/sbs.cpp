#include "factories.hpp"
#include "model_base.hpp"
#include "secure_config.hpp"
#include "temporal_isolation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
struct sbs_state
{
  friend auto operator==(sbs_state const&, sbs_state const&) -> bool = default;
};

static_assert(std::is_empty_v<sbs_state>);

constexpr std::array<std::uint32_t, 4> level_codes{0xb4, 0x51, 0x8a, 0x6f};
constexpr std::uint32_t                no_increment = 0xb4;
constexpr std::uint8_t                 boot_level   = 1;
constexpr std::uint8_t                 top_level    = 3;

/**
 * @brief The STM32H5 SBS, RM0481 section 14: the temporal isolation level of 14.3.5, 14.5.1
 *        and 14.5.2 and the bus rules of 14.3.4 and 14.5. `SBS_HDPLSR` reads the HDPL code of
 *        Table 135, HDPL1 0x51 after every reset, the level of a user flash boot. A write of
 *        `SBS_HDPLCR` with any value but 0xB4 raises HDPL by one up to HDPL3 0x6F and drops
 *        the engine's page verdicts, so the FLASH HDP areas the level closes take effect at the
 *        next instruction. `SBS_DBGCR` and `SBS_DBGLOCKR` are RAZ/WI above HDPL1, RM0481 14.5.4
 *        and 14.5.5.
 *
 *        Under TrustZone `SBS_SECCFGR` and `SBS_RSSCMDR` are always Secure, `SBS_EPOCHSELCR`,
 *        `SBS_ECCNMIR` and `SBS_CSLCKR` Secure and privileged, `SBS_FPUIMR` privileged and
 *        Secure with FPUSEC set, `SBS_MESR`, `SBS_CCCSR`, `SBS_CCVALR` and `SBS_CCSWCR` Secure
 *        with SBSSEC set, `SBS_CFGR2` Secure with CLASSBSEC set, `SBS_CNSLCKR` privileged, the
 *        rules secure_config_rules shares with the U5 SYSCFG. A Non-secure access to a Secure
 *        one of these is RAZ/WI and raises the GTZC1 TZIC SBS event, SBSF of `GTZC1_TZIC_SR4`,
 *        RM0481 5.7.8, on a read and on a write. An unprivileged access to a privileged one is
 *        RAZ/WI with no event, RM0481 5.4. FPUSEC takes privileged writes only and reads for
 *        every Secure transaction, after the register description of 14.5.8: RM0481 14.3.4
 *        lists FPUSEC among the privileged registers, whose unprivileged reads are RAZ. SBSSEC
 *        makes the RCC SBS clock bit Secure, RM0481 11.5.1.
 *
 *        Without TrustZone every transaction is Non-secure, so `SBS_SECCFGR`, `SBS_RSSCMDR` and
 *        `SBS_CSLCKR` are RAZ/WI and their storage keeps its reset, and `SBS_EPOCHSELCR`,
 *        `SBS_ECCNMIR`, `SBS_FPUIMR` and `SBS_CNSLCKR` are privileged, RM0481 14.5.6 to
 *        14.5.8, 14.5.11 and 14.5.17 to 14.5.19.
 *
 *        The SBS has no RCC reset bit, RM0481 11.8.25: `RCC_APB3RSTR` bit 1 reads 0 after a
 *        write of 1 on an STM32H573 and every SBS register keeps its value, where the STM32H562,
 *        H563 and H573 SVDs place SBSRST. A power-on or brown-out reset restores every register,
 *        any other machine reset every one but `SBS_RSSCMDR`, which RM0481 14.5.6 clears by a
 *        power-on reset.
 *
 *        Every register but `SBS_HDPLCR`, `SBS_HDPLSR` and `SBS_SECCFGR` is storage the model
 *        does not model: a firmware write that lands, past the bus rules, logs one
 *        model_warning per register and raises model_error under strict_peripherals before it
 *        is stored, as the U5 SYSCFG. Firmware reads are silent. `SBS_PMCR` takes every
 *        access, the bits RM0481 14.5.9 makes Secure with a GPIOB pin, ADC12SEC or ETHSEC
 *        included. The level lives in `SBS_HDPLSR`, so the model is stateless.
 */
class sbs final : public dispatch_model<sbs, sbs_state>,
                  public temporal_isolation,
                  public trustzone_aware
{
public:
  sbs(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;

  [[nodiscard]] auto stateless() const -> bool override;
  [[nodiscard]] auto storage_only(std::uint32_t offset) const -> bool override;
  [[nodiscard]] auto isolation_level() const -> std::uint8_t override;
  [[nodiscard]] auto secure_function() const -> bool override;
  [[nodiscard]] auto secure_word() const -> std::uint32_t override;

  void increment_written(std::uint32_t old, std::uint32_t now);

  [[nodiscard]] auto guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard;

private:
  void               restore_reset_values(std::span<std::uint32_t const> kept);
  [[nodiscard]] auto rule(std::uint32_t word, access_ctx const& ctx) -> bus_guard;

  register_file&               regs_;
  field_ref                    increment_;
  field_ref                    level_;
  field_ref                    fpu_;
  reg_ref                      security_;
  std::uint32_t                always_secure_{};
  std::array<std::uint32_t, 2> secure_privileged_{};
  std::array<std::uint32_t, 2> debug_{};
  std::array<std::uint32_t, 2> isolation_words_{};
  secure_config_rules          rules_;
  storage_notes                notes_;
};

auto sbs_writes(peripheral_desc const& desc) -> std::vector<write_entry<sbs>>
{
  std::vector<write_entry<sbs>> out;
  auto const* const             increment = find_register(desc, "SBS_HDPLCR");
  for(auto const& described : desc.registers)
  {
    if(not described.alternate_of)
    {
      out.push_back({described.name, increment == &described ? &sbs::increment_written : nullptr});
    }
  }
  return out;
}

sbs::sbs(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{
          ctx, desc, "SBS", sbs_writes(desc)
},
      regs_{ctx.regs}, increment_{field("SBS_HDPLCR", "INCR_HDPL")},
      level_{field("SBS_HDPLSR", "HDPL")}, fpu_{field("SBS_SECCFGR", "FPUSEC")},
      security_{reg("SBS_SECCFGR")}, always_secure_{reg("SBS_RSSCMDR").offset},
      secure_privileged_{reg("SBS_EPOCHSELCR").offset, reg("SBS_ECCNMIR").offset},
      debug_{reg("SBS_DBGCR").offset, reg("SBS_DBGLOCKR").offset},
      isolation_words_{increment_.offset, level_.offset},
      rules_{
          .system         = field("SBS_SECCFGR", "SBSSEC"),
          .class_b        = field("SBS_SECCFGR", "CLASSBSEC"),
          .fpu            = fpu_,
          .secure_lock    = reg("SBS_CSLCKR").offset,
          .nonsecure_lock = reg("SBS_CNSLCKR").offset,
          .fpu_mask       = reg("SBS_FPUIMR").offset,
          .config2        = reg("SBS_CFGR2").offset,
          .compensation =
              {reg("SBS_MESR").offset, reg("SBS_CCCSR").offset, reg("SBS_CCVALR").offset,
               reg("SBS_CCSWCR").offset},
          .unit = unit_named(ctx.part, desc.name)}
{}

void sbs::restore_reset_values(std::span<std::uint32_t const> kept)
{
  for(auto const& name : claims())
  {
    auto const offset = reg(name).offset;
    if(not std::ranges::contains(kept, offset))
    {
      regs_.poke(offset, describe(name).reset);
    }
  }
}

void sbs::reset(register_file&, reset_kind kind)
{
  auto const       power_on = kind == reset_kind::power_on or kind == reset_kind::brown_out;
  std::array const kept{
      isolation_words_[0], isolation_words_[1], power_on ? isolation_words_[0] : always_secure_};
  restore_reset_values(kept);
  update(regs_, increment_, no_increment);
  update(regs_, level_, level_codes[boot_level]);
}

auto sbs::guard(std::uint32_t word, access_ctx const& ctx, bool write) -> bus_guard
{
  auto const result = rule(word, ctx);
  if(write and storage_only(word))
  {
    notes_.written(ctx_, desc_, "SBS", word, ctx, result);
  }
  return result;
}

auto sbs::rule(std::uint32_t word, access_ctx const& ctx) -> bus_guard
{
  auto const all       = ~std::uint32_t{};
  auto const trustzone = ctx_.trustzone;
  if(std::ranges::contains(debug_, word))
  {
    return bus_guard::both(isolation_level() > boot_level ? all : 0);
  }
  if(word == security_.offset)
  {
    if(not ctx.secure)
    {
      return rules_.refused(true, false, ctx, ctx_.peers);
    }
    return {.wi = ctx.privileged ? 0 : fpu_.mask()};
  }
  if(word == always_secure_)
  {
    return rules_.refused(true, false, ctx, ctx_.peers);
  }
  if(std::ranges::contains(secure_privileged_, word))
  {
    return rules_.refused(trustzone, true, ctx, ctx_.peers);
  }
  return rules_.guard(word, secure_word(), ctx, ctx_.peers, ctx_.part).value_or(bus_guard{});
}

void sbs::on_event(std::uint32_t, std::uint64_t)
{}

auto sbs::stateless() const -> bool
{
  return true;
}

auto sbs::storage_only(std::uint32_t offset) const -> bool
{
  return offset != security_.offset and not std::ranges::contains(isolation_words_, offset);
}

auto sbs::secure_word() const -> std::uint32_t
{
  return ctx_.trustzone ? regs_.peek(security_.offset) : 0;
}

auto sbs::secure_function() const -> bool
{
  return rules_.system.of(secure_word()) != 0;
}

auto sbs::isolation_level() const -> std::uint8_t
{
  auto const code  = get(regs_, level_);
  auto const found = std::ranges::find(level_codes, code);
  return found == level_codes.end() ? boot_level
                                    : static_cast<std::uint8_t>(found - level_codes.begin());
}

void sbs::increment_written(std::uint32_t, std::uint32_t now)
{
  auto const level = isolation_level();
  if(increment_.of(now) == no_increment or level == top_level)
  {
    return;
  }
  update(regs_, level_, level_codes[level + 1]);
  ctx_.services.memory_policy_changed();
  ctx_.services.end_after_instruction();
}
}

auto sbs_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<sbs>(ctx, desc);
  };
}
}
