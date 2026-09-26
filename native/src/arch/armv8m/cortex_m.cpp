#include "cortex_m.hpp"

#include "exceptions.hpp"
#include "registers.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/services.hpp>

#include <reflex/serde/bson.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace popcorn::arch::armv8m
{
namespace
{
constexpr std::uint32_t excp_bkpt           = 7;
constexpr std::uint32_t excp_exception_exit = 8;
constexpr std::uint32_t reset_lr            = 0xffff'ffff;
constexpr std::uint32_t xpsr_thumb          = 0x0100'0000;
constexpr std::uint32_t ccr_reset           = 0x0000'0201;
constexpr std::uint32_t fpccr_reset         = 0xc000'0000;

auto profile_of(core_desc const& core) -> profile
{
  auto const found = find_profile(core.cpu);
  if(not found)
  {
    throw config_error(
        "description", 0, "core.cpu",
        std::format("a core the Cortex-M module implements, such as CM33, not {}", core.cpu));
  }
  return *found;
}

auto cpuid_of(core_desc const& core, profile const& found) -> std::uint32_t
{
  auto const& text     = core.revision;
  auto const  p        = text.find('p');
  auto        variant  = 0u;
  auto        revision = 0u;
  auto const  parse    = [](std::string_view digits, unsigned& out) {
    auto const [end, err] = std::from_chars(digits.data(), digits.data() + digits.size(), out);
    return err
        == std::errc{}
       and end
        == digits.data()
         + digits.size()
       and not digits.empty()
       and out
         < 16;
  };
  if(not text.starts_with('r')
     or p
     == std::string::npos
     or not parse(std::string_view{text}.substr(1, p - 1), variant)
     or not parse(std::string_view{text}.substr(p + 1), revision))
  {
    throw config_error(
        "description", 0, "core.revision",
        std::format("a revision spelled rNpM, such as r0p4, not {}", text));
  }
  return 0x41u << 24 | variant << 20 | 0xfu << 16 | std::uint32_t{found.cpuid_part} << 4 | revision;
}

void require(uc_err err, std::string_view what)
{
  if(err != UC_ERR_OK)
  {
    throw emulation_error(std::format("cortex_m {}: {}", what, uc_strerror(err)));
  }
}

auto bit_words(std::uint32_t count) -> std::vector<std::uint32_t>
{
  return std::vector<std::uint32_t>((count + 31) / 32, 0);
}

}

auto cortex_m::mmio_read(uc_struct*, std::uint64_t offset, unsigned size, void* user) noexcept
    -> std::uint64_t
{
  auto const& access = *static_cast<window_user const*>(user);
  try
  {
    return access.self->space_.read(access.which, static_cast<std::uint32_t>(offset), size);
  }
  catch(...)
  {
    access.self->keep_failure(std::current_exception());
    return 0;
  }
}

void cortex_m::mmio_write(
    uc_struct*,
    std::uint64_t offset,
    unsigned      size,
    std::uint64_t value,
    void*         user) noexcept
{
  auto const& access = *static_cast<window_user const*>(user);
  try
  {
    access.self->space_.write(
        access.which, static_cast<std::uint32_t>(offset), size, static_cast<std::uint32_t>(value));
  }
  catch(...)
  {
    access.self->keep_failure(std::current_exception());
  }
}

cortex_m::cortex_m(core_desc const& core)
    : profile_{profile_of(core)}, core_{core}, cpuid_{cpuid_of(core, profile_)}
{}

cortex_m::~cortex_m() = default;

void cortex_m::context_free::operator()(uc_context* saved) const noexcept
{
  uc_context_free(saved);
}

auto cortex_m::engine() const -> engine_spec
{
  return {
      .arch      = UC_ARCH_ARM,
      .mode      = UC_MODE_THUMB | (profile_.mclass_mode ? UC_MODE_MCLASS : 0),
      .cpu_model = profile_.engine_model,
  };
}

void cortex_m::attach(arch_context const& ctx)
{
  for(auto const& unit : ctx.description.peripherals)
  {
    auto const first = std::uint64_t{unit.base};
    auto const last  = first + unit.size;
    for(auto const base : window_bases)
    {
      if(first < std::uint64_t{base} + window_size and base < last)
      {
        throw config_error(
            ctx.description.part, 0, std::format("peripherals.{}", unit.name),
            std::format(
                "no peripheral in the core's system space, {:#010x}-{:#010x}, served by the "
                "Cortex-M module",
                base, base + window_size - 1));
      }
    }
  }
  context_.emplace(ctx);
  for(auto const [index, base] : std::views::enumerate(window_bases))
  {
    auto& user = windows_[static_cast<std::size_t>(index)];
    user       = {.self = this, .which = static_cast<window>(index)};
    require(
        uc_mmio_map(ctx.engine, base, window_size, &mmio_read, &user, &mmio_write, &user),
        std::format("mapping the system space at {:#010x}", base));
  }
  uc_context* saved = nullptr;
  require(uc_context_alloc(ctx.engine, &saved), "allocating the pristine context");
  pristine_.reset(saved);
  require(uc_context_save(ctx.engine, saved), "saving the pristine context");
}

auto cortex_m::reset_state(address vector_table) const -> state
{
  auto const count = exception_count();
  state      fresh{};
  fresh.exceptions = {
      .priority = std::vector<std::uint8_t>(count, 0),
      .enabled  = bit_words(count),
      .pending  = bit_words(count),
      .active   = bit_words(count),
      .level    = bit_words(count),
  };
  fresh.scs.vtor     = vector_table;
  fresh.scs.ccr      = ccr_reset;
  fresh.scs.fpccr    = core_.fpu ? fpccr_reset : 0;
  fresh.scs.mpu_rbar = std::vector<std::uint32_t>(core_.mpu_regions, 0);
  fresh.scs.mpu_rlar = std::vector<std::uint32_t>(core_.mpu_regions, 0);
  return fresh;
}

void cortex_m::reset(address reset_vector)
{
  rethrow_failure();
  auto* const engine = engine_handle();
  require(uc_context_restore(engine, pristine_.get()), "restoring the pristine context");
  auto&      services = context_->services;
  auto const sp       = services.read(reset_vector, 4, initiator::cpu);
  auto const entry    = services.read(reset_vector + 4, 4, initiator::cpu);
  if(not sp or not entry)
  {
    throw emulation_error(std::format("cortex_m reset: no vector table at {:#010x}", reset_vector));
  }
  state_ = reset_state(reset_vector);
  for(auto const& reg : registers())
  {
    if(reg.name.starts_with('r') or reg.name.starts_with('s') or reg.name == "fpscr")
    {
      write(reg.engine_id, 0);
    }
  }
  write(UC_ARM_REG_LR, reset_lr);
  write(UC_ARM_REG_XPSR_NZCVQG, (*entry & 1u) != 0 ? xpsr_thumb : 0u);
  write(UC_ARM_REG_PC, *entry);
  state_.special = {.msp = *sp & ~3u};
  set_context(engine, state_.special);
  set_pending(false);
}

auto cortex_m::deliver_pending() -> trap_result
{
  rethrow_failure();
  auto const best = best_pending(state_.exceptions);
  if(not best or not preempts(*best))
  {
    return trap_result::resume();
  }
  return enter(*best);
}

auto cortex_m::deliverable_now() const -> bool
{
  auto const best = best_pending(state_.exceptions);
  return best and preempts(*best);
}

auto cortex_m::preempts(std::uint32_t number) const -> bool
{
  std::array<int, 3>           ids{UC_ARM_REG_PRIMASK, UC_ARM_REG_BASEPRI, UC_ARM_REG_FAULTMASK};
  std::array<std::uint32_t, 3> held{};
  std::array<void*, 3>         out{&held[0], &held[1], &held[2]};
  require(
      uc_reg_read_batch(engine_handle(), ids.data(), out.data(), static_cast<int>(ids.size())),
      "reading the priority masks");
  auto const current = execution_priority(
      state_.exceptions, state_.scs.prigroup,
      {.primask   = (held[0] & 1u) != 0,
       .basepri   = static_cast<std::uint8_t>(held[1]),
       .faultmask = (held[2] & 1u) != 0});
  return group_priority(priority_of(state_.exceptions, number), state_.scs.prigroup) < current;
}

void cortex_m::set_irq_line(std::uint32_t source, bool active)
{
  auto const number = 16 + source;
  contract_assert(number < exception_count());
  auto&      exceptions = state_.exceptions;
  auto const was        = test_bit(exceptions.level, number);
  put_bit(exceptions.level, number, active);
  if(active and not was)
  {
    put_bit(exceptions.pending, number, true);
  }
  refresh_pending();
}

auto cortex_m::on_interrupt(std::uint32_t intno) -> trap_result
{
  if(intno == excp_exception_exit)
  {
    return exception_return();
  }
  if(intno == excp_bkpt)
  {
    write(UC_ARM_REG_PC, (pc() + 2) | 1u);
    return trap_result::stop(stop_reason::user);
  }
  return trap_result::fault({.kind = fault_kind::invalid, .fault_address = pc(), .detail = intno});
}

auto cortex_m::on_invalid_insn() -> trap_result
{
  return trap_result::fault({.kind = fault_kind::usage, .fault_address = pc()});
}

auto cortex_m::on_unmapped(address addr, memory_op) -> trap_result
{
  return trap_result::fault({.kind = fault_kind::unmapped, .fault_address = addr});
}

void cortex_m::on_event(std::uint32_t, std::uint64_t)
{}

auto cortex_m::idle() const -> bool
{
  rethrow_failure();
  return false;
}

auto cortex_m::pc() const -> address
{
  return read(UC_ARM_REG_PC);
}

auto cortex_m::resume_address() const -> address
{
  return pc() | 1u;
}

auto cortex_m::registers() const -> std::span<cpu_register const>
{
  return core_.fpu ? std::span<cpu_register const>{fpu_registers} : integer_registers;
}

void cortex_m::prepare_call(call_frame const&)
{
  throw not_implemented("cortex_m::prepare_call");
}

auto cortex_m::call_result(call_frame const&) const -> std::uint64_t
{
  throw not_implemented("cortex_m::call_result");
}

void cortex_m::read_args(call_frame&) const
{
  throw not_implemented("cortex_m::read_args");
}

auto cortex_m::return_address() const -> address
{
  return read(UC_ARM_REG_LR) & ~address{1};
}

void cortex_m::return_to_caller(std::optional<call_arg> const&, float_abi)
{
  throw not_implemented("cortex_m::return_to_caller");
}

void cortex_m::save(std::vector<std::byte>& out) const
{
  auto saved    = state_;
  saved.special = read_special(engine_handle());
  std::vector<std::byte> bytes;
  reflex::serde::bson::serializer{bytes}.dump(saved);
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void cortex_m::load(std::span<std::byte const> in)
{
  state_ = reflex::serde::bson::deserializer{in}.load<state>();
  set_context(engine_handle(), state_.special);
  refresh_pending();
}

auto cortex_m::cpuid() const noexcept -> std::uint32_t
{
  return cpuid_;
}

auto cortex_m::exception_count() const noexcept -> std::uint32_t
{
  return 16u + core_.num_irqs;
}

auto cortex_m::is_pending(std::uint32_t number) const noexcept -> bool
{
  return test_bit(state_.exceptions.pending, number);
}

auto cortex_m::is_active(std::uint32_t number) const noexcept -> bool
{
  return test_bit(state_.exceptions.active, number);
}

auto cortex_m::is_enabled(std::uint32_t number) const noexcept -> bool
{
  return number < 16 ? number >= 2 : test_bit(state_.exceptions.enabled, number);
}

void cortex_m::set_exception_pending(std::uint32_t number, bool value)
{
  put_bit(state_.exceptions.pending, number, value);
  refresh_pending();
}

void cortex_m::set_exception_enabled(std::uint32_t number, bool value)
{
  put_bit(state_.exceptions.enabled, number, value);
  refresh_pending();
}

void cortex_m::keep_failure(std::exception_ptr failure) noexcept
{
  if(not failure_)
  {
    failure_ = std::move(failure);
  }
  if(context_)
  {
    context_->services.end_slice();
  }
}

auto cortex_m::engine_handle() const -> uc_struct*
{
  return context_->engine;
}

auto cortex_m::read(int engine_id) const -> std::uint32_t
{
  std::uint32_t value = 0;
  require(uc_reg_read(engine_handle(), engine_id, &value), "reading a register");
  return value;
}

void cortex_m::write(int engine_id, std::uint32_t value) const
{
  require(uc_reg_write(engine_handle(), engine_id, &value), "writing a register");
}

void cortex_m::refresh_pending() noexcept
{
  auto any = false;
  for(std::uint32_t number = 2; number < exception_count() and not any; ++number)
  {
    any = is_pending(number) and is_enabled(number);
  }
  set_pending(any);
}

void cortex_m::rethrow_failure() const
{
  if(failure_)
  {
    std::rethrow_exception(std::exchange(failure_, nullptr));
  }
}

}
