#include "factories.hpp"
#include "model_base.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace popcorn::periph::stm32
{
namespace
{
struct otg_fs_state
{
  friend auto operator==(otg_fs_state const&, otg_fs_state const&) -> bool = default;
};

/**
 * @brief Core resets of the STM32U5 USB OTG_FS, RM0456 section 72.15.5, and no USB traffic.
 *        The self-clearing bits of OTG_GRSTCTL, CSRST, PSRST, FCRST, RXFFLSH and TXFFLSH, read
 *        0 again as soon as they are written: the reset or flush is done at once. AHBIDL keeps
 *        its reset value 1: the AHB master never leaves idle. The soft reset clears no other
 *        register. The other registers are storage: no host attaches, nothing enumerates, no
 *        interrupt is raised.
 */
class otg_fs final : public dispatch_model<otg_fs, otg_fs_state>
{
public:
  otg_fs(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t, std::uint64_t) override
  {}

  void reset_control_written(std::uint32_t old, std::uint32_t now);

private:
  register_file&           regs_;
  std::array<field_ref, 5> self_clearing_;
};

constexpr std::array<write_entry<otg_fs>, 1> otg_fs_writes{
    {
     {"GRSTCTL", &otg_fs::reset_control_written},
     }
};

otg_fs::otg_fs(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "OTG_FS", otg_fs_writes}, regs_{ctx.regs},
      self_clearing_{
          field("GRSTCTL", "CSRST"), field("GRSTCTL", "PSRST"), field("GRSTCTL", "FSRST"),
          field("GRSTCTL", "RXFFLSH"), field("GRSTCTL", "TXFFLSH")}
{}

void otg_fs::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
}

void otg_fs::reset_control_written(std::uint32_t, std::uint32_t)
{
  for(auto const bit : self_clearing_)
  {
    update(regs_, bit, 0);
  }
}
}

auto otg_fs_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<otg_fs>(ctx, desc);
  };
}
}
