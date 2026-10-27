#include "debug_freeze.hpp"
#include "factories.hpp"
#include "model_base.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace popcorn::periph::stm32
{
namespace
{
struct dbgmcu_state
{
  friend auto operator==(dbgmcu_state const&, dbgmcu_state const&) -> bool = default;
};

/**
 * @brief DBGMCU of the STM32U5, RM0456 section 75.12. DBGMCU_IDCODE reads the description's
 *        reset value, read-only, reserved bits 15:12 included, as RM0456 gives it as
 *        0xXXXX6XXX. ST's HAL reads it to pick the ADC calibration sequence. The base read
 *        handler of a dispatch_model returns only the bits that fields cover, and the fields
 *        of DBGMCU_IDCODE are DEV_ID and REV_ID, so `on_read` returns the whole reset value.
 *        The freeze fields, `DBG_<peripheral>_STOP` in the freeze registers, are read by the
 *        models that freeze while the machine holds its core, through debug_freeze, at each
 *        hold and release. Deviation: a freeze write while the core is held takes effect at the
 *        next hold or release, on silicon at once. DBGMCU_CR is storage: no low-power debug or
 *        trace behaviour follows it. The registers are "not reset by a system reset, only by a
 *        power-on reset", 75.12: a power-on or brown-out reset restores them, other resets keep
 *        them.
 */
class dbgmcu final : public dispatch_model<dbgmcu, dbgmcu_state>, public debug_freeze
{
public:
  dbgmcu(model_context& ctx, peripheral_desc const& desc);

  [[nodiscard]] auto frozen(std::string_view name) const -> std::optional<bool> override;

  void reset(register_file& regs, reset_kind kind) override;
  auto on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
      -> std::expected<std::uint32_t, bus_error> override;
  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void stored(std::uint32_t, std::uint32_t)
  {}
};

auto dbgmcu_writes(peripheral_desc const& desc) -> std::vector<write_entry<dbgmcu>>
{
  std::vector<write_entry<dbgmcu>> out;
  for(auto const& described : desc.registers)
  {
    out.push_back({described.name, &dbgmcu::stored});
  }
  return out;
}

dbgmcu::dbgmcu(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "DBGMCU", dbgmcu_writes(desc)}
{}

auto dbgmcu::on_read(register_file& regs, std::uint32_t offset, std::uint8_t size, access_ctx ctx)
    -> std::expected<std::uint32_t, bus_error>
{
  if((offset & ~3u) == reg("IDCODE").offset)
  {
    return read_lanes(describe("IDCODE").reset, offset, size);
  }
  return dispatch_model::on_read(regs, offset, size, ctx);
}

auto dbgmcu::frozen(std::string_view name) const -> std::optional<bool>
{
  for(auto const& described : desc_.registers)
  {
    if(std::ranges::contains(described.fields, name, &field_desc::name))
    {
      auto const bits = field(described.name, name);
      return bits.of(ctx_.regs.peek(bits.offset)) != 0;
    }
  }
  return std::nullopt;
}

void dbgmcu::reset(register_file& regs, reset_kind kind)
{
  if(kind != reset_kind::power_on and kind != reset_kind::brown_out)
  {
    return;
  }
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
}
}

auto dbgmcu_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<dbgmcu>(ctx, desc);
  };
}
}
