#include "system_space.hpp"

#include "cortex_m.hpp"
#include "exceptions.hpp"

#include <popcorn/core/services.hpp>

#include <unicorn/unicorn.h>

#include <array>
#include <cstdint>
#include <tuple>

namespace popcorn::arch::armv8m
{
namespace
{
constexpr std::uint32_t aircr_key         = 0x05fa;
constexpr std::uint32_t aircr_key_stat    = 0xfa05;
constexpr std::uint32_t aircr_sysresetreq = 1u << 2;
constexpr std::uint32_t ccr_read_one      = 0x0000'0201;
constexpr std::uint32_t ccr_writable = (1u << 1) | (1u << 3) | (1u << 4) | (1u << 8) | (1u << 10);
constexpr std::uint32_t ccr_v7_writable =
    (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 8) | (1u << 9);
constexpr std::uint32_t rbar_valid              = 1u << 4;
constexpr std::uint32_t scr_writable            = (1u << 1) | (1u << 2) | (1u << 4);
constexpr std::uint32_t vtor_writable           = 0xffff'ff80;
constexpr std::uint32_t syst_calib              = 0x4000'0000;
constexpr std::uint32_t cpacr_writable          = 0x00f0'0000;
constexpr std::uint32_t dwt_numcomp             = 4u << 28;
constexpr std::uint32_t shcsr_enables           = 0x0007'0000;
constexpr std::uint32_t aircr_bfhfnmins         = 1u << 13;
constexpr std::uint32_t aircr_pris              = 1u << 14;
constexpr std::uint32_t aircr_sysresetreqs      = 1u << 3;
constexpr std::uint32_t scr_sleepdeep           = 1u << 2;
constexpr std::uint32_t scr_sleepdeeps          = 1u << 3;
constexpr std::uint32_t scr_banked              = (1u << 1) | (1u << 4);
constexpr std::uint32_t ccr_bfhfnmign           = 1u << 8;
constexpr std::uint32_t shcsr_memfaultena       = 1u << 16;
constexpr std::uint32_t shcsr_busfaultena       = 1u << 17;
constexpr std::uint32_t shcsr_usgfaultena       = 1u << 18;
constexpr std::uint32_t shcsr_securefaultena    = 1u << 19;
constexpr std::uint32_t shcsr_securefaultpended = 1u << 20;
constexpr std::uint32_t shcsr_securefaultact    = 1u << 4;
constexpr std::uint32_t cfsr_bfsr               = 0x0000'ff00;
constexpr std::uint32_t nsacr_writable          = 0x0000'0cff;
constexpr std::uint32_t demcr_sdme              = 1u << 20;
constexpr std::uint32_t sau_ctrl_writable       = 0x3;
constexpr std::uint32_t sau_rbar_writable       = 0xffff'ffe0;
constexpr std::uint32_t sau_rlar_writable       = 0xffff'ffe3;

constexpr std::array<bool, 16> system_priority_implemented{false, false, false, false, true,  true,
                                                           true,  false, false, false, false, true,
                                                           true,  false, true,  true};

struct shcsr_bit
{
  std::uint32_t bit;
  std::uint32_t exception;
};

constexpr std::array shcsr_active{
    shcsr_bit{0,  exc_memmanage },
    shcsr_bit{1,  exc_busfault  },
    shcsr_bit{2,  exc_hardfault },
    shcsr_bit{3,  exc_usagefault},
    shcsr_bit{5,  exc_nmi       },
    shcsr_bit{7,  exc_svcall    },
    shcsr_bit{8,  exc_debugmon  },
    shcsr_bit{10, exc_pendsv    },
    shcsr_bit{11, exc_systick   }
};

constexpr std::array shcsr_pended{
    shcsr_bit{12, exc_usagefault},
    shcsr_bit{13, exc_memmanage },
    shcsr_bit{14, exc_busfault  },
    shcsr_bit{15, exc_svcall    }
};

constexpr std::array<std::uint32_t, 3> fpv4_media_features{0x1011'0021, 0x1100'0011, 0x0000'0000};
constexpr std::array<std::uint32_t, 3> fpv5_sp_media_features{
    0x1011'0021, 0x1100'0011, 0x0000'0040};
constexpr std::array<std::uint32_t, 3> fpv5_dp_media_features{
    0x1011'0221, 0x1200'0011, 0x0000'0040};
constexpr std::uint32_t ccr_caches = (1u << 16) | (1u << 17) | (1u << 18);

constexpr auto merge(std::uint32_t old, std::uint32_t value, std::uint32_t mask) noexcept
    -> std::uint32_t
{
  return (old & ~mask) | (value & mask);
}

constexpr auto in(std::uint32_t offset, std::uint32_t first, std::uint32_t last) noexcept -> bool
{
  return offset >= first and offset <= last;
}

auto byte_addressed(std::uint32_t offset) noexcept -> bool
{
  return in(offset, 0x400, 0x5ef) or in(offset, 0xd18, 0xd23) or in(offset, 0xd28, 0xd2b);
}

auto bit(std::uint32_t value, std::uint32_t at) noexcept -> bool
{
  return ((value >> at) & 1u) != 0;
}
}

auto system_space::read(window which, std::uint32_t offset, unsigned size) const -> std::uint32_t
{
  auto const view = core_.view_of(which);
  if(not view)
  {
    return 0;
  }
  auto const word    = read_word(which, offset & ~3u, *view);
  auto const shifted = word >> (8 * (offset & 3u));
  return size >= 4 ? shifted : shifted & ((1u << (8 * size)) - 1);
}

void system_space::write(window which, std::uint32_t offset, unsigned size, std::uint32_t value)
{
  auto const view = core_.view_of(which);
  if(not view)
  {
    return;
  }
  core_.nvic_changed();
  if(which == window::scs_nonsecure)
  {
    which = window::scs;
  }
  if(which == window::scs and byte_addressed(offset))
  {
    for(unsigned at = 0; at < size and at < 4; ++at)
    {
      write_scs_byte(offset + at, static_cast<std::uint8_t>(value >> (8 * at)), *view);
    }
    return;
  }
  auto const shift = 8 * (offset & 3u);
  auto const mask  = size >= 4 ? ~0u : ((1u << (8 * size)) - 1) << shift;
  write_word(which, offset & ~3u, value << shift, mask, *view);
}

auto system_space::read_word(window which, std::uint32_t offset, security view) const
    -> std::uint32_t
{
  auto const& scs = core_.state_.scs;
  switch(which)
  {
    case window::itm:
      if(offset < 0x80)
      {
        return 1;
      }
      if(offset == 0xe00)
      {
        return scs.itm_ter;
      }
      if(offset == 0xe40)
      {
        return scs.itm_tpr;
      }
      if(offset == 0xe80)
      {
        return scs.itm_tcr;
      }
      return 0;
    case window::dwt:
      if(offset == 0x000)
      {
        return dwt_numcomp | (scs.dwt_ctrl & 1u);
      }
      if(offset == 0x004)
      {
        return core_.read_cyccnt();
      }
      if(in(offset, 0x020, 0x05c) and (offset & 0x4) == 0)
      {
        return scs.dwt_comparators[(offset - 0x020) / 8];
      }
      return 0;
    case window::fpb:
      return 0;
    case window::scs:
    case window::scs_nonsecure:
      return read_scs(offset, view);
  }
  return 0;
}

auto system_space::irq_visible(std::uint32_t number, security view) const noexcept -> bool
{
  return view == security::secure or test_bit(core_.state_.exceptions.targets_nonsecure, number);
}

auto system_space::priority_slot(std::uint32_t number, security view) const -> std::uint8_t*
{
  auto&      exceptions = core_.state_.exceptions;
  auto const bfhfnmins  = core_.state_.scs.aircr_bfhfnmins;
  if(number >= exc_irq0)
  {
    return number < core_.exception_count() and irq_visible(number, view)
             ? &exceptions.priority[number]
             : nullptr;
  }
  if(view == security::secure)
  {
    auto const implemented =
        system_priority_implemented[number] or (number == exc_securefault and core_.trustzone());
    return implemented ? &exceptions.priority[number] : nullptr;
  }
  switch(number)
  {
    case exc_memmanage:
    case exc_usagefault:
    case exc_svcall:
    case exc_pendsv:
    case exc_systick:
      return &exceptions.nonsecure_priority[number];
    case exc_busfault:
      return bfhfnmins ? &exceptions.priority[number] : nullptr;
    case exc_debugmon:
      return (core_.state_.scs.demcr & demcr_sdme) == 0 ? &exceptions.priority[number] : nullptr;
    default:
      return nullptr;
  }
}

auto system_space::priority_byte(std::uint32_t offset, security view) const -> std::uint8_t
{
  auto const number =
      in(offset, 0x400, 0x5ef) ? exc_irq0 + (offset - 0x400) : exc_memmanage + (offset - 0xd18);
  auto const* const slot = priority_slot(number, view);
  return slot != nullptr ? *slot : 0;
}

auto system_space::read_icsr(security view) const -> std::uint32_t
{
  auto const nonsecure = view == security::nonsecure;
  auto const pended    = [&](std::uint32_t number) {
    return nonsecure ? bit(core_.state_.exceptions.nonsecure_pending, number)
                     : core_.is_pending(number);
  };
  std::uint32_t icsr = core_.read(UC_ARM_REG_IPSR) & 0x1ffu;
  if(not nonsecure or core_.state_.scs.aircr_bfhfnmins)
  {
    icsr |= core_.is_pending(exc_nmi) ? 1u << 31 : 0u;
  }
  icsr |= pended(exc_pendsv) ? 1u << 28 : 0u;
  icsr |= pended(exc_systick) ? 1u << 26 : 0u;
  auto const& exceptions = core_.state_.exceptions;
  if(auto const best = best_pending(exceptions))
  {
    icsr |= (*best & 0x1ffu) << 12;
  }
  auto irq_pending = false;
  auto active      = 0u;
  for(std::uint32_t number = 1; number < core_.exception_count(); ++number)
  {
    irq_pending = irq_pending or (number >= exc_irq0 and core_.is_pending(number));
    active += core_.is_active(number) ? 1u : 0u;
  }
  icsr |= irq_pending ? 1u << 22 : 0u;
  icsr |= active <= 1 ? 1u << 11 : 0u;
  return icsr;
}

namespace
{
enum class shcsr_owner : std::uint8_t
{
  banked,
  busfault,
  debug,
};

struct shcsr_nonsecure_bit
{
  std::uint32_t bit;
  std::uint32_t exception;
  shcsr_owner   owner;
  bool          pended;
};

constexpr std::array shcsr_nonsecure{
    shcsr_nonsecure_bit{0,  exc_memmanage,  shcsr_owner::banked,   false},
    shcsr_nonsecure_bit{1,  exc_busfault,   shcsr_owner::busfault, false},
    shcsr_nonsecure_bit{2,  exc_hardfault,  shcsr_owner::banked,   false},
    shcsr_nonsecure_bit{3,  exc_usagefault, shcsr_owner::banked,   false},
    shcsr_nonsecure_bit{5,  exc_nmi,        shcsr_owner::busfault, false},
    shcsr_nonsecure_bit{7,  exc_svcall,     shcsr_owner::banked,   false},
    shcsr_nonsecure_bit{8,  exc_debugmon,   shcsr_owner::debug,    false},
    shcsr_nonsecure_bit{10, exc_pendsv,     shcsr_owner::banked,   false},
    shcsr_nonsecure_bit{11, exc_systick,    shcsr_owner::banked,   false},
    shcsr_nonsecure_bit{12, exc_usagefault, shcsr_owner::banked,   true },
    shcsr_nonsecure_bit{13, exc_memmanage,  shcsr_owner::banked,   true },
    shcsr_nonsecure_bit{14, exc_busfault,   shcsr_owner::busfault, true },
    shcsr_nonsecure_bit{15, exc_svcall,     shcsr_owner::banked,   true },
    shcsr_nonsecure_bit{21, exc_hardfault,  shcsr_owner::busfault, true },
};
}

auto system_space::read_shcsr(security view) const -> std::uint32_t
{
  auto const& scs = core_.state_.scs;
  if(view == security::secure)
  {
    std::uint32_t shcsr = scs.shcsr_enables;
    for(auto const& entry : shcsr_active)
    {
      auto const secure_only = entry.exception == exc_hardfault or entry.exception == exc_nmi;
      if(core_.v7() and secure_only)
      {
        continue;
      }
      shcsr |= core_.is_active(entry.exception) ? 1u << entry.bit : 0u;
    }
    for(auto const& entry : shcsr_pended)
    {
      shcsr |= core_.is_pending(entry.exception) ? 1u << entry.bit : 0u;
    }
    if(core_.trustzone())
    {
      shcsr |= core_.is_active(exc_securefault) ? shcsr_securefaultact : 0u;
      shcsr |= core_.is_pending(exc_securefault) ? shcsr_securefaultpended : 0u;
    }
    return shcsr;
  }
  auto const&   exceptions = core_.state_.exceptions;
  auto const    bfhfnmins  = scs.aircr_bfhfnmins;
  std::uint32_t shcsr      = (scs.nonsecure.shcsr_enables & (shcsr_memfaultena | shcsr_usgfaultena))
                           | (bfhfnmins ? scs.shcsr_enables & shcsr_busfaultena : 0u);
  for(auto const& entry : shcsr_nonsecure)
  {
    auto set = false;
    switch(entry.owner)
    {
      case shcsr_owner::banked:
        set =
            bit(entry.pended ? exceptions.nonsecure_pending : exceptions.nonsecure_active,
                entry.exception);
        break;
      case shcsr_owner::busfault:
        set = bfhfnmins
          and (entry.exception == exc_hardfault
                   ? bit(exceptions.nonsecure_pending, exc_hardfault)
                   : (entry.pended ? core_.is_pending(entry.exception)
                                   : core_.is_active(entry.exception)));
        break;
      case shcsr_owner::debug:
        set = (scs.demcr & demcr_sdme) == 0 and core_.is_active(entry.exception);
        break;
    }
    shcsr |= set ? 1u << entry.bit : 0u;
  }
  return shcsr;
}

auto system_space::read_scs(std::uint32_t offset, security view) const -> std::uint32_t
{
  auto const& scs       = core_.state_.scs;
  auto const  nonsecure = view == security::nonsecure;
  auto const  secure_tz = core_.trustzone() and not nonsecure;
  auto const  bfhfnmins = scs.aircr_bfhfnmins;
  auto const  irq_words = (core_.core_.num_irqs + 31u) / 32u;
  auto const  irq_bits  = [&](std::vector<std::uint32_t> const& bits, std::uint32_t index) {
    std::uint32_t out = 0;
    for(std::uint32_t at = 0; at < 32; ++at)
    {
      auto const number = exc_irq0 + 32 * index + at;
      if(number < core_.exception_count()
         and irq_visible(number, view)
         and bit(bits[number / 32], number % 32))
      {
        out |= 1u << at;
      }
    }
    return out;
  };
  auto const& exceptions = core_.state_.exceptions;
  if(in(offset, 0x100, 0x13c))
  {
    return (offset - 0x100) / 4 < irq_words ? irq_bits(exceptions.enabled, (offset - 0x100) / 4)
                                            : 0;
  }
  if(in(offset, 0x180, 0x1bc))
  {
    return (offset - 0x180) / 4 < irq_words ? irq_bits(exceptions.enabled, (offset - 0x180) / 4)
                                            : 0;
  }
  if(in(offset, 0x200, 0x23c))
  {
    return (offset - 0x200) / 4 < irq_words ? irq_bits(exceptions.pending, (offset - 0x200) / 4)
                                            : 0;
  }
  if(in(offset, 0x280, 0x2bc))
  {
    return (offset - 0x280) / 4 < irq_words ? irq_bits(exceptions.pending, (offset - 0x280) / 4)
                                            : 0;
  }
  if(in(offset, 0x300, 0x33c))
  {
    return (offset - 0x300) / 4 < irq_words ? irq_bits(exceptions.active, (offset - 0x300) / 4) : 0;
  }
  if(in(offset, 0x380, 0x3bc))
  {
    return secure_tz and (offset - 0x380) / 4 < irq_words
             ? irq_bits(exceptions.targets_nonsecure, (offset - 0x380) / 4)
             : 0;
  }
  if(in(offset, 0x400, 0x5ec) or in(offset, 0xd18, 0xd20))
  {
    std::uint32_t out = 0;
    for(std::uint32_t at = 0; at < 4; ++at)
    {
      out |= std::uint32_t{priority_byte(offset + at, view)} << (8 * at);
    }
    return out;
  }
  if(in(offset, 0xd40, 0xd7c))
  {
    return core_.profile_.ids[(offset - 0xd40) / 4];
  }
  if(in(offset, 0xf40, 0xf48))
  {
    if(not core_.core_.fpu)
    {
      return 0;
    }
    auto const& table = not core_.profile_.fpv5 ? fpv4_media_features
                      : core_.core_.fpu_double  ? fpv5_dp_media_features
                                                : fpv5_sp_media_features;
    return table[(offset - 0xf40) / 4];
  }
  if(core_.v7() and (in(offset, 0xd9c, 0xdb8) or offset == 0xdc0 or offset == 0xdc4))
  {
    if(offset >= 0xdc0 or scs.mpu_rnr >= scs.mpu_rbar.size())
    {
      return 0;
    }
    return (offset - 0xd9c) % 8 == 0 ? scs.mpu_rbar[scs.mpu_rnr] | scs.mpu_rnr
                                     : scs.mpu_rlar[scs.mpu_rnr];
  }
  auto const mpu = core_.mpu_of(view);
  if(in(offset, 0xda4, 0xdbc))
  {
    auto const region = mpu.rnr + 1 + (offset - 0xda4) / 8;
    if(region >= mpu.rbar.size())
    {
      return 0;
    }
    return (offset - 0xda4) % 8 == 0 ? mpu.rbar[region] : mpu.rlar[region];
  }
  if(in(offset, 0xdd0, 0xde8))
  {
    if(not secure_tz)
    {
      return 0;
    }
    switch(offset)
    {
      case 0xdd0:
        return scs.sau_ctrl;
      case 0xdd4:
        return core_.core_.sau_regions;
      case 0xdd8:
        return scs.sau_rnr;
      case 0xddc:
        return scs.sau_rnr < scs.sau_rbar.size() ? scs.sau_rbar[scs.sau_rnr] : 0;
      case 0xde0:
        return scs.sau_rnr < scs.sau_rlar.size() ? scs.sau_rlar[scs.sau_rnr] : 0;
      case 0xde4:
        return scs.sfsr;
      case 0xde8:
        return scs.sfar;
      default:
        return 0;
    }
  }
  auto const& ns = scs.nonsecure;
  switch(offset)
  {
    case 0x004:
      return irq_words == 0 ? 0 : irq_words - 1;
    case 0x008:
      return scs.actlr;
    case 0x010:
    case 0x014:
    case 0x018:
      return core_.read_systick(offset, view);
    case 0x01c:
      return syst_calib;
    case 0xd00:
      return core_.cpuid();
    case 0xd04:
      return read_icsr(view);
    case 0xd08:
      return (nonsecure ? ns.vtor : scs.vtor) & vtor_writable;
    case 0xd0c:
    {
      auto aircr = aircr_key_stat << 16 | (nonsecure ? ns.prigroup : scs.prigroup) << 8;
      if(core_.trustzone())
      {
        aircr |= bfhfnmins ? aircr_bfhfnmins : 0u;
        aircr |= not nonsecure and scs.aircr_pris ? aircr_pris : 0u;
        aircr |= not nonsecure and scs.aircr_sysresetreqs ? aircr_sysresetreqs : 0u;
      }
      return aircr;
    }
    case 0xd10:
      if(nonsecure)
      {
        return (ns.scr & scr_banked)
             | ((scs.scr & scr_sleepdeeps) == 0 ? scs.scr & scr_sleepdeep : 0u);
      }
      return scs.scr;
    case 0xd14:
      if(nonsecure)
      {
        return (ns.ccr & ~ccr_bfhfnmign) | (scs.ccr & ccr_bfhfnmign) | ccr_read_one;
      }
      return core_.v7() ? scs.ccr : scs.ccr | ccr_read_one;
    case 0xd24:
      return read_shcsr(view);
    case 0xd28:
      return nonsecure ? (ns.cfsr & ~cfsr_bfsr) | (bfhfnmins ? scs.cfsr & cfsr_bfsr : 0u)
                       : scs.cfsr;
    case 0xd2c:
      return not nonsecure or bfhfnmins ? scs.hfsr : 0;
    case 0xd30:
      return scs.dfsr;
    case 0xd34:
      return nonsecure ? ns.mmfar : scs.mmfar;
    case 0xd38:
      return not nonsecure or bfhfnmins ? scs.bfar : 0;
    case 0xd3c:
      return scs.afsr;
    case 0xd88:
      if(not core_.core_.fpu)
      {
        return 0;
      }
      return nonsecure ? ns.cpacr & core_.nonsecure_cpacr_mask() : scs.cpacr;
    case 0xd8c:
      return secure_tz ? scs.nsacr : 0;
    case 0xd90:
      return std::uint32_t{core_.mpu_regions_of(view)} << 8;
    case 0xd94:
      return mpu.ctrl;
    case 0xd98:
      return mpu.rnr;
    case 0xd9c:
      return mpu.rnr < mpu.rbar.size() ? mpu.rbar[mpu.rnr] : 0;
    case 0xda0:
      return mpu.rnr < mpu.rlar.size() ? mpu.rlar[mpu.rnr] : 0;
    case 0xdc0:
      return mpu.mair0;
    case 0xdc4:
      return mpu.mair1;
    case 0xdf8:
      return scs.dcrdr;
    case 0xdfc:
      return scs.demcr;
    case 0xf34:
      return core_.core_.fpu ? core_.read_fpccr(view) : 0;
    case 0xf38:
      return core_.core_.fpu ? (nonsecure ? ns.fpcar : scs.fpcar) : 0;
    case 0xf3c:
      return core_.core_.fpu ? (nonsecure ? ns.fpdscr : scs.fpdscr) : 0;
    default:
      return 0;
  }
}

void system_space::write_word(
    window        which,
    std::uint32_t offset,
    std::uint32_t value,
    std::uint32_t mask,
    security      view)
{
  auto& scs = core_.state_.scs;
  switch(which)
  {
    case window::itm:
      if(offset == 0xe00)
      {
        scs.itm_ter = merge(scs.itm_ter, value, mask);
      }
      else if(offset == 0xe40)
      {
        scs.itm_tpr = merge(scs.itm_tpr, value, mask);
      }
      else if(offset == 0xe80)
      {
        scs.itm_tcr = merge(scs.itm_tcr, value, mask);
      }
      return;
    case window::dwt:
      if(offset == 0x000)
      {
        auto const cycles = core_.read_cyccnt();
        scs.dwt_ctrl      = merge(scs.dwt_ctrl, value, mask & 1u);
        core_.rebase_cyccnt(cycles);
      }
      else if(offset == 0x004)
      {
        core_.rebase_cyccnt(merge(core_.read_cyccnt(), value, mask));
      }
      else if(in(offset, 0x020, 0x05c) and (offset & 0x4) == 0)
      {
        auto& slot = scs.dwt_comparators[(offset - 0x020) / 8];
        slot       = merge(slot, value, mask);
      }
      return;
    case window::fpb:
      return;
    case window::scs:
    case window::scs_nonsecure:
      write_scs(offset, value, mask, view);
      return;
  }
}

void system_space::write_icsr(std::uint32_t bits, security view)
{
  if(view == security::nonsecure)
  {
    if(core_.state_.scs.aircr_bfhfnmins)
    {
      if(bit(bits, 31))
      {
        core_.set_exception_pending(exc_nmi, true);
      }
      if(bit(bits, 30) and not test_bit(core_.state_.exceptions.level, exc_nmi))
      {
        core_.set_exception_pending(exc_nmi, false);
      }
    }
    for(auto const& [set, clear, number] : {
            std::tuple{28u, 27u, exc_pendsv },
            std::tuple{26u, 25u, exc_systick}
    })
    {
      if(bit(bits, set))
      {
        core_.set_nonsecure_pending(number, true);
      }
      if(bit(bits, clear))
      {
        core_.set_nonsecure_pending(number, false);
      }
    }
    return;
  }
  if(bit(bits, 31))
  {
    core_.set_exception_pending(exc_nmi, true);
  }
  if(bit(bits, 30) and not test_bit(core_.state_.exceptions.level, exc_nmi))
  {
    core_.set_exception_pending(exc_nmi, false);
  }
  if(bit(bits, 28))
  {
    core_.set_exception_pending(exc_pendsv, true);
  }
  if(bit(bits, 27))
  {
    core_.set_exception_pending(exc_pendsv, false);
  }
  if(bit(bits, 26))
  {
    core_.set_exception_pending(exc_systick, true);
  }
  if(bit(bits, 25))
  {
    core_.set_exception_pending(exc_systick, false);
  }
}

void system_space::write_shcsr(std::uint32_t value, std::uint32_t mask, security view)
{
  auto& scs = core_.state_.scs;
  if(view == security::secure)
  {
    auto const trustzone = core_.trustzone();
    scs.shcsr_enables    = merge(
        scs.shcsr_enables, value, mask & (shcsr_enables | (trustzone ? shcsr_securefaultena : 0u)));
    for(auto const& entry : shcsr_pended)
    {
      if(bit(mask, entry.bit))
      {
        core_.set_exception_pending(entry.exception, bit(value, entry.bit));
      }
    }
    if(trustzone and (mask & shcsr_securefaultpended) != 0)
    {
      core_.set_exception_pending(exc_securefault, (value & shcsr_securefaultpended) != 0);
    }
    return;
  }
  auto const bfhfnmins = scs.aircr_bfhfnmins;
  scs.nonsecure.shcsr_enables =
      merge(scs.nonsecure.shcsr_enables, value, mask & (shcsr_memfaultena | shcsr_usgfaultena));
  if(bfhfnmins)
  {
    scs.shcsr_enables = merge(scs.shcsr_enables, value, mask & shcsr_busfaultena);
  }
  for(auto const& entry : shcsr_nonsecure)
  {
    if(not entry.pended or not bit(mask, entry.bit))
    {
      continue;
    }
    auto const pend = bit(value, entry.bit);
    if(entry.owner == shcsr_owner::banked)
    {
      core_.set_nonsecure_pending(entry.exception, pend);
    }
    else if(bfhfnmins and entry.exception == exc_hardfault)
    {
      core_.set_nonsecure_pending(entry.exception, pend);
    }
    else if(bfhfnmins)
    {
      core_.set_exception_pending(entry.exception, pend);
    }
  }
}

void system_space::write_sau(std::uint32_t offset, std::uint32_t value, std::uint32_t mask)
{
  auto&      scs    = core_.state_.scs;
  auto const update = [&](std::uint32_t& slot, std::uint32_t writable, int engine_id) {
    auto const next = merge(slot, value, mask & writable);
    if(next == slot)
    {
      return;
    }
    slot = next;
    core_.write(engine_id, next);
    core_.sau_changed();
  };
  switch(offset)
  {
    case 0xdd0:
      update(scs.sau_ctrl, sau_ctrl_writable, UC_ARM_REG_SAU_CTRL);
      return;
    case 0xdd8:
      if(auto const region = merge(scs.sau_rnr, value, mask) & 0xffu; region < scs.sau_rbar.size())
      {
        scs.sau_rnr = region;
        core_.write(UC_ARM_REG_SAU_RNR, region);
      }
      return;
    case 0xddc:
      if(scs.sau_rnr < scs.sau_rbar.size())
      {
        update(scs.sau_rbar[scs.sau_rnr], sau_rbar_writable, UC_ARM_REG_SAU_RBAR);
      }
      return;
    case 0xde0:
      if(scs.sau_rnr < scs.sau_rlar.size())
      {
        update(scs.sau_rlar[scs.sau_rnr], sau_rlar_writable, UC_ARM_REG_SAU_RLAR);
      }
      return;
    case 0xde4:
      scs.sfsr &= ~(value & mask);
      return;
    case 0xde8:
      scs.sfar = merge(scs.sfar, value, mask);
      return;
    default:
      return;
  }
}

void system_space::write_scs(
    std::uint32_t offset,
    std::uint32_t value,
    std::uint32_t mask,
    security      view)
{
  auto&      scs       = core_.state_.scs;
  auto&      ns        = scs.nonsecure;
  auto const nonsecure = view == security::nonsecure;
  auto const secure_tz = core_.trustzone() and not nonsecure;
  auto const bfhfnmins = scs.aircr_bfhfnmins;
  auto const irq_words = (core_.core_.num_irqs + 31u) / 32u;
  auto const each_irq  = [&](std::uint32_t base, auto&& apply) {
    auto const index = (offset - base) / 4;
    if(index >= irq_words)
    {
      return;
    }
    for(std::uint32_t at = 0; at < 32; ++at)
    {
      auto const number = exc_irq0 + 32 * index + at;
      if(number < core_.exception_count() and irq_visible(number, view) and bit(value & mask, at))
      {
        apply(number);
      }
    }
  };
  if(in(offset, 0x100, 0x13c))
  {
    each_irq(0x100, [&](std::uint32_t number) { core_.set_exception_enabled(number, true); });
    return;
  }
  if(in(offset, 0x180, 0x1bc))
  {
    each_irq(0x180, [&](std::uint32_t number) { core_.set_exception_enabled(number, false); });
    return;
  }
  if(in(offset, 0x200, 0x23c))
  {
    each_irq(0x200, [&](std::uint32_t number) { core_.set_exception_pending(number, true); });
    return;
  }
  if(in(offset, 0x280, 0x2bc))
  {
    each_irq(0x280, [&](std::uint32_t number) {
      if(not test_bit(core_.state_.exceptions.level, number))
      {
        core_.set_exception_pending(number, false);
      }
    });
    return;
  }
  if(in(offset, 0x380, 0x3bc))
  {
    if(not secure_tz)
    {
      return;
    }
    auto const index = (offset - 0x380) / 4;
    for(std::uint32_t at = 0; index < irq_words and at < 32; ++at)
    {
      auto const number = exc_irq0 + 32 * index + at;
      if(number < core_.exception_count() and bit(mask, at))
      {
        put_bit(core_.state_.exceptions.targets_nonsecure, number, bit(value, at));
      }
    }
    return;
  }
  if(core_.v7() and (in(offset, 0xd9c, 0xdb8) or offset == 0xdc0 or offset == 0xdc4))
  {
    write_pmsav7(offset, value, mask);
    return;
  }
  auto mpu = core_.mpu_of(view);
  if(in(offset, 0xda4, 0xdbc))
  {
    auto const region = mpu.rnr + 1 + (offset - 0xda4) / 8;
    if(region < mpu.rbar.size())
    {
      auto& slot = (offset - 0xda4) % 8 == 0 ? mpu.rbar[region] : mpu.rlar[region];
      slot       = merge(slot, value, mask);
    }
    return;
  }
  if(in(offset, 0xdd0, 0xde8))
  {
    if(secure_tz)
    {
      write_sau(offset, value, mask);
    }
    return;
  }
  switch(offset)
  {
    case 0x008:
      scs.actlr = merge(scs.actlr, value, mask);
      return;
    case 0x010:
    case 0x014:
    case 0x018:
      core_.write_systick(offset, value, mask, view);
      return;
    case 0xd04:
      write_icsr(value & mask, view);
      return;
    case 0xd08:
    {
      auto& vtor = nonsecure ? ns.vtor : scs.vtor;
      vtor       = merge(vtor & vtor_writable, value, mask & vtor_writable);
      return;
    }
    case 0xd0c:
      if(mask == ~0u and value >> 16 == aircr_key)
      {
        (nonsecure ? ns.prigroup : scs.prigroup) = (value >> 8) & 0x7u;
        if(secure_tz)
        {
          scs.aircr_bfhfnmins    = (value & aircr_bfhfnmins) != 0;
          scs.aircr_pris         = (value & aircr_pris) != 0;
          scs.aircr_sysresetreqs = (value & aircr_sysresetreqs) != 0;
        }
        if((value & aircr_sysresetreq) != 0 and not(nonsecure and scs.aircr_sysresetreqs))
        {
          core_.context_->services.request_reset(reset_kind::software);
        }
      }
      return;
    case 0xd10:
      if(nonsecure)
      {
        ns.scr = merge(ns.scr, value, mask & scr_banked);
        if((scs.scr & scr_sleepdeeps) == 0)
        {
          scs.scr = merge(scs.scr, value, mask & scr_sleepdeep);
        }
        return;
      }
      scs.scr = merge(scs.scr, value, mask & (scr_writable | (secure_tz ? scr_sleepdeeps : 0u)));
      return;
    case 0xd14:
    {
      auto const writable =
          (core_.v7() ? ccr_v7_writable : ccr_writable) | (core_.profile_.caches ? ccr_caches : 0u);
      if(nonsecure)
      {
        ns.ccr = merge(ns.ccr, value, mask & writable & ~ccr_bfhfnmign);
        if(bfhfnmins)
        {
          scs.ccr = merge(scs.ccr, value, mask & ccr_bfhfnmign);
        }
        return;
      }
      scs.ccr = merge(scs.ccr, value, mask & writable);
      return;
    }
    case 0xd24:
      write_shcsr(value, mask, view);
      return;
    case 0xd2c:
      if(not nonsecure or bfhfnmins)
      {
        scs.hfsr &= ~(value & mask);
      }
      return;
    case 0xd30:
      scs.dfsr &= ~(value & mask);
      return;
    case 0xd34:
    {
      auto& mmfar = nonsecure ? ns.mmfar : scs.mmfar;
      mmfar       = merge(mmfar, value, mask);
      return;
    }
    case 0xd38:
      if(not nonsecure or bfhfnmins)
      {
        scs.bfar = merge(scs.bfar, value, mask);
      }
      return;
    case 0xd3c:
      scs.afsr = merge(scs.afsr, value, mask);
      return;
    case 0xd88:
      if(not core_.core_.fpu)
      {
        return;
      }
      if(nonsecure)
      {
        ns.cpacr = merge(ns.cpacr, value, mask & core_.nonsecure_cpacr_mask());
        return;
      }
      scs.cpacr = merge(scs.cpacr, value, mask & cpacr_writable);
      if(secure_tz)
      {
        core_.mirror_cpacr();
      }
      return;
    case 0xd8c:
      if(secure_tz)
      {
        scs.nsacr = merge(scs.nsacr, value, mask & nsacr_writable);
        core_.mirror_nsacr();
      }
      return;
    case 0xd94:
      mpu.ctrl = merge(mpu.ctrl, value, mask & 0x7u);
      return;
    case 0xd98:
      if(auto const region = merge(mpu.rnr, value, mask) & 0xffu; region < mpu.rbar.size())
      {
        mpu.rnr = region;
      }
      return;
    case 0xd9c:
      if(mpu.rnr < mpu.rbar.size())
      {
        mpu.rbar[mpu.rnr] = merge(mpu.rbar[mpu.rnr], value, mask);
      }
      return;
    case 0xda0:
      if(mpu.rnr < mpu.rlar.size())
      {
        mpu.rlar[mpu.rnr] = merge(mpu.rlar[mpu.rnr], value, mask);
      }
      return;
    case 0xdc0:
      mpu.mair0 = merge(mpu.mair0, value, mask);
      return;
    case 0xdc4:
      mpu.mair1 = merge(mpu.mair1, value, mask);
      return;
    case 0xdf8:
      scs.dcrdr = merge(scs.dcrdr, value, mask);
      return;
    case 0xdfc:
    {
      auto const cycles = core_.read_cyccnt();
      scs.demcr         = merge(scs.demcr, value, mask & (core_.trustzone() ? ~demcr_sdme : ~0u));
      core_.rebase_cyccnt(cycles);
      return;
    }
    case 0xf00:
    {
      auto const number      = exc_irq0 + ((value & mask) & 0x1ffu);
      auto const user_pended = bit(nonsecure ? ns.ccr : scs.ccr, 1);
      if(number < core_.exception_count()
         and irq_visible(number, view)
         and (user_pended or core_.privileged()))
      {
        core_.set_exception_pending(number, true);
      }
      return;
    }
    case 0xf34:
      core_.write_fpccr(value, mask, view);
      return;
    case 0xf38:
    {
      auto& fpcar = nonsecure ? ns.fpcar : scs.fpcar;
      fpcar       = merge(fpcar, value, mask);
      return;
    }
    case 0xf3c:
    {
      auto& fpdscr = nonsecure ? ns.fpdscr : scs.fpdscr;
      fpdscr       = merge(fpdscr, value, mask);
      return;
    }
    default:
      return;
  }
}

void system_space::write_scs_byte(std::uint32_t offset, std::uint8_t value, security view)
{
  auto const prio_mask =
      static_cast<std::uint8_t>((0xffu << (8u - core_.core_.nvic_prio_bits)) & 0xffu);
  if(in(offset, 0x400, 0x5ef) or in(offset, 0xd18, 0xd23))
  {
    auto const number =
        in(offset, 0x400, 0x5ef) ? exc_irq0 + (offset - 0x400) : exc_memmanage + (offset - 0xd18);
    if(auto* const slot = priority_slot(number, view))
    {
      *slot = value & prio_mask;
    }
    return;
  }
  auto& scs   = core_.state_.scs;
  auto  clear = std::uint32_t{value} << (8 * (offset - 0xd28));
  if(view == security::nonsecure)
  {
    scs.nonsecure.cfsr &= ~(clear & ~cfsr_bfsr);
    clear &= scs.aircr_bfhfnmins ? cfsr_bfsr : 0u;
  }
  scs.cfsr &= ~clear;
}

void system_space::write_pmsav7(std::uint32_t offset, std::uint32_t value, std::uint32_t mask)
{
  auto& scs = core_.state_.scs;
  if(offset >= 0xdc0)
  {
    return;
  }
  if((offset - 0xd9c) % 8 != 0)
  {
    if(scs.mpu_rnr < scs.mpu_rlar.size())
    {
      scs.mpu_rlar[scs.mpu_rnr] = merge(scs.mpu_rlar[scs.mpu_rnr], value, mask);
    }
    return;
  }
  auto const written = value & mask;
  if((written & rbar_valid) != 0 and (written & 0xfu) < scs.mpu_rbar.size())
  {
    scs.mpu_rnr = written & 0xfu;
  }
  if(scs.mpu_rnr < scs.mpu_rbar.size())
  {
    scs.mpu_rbar[scs.mpu_rnr] = merge(scs.mpu_rbar[scs.mpu_rnr], value, mask) & ~0x1fu;
  }
}
}
