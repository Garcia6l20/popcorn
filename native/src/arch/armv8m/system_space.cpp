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
constexpr std::uint32_t ccr_writable  = (1u << 1) | (1u << 3) | (1u << 4) | (1u << 8) | (1u << 10);
constexpr std::uint32_t scr_writable  = (1u << 1) | (1u << 2) | (1u << 4);
constexpr std::uint32_t vtor_writable = 0xffff'ff80;
constexpr std::uint32_t syst_csr_writable = 0x7;
constexpr std::uint32_t syst_rvr_writable = 0x00ff'ffff;
constexpr std::uint32_t syst_calib        = 0x4000'0000;
constexpr std::uint32_t cpacr_writable    = 0x00f0'0000;
constexpr std::uint32_t dwt_numcomp       = 4u << 28;
constexpr std::uint32_t shcsr_enables     = 0x0007'0000;

constexpr std::uint32_t exc_memmanage  = 4;
constexpr std::uint32_t exc_busfault   = 5;
constexpr std::uint32_t exc_usagefault = 6;
constexpr std::uint32_t exc_svcall     = 11;
constexpr std::uint32_t exc_debugmon   = 12;
constexpr std::uint32_t exc_pendsv     = 14;
constexpr std::uint32_t exc_systick    = 15;

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

constexpr std::array<std::uint32_t, 16> m33_id_registers{
    0x0000'0030, 0x0000'0210, 0x0020'0000, 0x0000'0000, 0x0010'1f40, 0x0000'0000,
    0x0100'0000, 0x0000'0000, 0x0110'1110, 0x0221'2000, 0x2023'2232, 0x0111'1131,
    0x0131'0132, 0x0000'0000, 0x0000'0000, 0x8000'c000};

constexpr std::array<std::uint32_t, 3> fpv5_sp_media_features{
    0x1011'0021, 0x1100'0011, 0x0000'0040};

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
        return scs.dwt_cyccnt;
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
    return m33_id_registers[(offset - 0xd40) / 4];
  }
  if(in(offset, 0xf40, 0xf48))
  {
    return core_.core_.fpu ? fpv5_sp_media_features[(offset - 0xf40) / 4] : 0;
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
      return scs.syst_csr;
    case 0x014:
      return scs.syst_rvr;
    case 0x018:
      return scs.syst_cvr;
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
      return scs.vtor;
    case 0xd0c:
      return aircr_key_stat << 16 | scs.prigroup << 8;
    case 0xd10:
      return scs.scr;
    case 0xd14:
      return scs.ccr | ccr_read_one;
    case 0xd24:
    {
      std::uint32_t shcsr = scs.shcsr_enables;
      for(auto const& entry : shcsr_active)
      {
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
      return scs.cpacr;
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
      return scs.fpccr;
    case 0xf38:
      return scs.fpcar;
    case 0xf3c:
      return scs.fpdscr;
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
        scs.dwt_ctrl = merge(scs.dwt_ctrl, value, mask & 1u);
      }
      else if(offset == 0x004)
      {
        scs.dwt_cyccnt = merge(scs.dwt_cyccnt, value, mask);
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
      scs.syst_csr = merge(scs.syst_csr, value, mask & syst_csr_writable);
      return;
    case 0x014:
      scs.syst_rvr = merge(scs.syst_rvr, value, mask & syst_rvr_writable);
      return;
    case 0x018:
      scs.syst_cvr = merge(scs.syst_cvr, value, mask);
      return;
    case 0xd04:
    {
      auto const bits = value & mask;
      if(bit(bits, 31))
      {
        core_.set_exception_pending(exc_nmi, true);
      }
      if(bit(bits, 30))
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
      scs.vtor = merge(scs.vtor, value, mask & vtor_writable);
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
      scs.ccr = merge(scs.ccr, value, mask & ccr_writable);
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
      scs.cpacr = merge(scs.cpacr, value, mask & cpacr_writable);
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
      scs.demcr = merge(scs.demcr, value, mask);
      return;
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
}
