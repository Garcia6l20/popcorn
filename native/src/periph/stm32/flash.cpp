#include "factories.hpp"
#include "model_base.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <utility>

namespace popcorn::periph::stm32
{
namespace
{
constexpr std::uint32_t key1 = 0x4567'0123;
constexpr std::uint32_t key2 = 0xcdef'89ab;

struct flash_state
{
  bool first_key{};
  bool locked_up{};

  friend auto operator==(flash_state const&, flash_state const&) -> bool = default;
};

/**
 * @brief FLASH control register lock of the STM32U5, RM0456 section 7, "Unlock the
 *        secure/nonsecure FLASH control registers": KEY1 then KEY2 in FLASH_NSKEYR clears
 *        FLASH_NSCR.LOCK, any other write to FLASH_NSKEYR is a bus error and keeps FLASH_NSCR
 *        locked until the next reset. A key write while already unlocked counts as a wrong
 *        sequence, a case the RM leaves implicit. FLASH_NSCR ignores writes while locked.
 *        Programming and erase are not modelled: their start bits are stored, then cleared, and
 *        the first request logs a warning. Option-byte unlock is not modelled either: OPTLOCK
 *        stays 1, so firmware polling for it spins until spin detection stops the run.
 */
class flash final : public dispatch_model<flash, flash_state>
{
public:
  flash(model_context& ctx, peripheral_desc const& desc);

  void reset(register_file& regs, reset_kind kind) override;
  void on_event(std::uint32_t kind, std::uint64_t payload) override;
  auto on_write(
      register_file& regs,
      std::uint32_t  offset,
      std::uint8_t   size,
      std::uint32_t  old,
      std::uint32_t  value,
      access_ctx     ctx) -> std::expected<void, bus_error> override;

  void key_written(std::uint32_t old, std::uint32_t now);
  void control_written(std::uint32_t old, std::uint32_t now);

private:
  register_file& regs_;
  reg_ref        key_;
  field_ref      lock_;
  field_ref      option_lock_;
  field_ref      start_;
  field_ref      option_start_;
  field_ref      program_;
  field_ref      page_erase_;
  bool           rejected_{};
  bool           program_logged_{};
};

constexpr std::array<write_entry<flash>, 2> flash_writes{
    {
     {"FLASH_NSKEYR", &flash::key_written},
     {"FLASH_NSCR", &flash::control_written},
     }
};

flash::flash(model_context& ctx, peripheral_desc const& desc)
    : dispatch_model{ctx, desc, "FLASH", flash_writes}, regs_{ctx.regs}, key_{reg("FLASH_NSKEYR")},
      lock_{field("FLASH_NSCR", "LOCK")}, option_lock_{field("FLASH_NSCR", "OPTLOCK")},
      start_{field("FLASH_NSCR", "STRT")}, option_start_{field("FLASH_NSCR", "OPTSTRT")},
      program_{field("FLASH_NSCR", "PG")}, page_erase_{field("FLASH_NSCR", "PER")}
{}

void flash::reset(register_file& regs, reset_kind)
{
  for(auto const& name : claims())
  {
    regs.poke(reg(name).offset, describe(name).reset);
  }
  state_ = {};
}

void flash::on_event(std::uint32_t, std::uint64_t)
{}

auto flash::on_write(
    register_file& regs,
    std::uint32_t  offset,
    std::uint8_t   size,
    std::uint32_t  old,
    std::uint32_t  value,
    access_ctx     ctx) -> std::expected<void, bus_error>
{
  auto const done = dispatch_model::on_write(regs, offset, size, old, value, ctx);
  if(std::exchange(rejected_, false))
  {
    return std::unexpected{bus_error::denied};
  }
  return done;
}

void flash::key_written(std::uint32_t, std::uint32_t now)
{
  regs_.poke(key_.offset, 0);
  auto const locked = get(regs_, lock_) != 0;
  if(not state_.locked_up and locked and not state_.first_key and now == key1)
  {
    state_.first_key = true;
    return;
  }
  if(not state_.locked_up and locked and state_.first_key and now == key2)
  {
    state_.first_key = false;
    update(regs_, lock_, 0);
    return;
  }
  state_.first_key = false;
  state_.locked_up = true;
  rejected_        = true;
}

void flash::control_written(std::uint32_t old, std::uint32_t now)
{
  if(lock_.of(old) != 0)
  {
    regs_.poke(lock_.offset, old);
    return;
  }
  auto const requested = program_.of(now) != 0 or page_erase_.of(now) != 0 or start_.of(now) != 0;
  if(requested and not std::exchange(program_logged_, true))
  {
    warn(
        "FLASH_NSCR",
        std::format(
            "{} requested, flash programming and erase are not modelled",
            program_.of(now) != 0 ? "PG" : (page_erase_.of(now) != 0 ? "PER" : "STRT")));
  }
  now = option_lock_.with(now, 1);
  now = start_.with(now, 0);
  now = option_start_.with(now, 0);
  regs_.poke(lock_.offset, now);
}
}

auto flash_factory() -> model_factory
{
  return [](model_context& ctx, peripheral_desc const& desc) -> std::shared_ptr<model> {
    return std::make_shared<flash>(ctx, desc);
  };
}
}
