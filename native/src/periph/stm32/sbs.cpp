#include "factories.hpp"
#include "model_base.hpp"
#include "temporal_isolation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
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
 * @brief The temporal isolation level of the STM32H5 SBS, RM0481 14.3.5, 14.5.1 and 14.5.2,
 *        under TrustZone. `SBS_HDPLSR` reads the HDPL code of Table 135, HDPL1 0x51 after every
 *        reset, the level of a user flash boot. A write of `SBS_HDPLCR` with any value but 0xB4
 *        raises HDPL by one up to HDPL3 0x6F and drops the engine's page verdicts, so the FLASH
 *        HDP areas the level closes take effect at the next instruction. An SBS RCC reset keeps
 *        both registers, which RM0481 resets by system reset only. The level lives in
 *        `SBS_HDPLSR`, so the model is stateless. The other SBS registers are storage. Without
 *        TrustZone the factory makes no model and the SBS is storage.
 */
class sbs final : public dispatch_model<sbs, sbs_state>,
                  public temporal_isolation,
                  public bus_reset_listener
{
public:
  sbs(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_bus_reset() override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;

  [[nodiscard]] auto stateless() const -> bool override;
  [[nodiscard]] auto isolation_level() const -> std::uint8_t override;

  void increment_written(std::uint32_t old, std::uint32_t now);

private:
  register_file& regs_;
  field_ref      increment_;
  field_ref      level_;
};

constexpr std::array<write_entry<sbs>, 2> sbs_writes{
    {
     {"SBS_HDPLCR", &sbs::increment_written},
     {"SBS_HDPLSR", nullptr},
     }
};

sbs::sbs(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "SBS", sbs_writes}, regs_{ctx.regs},
      increment_{field("SBS_HDPLCR", "INCR_HDPL")}, level_{field("SBS_HDPLSR", "HDPL")}
{}

void sbs::reset(register_file&, reset_kind)
{
  update(regs_, increment_, no_increment);
  update(regs_, level_, level_codes[boot_level]);
}

void sbs::on_bus_reset()
{}

void sbs::on_event(std::uint32_t, std::uint64_t)
{}

auto sbs::stateless() const -> bool
{
  return true;
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
    if(not ctx.trustzone)
    {
      return nullptr;
    }
    return std::make_shared<sbs>(ctx, desc);
  };
}
}
