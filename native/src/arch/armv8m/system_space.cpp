#include "system_space.hpp"

#include "cortex_m.hpp"
#include "exceptions.hpp"

#include <popcorn/core/services.hpp>

#include <unicorn/unicorn.h>

#include <array>
#include <cstdint>

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
constexpr std::uint32_t rbar_valid     = 1u << 4;
constexpr std::uint32_t scr_writable   = (1u << 1) | (1u << 2) | (1u << 4);
constexpr std::uint32_t vtor_writable  = 0xffff'ff80;
constexpr std::uint32_t syst_calib     = 0x4000'0000;
constexpr std::uint32_t cpacr_writable = 0x00f0'0000;
constexpr std::uint32_t dwt_numcomp    = 4u << 28;
constexpr std::uint32_t shcsr_enables  = 0x0007'0000;

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
  auto const word    = read_word(which, offset & ~3u);
  auto const shifted = word >> (8 * (offset & 3u));
  return size >= 4 ? shifted : shifted & ((1u << (8 * size)) - 1);
}

void system_space::write(window which, std::uint32_t offset, unsigned size, std::uint32_t value)
{
  if(which == window::scs and byte_addressed(offset))
  {
    for(unsigned at = 0; at < size and at < 4; ++at)
    {
      write_scs_byte(offset + at, static_cast<std::uint8_t>(value >> (8 * at)));
    }
    return;
  }
  auto const shift = 8 * (offset & 3u);
  auto const mask  = size >= 4 ? ~0u : ((1u << (8 * size)) - 1) << shift;
  write_word(which, offset & ~3u, value << shift, mask);
}

auto system_space::read_word(window which, std::uint32_t offset) const -> std::uint32_t
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
      return read_scs(offset);
  }
  return 0;
}

auto system_space::priority_byte(std::uint32_t offset) const -> std::uint8_t
{
  auto const& priority = core_.state_.exceptions.priority;
  if(in(offset, 0x400, 0x5ef))
  {
    auto const number = exc_irq0 + (offset - 0x400);
    return number < core_.exception_count() ? priority[number] : 0;
  }
  auto const number = exc_memmanage + (offset - 0xd18);
  return system_priority_implemented[number] ? priority[number] : 0;
}

auto system_space::read_scs(std::uint32_t offset) const -> std::uint32_t
{
  auto const& scs       = core_.state_.scs;
  auto const  irq_words = (core_.core_.num_irqs + 31u) / 32u;
  auto const  irq_bits  = [&](std::vector<std::uint32_t> const& bits, std::uint32_t index) {
    std::uint32_t out = 0;
    for(std::uint32_t at = 0; at < 32; ++at)
    {
      auto const number = exc_irq0 + 32 * index + at;
      if(number < core_.exception_count() and bit(bits[number / 32], number % 32))
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
  if(in(offset, 0x400, 0x5ec) or in(offset, 0xd18, 0xd20))
  {
    std::uint32_t out = 0;
    for(std::uint32_t at = 0; at < 4; ++at)
    {
      out |= std::uint32_t{priority_byte(offset + at)} << (8 * at);
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
  if(in(offset, 0xda4, 0xdbc))
  {
    auto const region = scs.mpu_rnr + 1 + (offset - 0xda4) / 8;
    if(region >= scs.mpu_rbar.size())
    {
      return 0;
    }
    return (offset - 0xda4) % 8 == 0 ? scs.mpu_rbar[region] : scs.mpu_rlar[region];
  }
  switch(offset)
  {
    case 0x004:
      return irq_words == 0 ? 0 : irq_words - 1;
    case 0x008:
      return scs.actlr;
    case 0x010:
    case 0x014:
    case 0x018:
      return core_.read_systick(offset);
    case 0x01c:
      return syst_calib;
    case 0xd00:
      return core_.cpuid();
    case 0xd04:
    {
      std::uint32_t icsr = core_.read(UC_ARM_REG_IPSR) & 0x1ffu;
      icsr |= core_.is_pending(exc_nmi) ? 1u << 31 : 0u;
      icsr |= core_.is_pending(exc_pendsv) ? 1u << 28 : 0u;
      icsr |= core_.is_pending(exc_systick) ? 1u << 26 : 0u;
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
    case 0xd08:
      return scs.vtor & vtor_writable;
    case 0xd0c:
      return aircr_key_stat << 16 | scs.prigroup << 8;
    case 0xd10:
      return scs.scr;
    case 0xd14:
      return core_.v7() ? scs.ccr : scs.ccr | ccr_read_one;
    case 0xd24:
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
      return shcsr;
    }
    case 0xd28:
      return scs.cfsr;
    case 0xd2c:
      return scs.hfsr;
    case 0xd30:
      return scs.dfsr;
    case 0xd34:
      return scs.mmfar;
    case 0xd38:
      return scs.bfar;
    case 0xd3c:
      return scs.afsr;
    case 0xd88:
      return core_.core_.fpu ? scs.cpacr : 0;
    case 0xd90:
      return std::uint32_t{core_.core_.mpu_regions} << 8;
    case 0xd94:
      return scs.mpu_ctrl;
    case 0xd98:
      return scs.mpu_rnr;
    case 0xd9c:
      return scs.mpu_rnr < scs.mpu_rbar.size() ? scs.mpu_rbar[scs.mpu_rnr] : 0;
    case 0xda0:
      return scs.mpu_rnr < scs.mpu_rlar.size() ? scs.mpu_rlar[scs.mpu_rnr] : 0;
    case 0xdc0:
      return scs.mair0;
    case 0xdc4:
      return scs.mair1;
    case 0xdf8:
      return scs.dcrdr;
    case 0xdfc:
      return scs.demcr;
    case 0xf34:
      return core_.core_.fpu ? scs.fpccr : 0;
    case 0xf38:
      return core_.core_.fpu ? scs.fpcar : 0;
    case 0xf3c:
      return core_.core_.fpu ? scs.fpdscr : 0;
    default:
      return 0;
  }
}

void system_space::write_word(
    window        which,
    std::uint32_t offset,
    std::uint32_t value,
    std::uint32_t mask)
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
      write_scs(offset, value, mask);
      return;
  }
}

void system_space::write_scs(std::uint32_t offset, std::uint32_t value, std::uint32_t mask)
{
  auto&      scs       = core_.state_.scs;
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
      if(number < core_.exception_count() and bit(value & mask, at))
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
  if(core_.v7() and (in(offset, 0xd9c, 0xdb8) or offset == 0xdc0 or offset == 0xdc4))
  {
    write_pmsav7(offset, value, mask);
    return;
  }
  if(in(offset, 0xda4, 0xdbc))
  {
    auto const region = scs.mpu_rnr + 1 + (offset - 0xda4) / 8;
    if(region < scs.mpu_rbar.size())
    {
      auto& slot = (offset - 0xda4) % 8 == 0 ? scs.mpu_rbar[region] : scs.mpu_rlar[region];
      slot       = merge(slot, value, mask);
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
      core_.write_systick(offset, value, mask);
      return;
    case 0xd04:
    {
      auto const bits = value & mask;
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
      return;
    }
    case 0xd08:
      scs.vtor = merge(scs.vtor & vtor_writable, value, mask & vtor_writable);
      return;
    case 0xd0c:
      if(mask == ~0u and value >> 16 == aircr_key)
      {
        scs.prigroup = (value >> 8) & 0x7u;
        if((value & aircr_sysresetreq) != 0)
        {
          core_.context_->services.request_reset(reset_kind::software);
        }
      }
      return;
    case 0xd10:
      scs.scr = merge(scs.scr, value, mask & scr_writable);
      return;
    case 0xd14:
      scs.ccr = merge(
          scs.ccr, value,
          mask
              & ((core_.v7() ? ccr_v7_writable : ccr_writable)
                 | (core_.profile_.caches ? ccr_caches : 0u)));
      return;
    case 0xd24:
      scs.shcsr_enables = merge(scs.shcsr_enables, value, mask & shcsr_enables);
      for(auto const& entry : shcsr_pended)
      {
        if(bit(mask, entry.bit))
        {
          core_.set_exception_pending(entry.exception, bit(value, entry.bit));
        }
      }
      return;
    case 0xd2c:
      scs.hfsr &= ~(value & mask);
      return;
    case 0xd30:
      scs.dfsr &= ~(value & mask);
      return;
    case 0xd34:
      scs.mmfar = merge(scs.mmfar, value, mask);
      return;
    case 0xd38:
      scs.bfar = merge(scs.bfar, value, mask);
      return;
    case 0xd3c:
      scs.afsr = merge(scs.afsr, value, mask);
      return;
    case 0xd88:
      scs.cpacr = merge(scs.cpacr, value, mask & (core_.core_.fpu ? cpacr_writable : 0u));
      return;
    case 0xd94:
      scs.mpu_ctrl = merge(scs.mpu_ctrl, value, mask & 0x7u);
      return;
    case 0xd98:
      if(auto const region = merge(scs.mpu_rnr, value, mask) & 0xffu;
         region < core_.core_.mpu_regions)
      {
        scs.mpu_rnr = region;
      }
      return;
    case 0xd9c:
      if(scs.mpu_rnr < scs.mpu_rbar.size())
      {
        scs.mpu_rbar[scs.mpu_rnr] = merge(scs.mpu_rbar[scs.mpu_rnr], value, mask);
      }
      return;
    case 0xda0:
      if(scs.mpu_rnr < scs.mpu_rlar.size())
      {
        scs.mpu_rlar[scs.mpu_rnr] = merge(scs.mpu_rlar[scs.mpu_rnr], value, mask);
      }
      return;
    case 0xdc0:
      scs.mair0 = merge(scs.mair0, value, mask);
      return;
    case 0xdc4:
      scs.mair1 = merge(scs.mair1, value, mask);
      return;
    case 0xdf8:
      scs.dcrdr = merge(scs.dcrdr, value, mask);
      return;
    case 0xdfc:
    {
      auto const cycles = core_.read_cyccnt();
      scs.demcr         = merge(scs.demcr, value, mask);
      core_.rebase_cyccnt(cycles);
      return;
    }
    case 0xf00:
    {
      auto const number = exc_irq0 + ((value & mask) & 0x1ffu);
      if(number < core_.exception_count() and (bit(scs.ccr, 1) or core_.privileged()))
      {
        core_.set_exception_pending(number, true);
      }
      return;
    }
    case 0xf34:
      scs.fpccr = merge(scs.fpccr, value, mask);
      return;
    case 0xf38:
      scs.fpcar = merge(scs.fpcar, value, mask);
      return;
    case 0xf3c:
      scs.fpdscr = merge(scs.fpdscr, value, mask);
      return;
    default:
      return;
  }
}

void system_space::write_scs_byte(std::uint32_t offset, std::uint8_t value)
{
  auto&      exceptions = core_.state_.exceptions;
  auto const prio_mask =
      static_cast<std::uint8_t>((0xffu << (8u - core_.core_.nvic_prio_bits)) & 0xffu);
  if(in(offset, 0x400, 0x5ef))
  {
    auto const number = exc_irq0 + (offset - 0x400);
    if(number < core_.exception_count())
    {
      exceptions.priority[number] = value & prio_mask;
    }
    return;
  }
  if(in(offset, 0xd18, 0xd23))
  {
    auto const number = exc_memmanage + (offset - 0xd18);
    if(system_priority_implemented[number])
    {
      exceptions.priority[number] = value & prio_mask;
    }
    return;
  }
  auto& cfsr = core_.state_.scs.cfsr;
  cfsr &= ~(std::uint32_t{value} << (8 * (offset - 0xd28)));
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
