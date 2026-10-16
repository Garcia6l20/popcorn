#include "cortex_m.hpp"
#include "exceptions.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/services.hpp>

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string_view>

namespace popcorn::arch::armv8m
{
namespace
{
constexpr std::uint32_t fpccr_lspact    = 1u << 0;
constexpr std::uint32_t fpccr_user      = 1u << 1;
constexpr std::uint32_t fpccr_s         = 1u << 2;
constexpr std::uint32_t fpccr_thread    = 1u << 3;
constexpr std::uint32_t fpccr_hfrdy     = 1u << 4;
constexpr std::uint32_t fpccr_mmrdy     = 1u << 5;
constexpr std::uint32_t fpccr_bfrdy     = 1u << 6;
constexpr std::uint32_t fpccr_monrdy    = 1u << 8;
constexpr std::uint32_t fpccr_splimviol = 1u << 9;
constexpr std::uint32_t fpccr_ufrdy     = 1u << 10;
constexpr std::uint32_t fpccr_clronrets = 1u << 27;
constexpr std::uint32_t fpccr_clronret  = 1u << 28;
constexpr std::uint32_t fpccr_lspens    = 1u << 29;
constexpr std::uint32_t fpccr_lspen     = 1u << 30;
constexpr std::uint32_t fpccr_aspen     = 1u << 31;
constexpr std::uint32_t fpccr_banked    = fpccr_lspact
                                        | fpccr_user
                                        | fpccr_thread
                                        | fpccr_mmrdy
                                        | fpccr_splimviol
                                        | fpccr_ufrdy
                                        | fpccr_aspen;
constexpr std::uint32_t demcr_sdme      = 1u << 20;
constexpr std::uint32_t nsacr_cp10      = 1u << 10;
constexpr std::uint32_t nsacr_cp11      = 1u << 11;
constexpr std::uint32_t cpacr_cp10      = 0x0030'0000;
constexpr std::uint32_t cpacr_cp11      = 0x00c0'0000;

auto nonsecure_ready(scs_state const& scs) noexcept -> std::uint32_t
{
  return (scs.aircr_bfhfnmins ? fpccr_hfrdy | fpccr_bfrdy : 0u)
       | ((scs.demcr & demcr_sdme) == 0 ? fpccr_monrdy : 0u);
}

void require(uc_err err, std::string_view what)
{
  if(err != UC_ERR_OK)
  {
    throw emulation_error(std::format("cortex_m {}: {}", what, uc_strerror(err)));
  }
}
}

void cortex_m::idau_lookup(
    void*         user,
    std::uint32_t address,
    int*          region,
    bool*         exempt,
    bool*         ns,
    bool*         nsc) noexcept
{
  auto const& ranges = static_cast<cortex_m const*>(user)->context_->idau;
  auto const  after  = std::ranges::upper_bound(ranges, address, {}, &idau_region::start);
  *exempt            = false;
  if(after == ranges.begin() or address >= std::prev(after)->end)
  {
    *region = -1;
    *ns     = false;
    *nsc    = false;
    return;
  }
  auto const found = std::prev(after);
  *region          = static_cast<int>(found - ranges.begin());
  *exempt          = found->exempt;
  *ns              = not found->nsc;
  *nsc             = found->nsc;
}

auto cortex_m::trustzone() const noexcept -> bool
{
  return trustzone_;
}

[[gnu::noinline]] auto cortex_m::deliver_banked() -> trap_result
{
  auto const best = best_pending(state_.exceptions, priority_config(), state_.call_masked);
  if(best.number == 0 or not preempts(best.number, best.state))
  {
    return trap_result::resume();
  }
  return enter(best.number, best.state);
}

[[gnu::noinline]] auto cortex_m::banked_deliverable() const -> bool
{
  auto const best = best_pending(state_.exceptions, priority_config(), state_.call_masked);
  return best.number != 0 and preempts(best.number, best.state);
}

auto cortex_m::view_of(window which) const noexcept -> std::optional<security>
{
  auto const secure_access = not trustzone() or access_.secure;
  if(which == window::scs_nonsecure)
  {
    return secure_access ? std::optional{security::nonsecure} : std::nullopt;
  }
  return secure_access ? security::secure : security::nonsecure;
}

auto cortex_m::mpu_of(security view) noexcept -> mpu_bank
{
  auto& scs = state_.scs;
  if(view == security::nonsecure)
  {
    auto& ns = scs.nonsecure;
    return {ns.mpu_ctrl, ns.mpu_rnr, ns.mair0, ns.mair1, ns.mpu_rbar, ns.mpu_rlar};
  }
  return {scs.mpu_ctrl, scs.mpu_rnr, scs.mair0, scs.mair1, scs.mpu_rbar, scs.mpu_rlar};
}

auto cortex_m::mpu_regions_of(security view) const noexcept -> std::uint8_t
{
  return trustzone() and view == security::secure ? core_.mpu_regions_secure : core_.mpu_regions;
}

auto cortex_m::nonsecure_cpacr_mask() const noexcept -> std::uint32_t
{
  auto const nsacr = state_.scs.nsacr;
  return ((nsacr & nsacr_cp10) != 0 ? cpacr_cp10 : 0u)
       | ((nsacr & nsacr_cp11) != 0 ? cpacr_cp11 : 0u);
}

void cortex_m::attach_security()
{
  auto* const engine = context_->engine;
  require(uc_ctl_arm_idau(engine, &idau_lookup, this), "installing the IDAU");
  require(uc_ctl_arm_security(engine, 1), "turning the security lookup on");
}

void cortex_m::apply_security(bool restored)
{
  auto* const engine = engine_handle();
  write_security(engine, state_.current);
  set_context(engine, state_.special, stack_limits());
  write_bank(
      engine, state_.current == security::secure ? security::nonsecure : security::secure,
      state_.other);
  mirror_sau();
  mirror_nsacr();
  if(core_.fpu)
  {
    mirror_cpacr();
    mirror_fpccr(restored ? (state_.scs.fpccr & fpccr_s) != 0 : engine_fpccr_s());
  }
  require(uc_ctl_flush_tlb(engine), "flushing the TLB");
}

void cortex_m::mirror_sau()
{
  auto const& scs  = state_.scs;
  auto const  keep = [&](int engine_id, std::uint32_t value) {
    if(read(engine_id) != value)
    {
      write(engine_id, value);
    }
  };
  for(std::uint32_t region = 0; region < scs.sau_rbar.size(); ++region)
  {
    write(UC_ARM_REG_SAU_RNR, region);
    keep(UC_ARM_REG_SAU_RBAR, scs.sau_rbar[region]);
    keep(UC_ARM_REG_SAU_RLAR, scs.sau_rlar[region]);
  }
  if(not scs.sau_rbar.empty())
  {
    write(UC_ARM_REG_SAU_RNR, scs.sau_rnr);
  }
  keep(UC_ARM_REG_SAU_CTRL, scs.sau_ctrl);
}

void cortex_m::mirror_cpacr() const
{
  write(UC_ARM_REG_CPACR_S, state_.scs.cpacr);
}

void cortex_m::mirror_nsacr() const
{
  write(UC_ARM_REG_NSACR, state_.scs.nsacr);
}

auto cortex_m::engine_fpccr_s() const -> bool
{
  return (read(UC_ARM_REG_FPCCR_S) & fpccr_s) != 0;
}

void cortex_m::mirror_fpccr(bool s_bit) const
{
  auto const& scs = state_.scs;
  write(
      UC_ARM_REG_FPCCR_S,
      (scs.fpccr & ~(fpccr_lspen | fpccr_lspact | fpccr_s)) | (s_bit ? fpccr_s : 0u));
  write(UC_ARM_REG_FPCCR_NS, scs.nonsecure.fpccr & fpccr_banked & ~fpccr_lspact);
}

auto cortex_m::read_fpccr(security view) const -> std::uint32_t
{
  auto const& scs = state_.scs;
  if(not trustzone())
  {
    return scs.fpccr;
  }
  if(view == security::secure)
  {
    return (scs.fpccr & ~(fpccr_s | fpccr_lspact)) | (engine_fpccr_s() ? fpccr_s : 0u);
  }
  auto const readable = fpccr_clronret | fpccr_lspen | nonsecure_ready(scs);
  return (scs.nonsecure.fpccr & fpccr_banked & ~fpccr_lspact) | (scs.fpccr & readable);
}

void cortex_m::write_fpccr(std::uint32_t value, std::uint32_t mask, security view)
{
  auto& scs = state_.scs;
  if(not trustzone())
  {
    scs.fpccr = (scs.fpccr & ~mask) | (value & mask);
    return;
  }
  if(not core_.fpu)
  {
    return;
  }
  auto const merge = [&](std::uint32_t old, std::uint32_t writable) {
    return (old & ~(mask & writable)) | (value & mask & writable);
  };
  if(view == security::secure)
  {
    auto const s_bit = (mask & fpccr_s) != 0 ? (value & fpccr_s) != 0 : engine_fpccr_s();
    scs.fpccr        = merge(scs.fpccr, ~fpccr_lspact);
    mirror_fpccr(s_bit);
    return;
  }
  auto const writable = ((scs.fpccr & fpccr_clronrets) == 0 ? fpccr_clronret : 0u)
                      | ((scs.fpccr & fpccr_lspens) == 0 ? fpccr_lspen : 0u)
                      | nonsecure_ready(scs);
  scs.nonsecure.fpccr = merge(scs.nonsecure.fpccr, fpccr_banked & ~fpccr_lspact);
  scs.fpccr           = merge(scs.fpccr, writable);
  mirror_fpccr(engine_fpccr_s());
}

void cortex_m::sau_changed()
{
  context_->services.end_after_instruction();
}

void cortex_m::set_nonsecure_pending(std::uint32_t number, bool value) noexcept
{
  if(not banked(number))
  {
    return;
  }
  auto& bits = state_.exceptions.nonsecure_pending;
  if(value and (state_.scs.nonsecure.scr & scr_sevonpend) != 0 and (bits >> number & 1u) == 0)
  {
    state_.event_register = true;
  }
  bits = value ? bits | 1u << number : bits & ~(1u << number);
  refresh_pending();
}

auto cortex_m::priority_config() const noexcept -> security_config
{
  auto const& scs = state_.scs;
  return {
      .trustzone          = trustzone(),
      .bfhfnmins          = scs.aircr_bfhfnmins,
      .pris               = scs.aircr_pris,
      .sdme               = (scs.demcr & demcr_sdme) != 0,
      .prigroup           = scs.prigroup,
      .prigroup_nonsecure = scs.nonsecure.prigroup,
  };
}

auto cortex_m::current_security() const -> security
{
  return trustzone() ? read_security(engine_handle()) : security::secure;
}

void cortex_m::pend(std::uint32_t number, security state)
{
  if(state == security::nonsecure and banked(number))
  {
    set_nonsecure_pending(number, true);
    return;
  }
  set_exception_pending(number, true);
}

[[gnu::noinline]] auto cortex_m::banked_priority(bool ignore_primask) const -> int
{
  std::array<int, 6> ids{UC_ARM_REG_PRIMASK_S,  UC_ARM_REG_BASEPRI_S,  UC_ARM_REG_FAULTMASK_S,
                         UC_ARM_REG_PRIMASK_NS, UC_ARM_REG_BASEPRI_NS, UC_ARM_REG_FAULTMASK_NS};
  std::array<std::uint32_t, 6> held{};
  std::array<void*, 6>         out{&held[0], &held[1], &held[2], &held[3], &held[4], &held[5]};
  require(
      uc_reg_read_batch(engine_handle(), ids.data(), out.data(), static_cast<int>(ids.size())),
      "reading the banked priority masks");
  auto const of = [&](std::size_t at) {
    return masks{
        .primask   = not ignore_primask and (held[at] & 1u) != 0,
        .basepri   = static_cast<std::uint8_t>(held[at + 1]),
        .faultmask = (held[at + 2] & 1u) != 0,
    };
  };
  return execution_priority(state_.exceptions, priority_config(), of(0), of(3));
}
}
