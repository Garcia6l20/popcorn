#pragma once

#include "gtzc.hpp"
#include "model_base.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace popcorn::periph::stm32
{
/** @brief Bits of a configuration register that are Secure while another peripheral is. */
struct owned_bits
{
  /** @brief Bits within the register word. */
  std::uint32_t mask{};
  /** @brief Index of the owner in part_description::peripherals, empty when the part has none. */
  std::optional<std::size_t> unit{};
};

/**
 * @brief The register rules the STM32U5 SYSCFG, RM0456 15.2.2 Table 131, and the STM32H5 SBS,
 *        RM0481 14.3.4 and 14.5, share under TrustZone. The Secure lock register is Secure and
 *        privileged, the FPU interrupt mask privileged and Secure with FPUSEC set, the
 *        Non-secure lock register privileged, the Class B register Secure with CLASSBSEC set,
 *        the memory erase and compensation cell group Secure with SYSCFGSEC or SBSSEC set. A
 *        Non-secure access to a Secure one is RAZ/WI and raises the unit's illegal access
 *        event, an unprivileged access to a privileged one is RAZ/WI with no event. In the
 *        pin configuration register, when set, such as `SYSCFG_CFGR1`, the Fast-mode Plus bit of
 *        PB6 to PB9 is Secure while that pin is Secure in GPIOB, and each owned group is
 *        Secure while its owner is, RAZ/WI to a Non-secure access with no event.
 */
struct secure_config_rules
{
  /** @brief SYSCFGSEC or SBSSEC of the security register. */
  field_ref system{};
  /** @brief CLASSBSEC of the security register. */
  field_ref class_b{};
  /** @brief FPUSEC of the security register. */
  field_ref fpu{};
  /** @brief Offset of the Secure lock register, `SYSCFG_CSLCKR` or `SBS_CSLCKR`. */
  std::uint32_t secure_lock{};
  /** @brief Offset of the Non-secure lock register. */
  std::uint32_t nonsecure_lock{};
  /** @brief Offset of the FPU interrupt mask register. */
  std::uint32_t fpu_mask{};
  /** @brief Offset of the Class B register. */
  std::uint32_t config2{};
  /** @brief Offsets of the memory erase status and compensation cell registers. */
  std::array<std::uint32_t, 4> compensation{};
  /** @brief Offset of the pin configuration register, empty when the unit keeps it as storage. */
  std::optional<std::uint32_t> pin_config{};
  /** @brief Fast-mode Plus bits of PB6 to PB9 in the pin configuration register. */
  std::array<field_ref, 4> fast_mode{};
  /** @brief Groups of the pin configuration register that follow another peripheral. */
  std::array<owned_bits, 2> owned{};
  /** @brief Index of the guarded unit, the source of its illegal access event. */
  std::optional<std::size_t> unit{};
  /** @brief The unit's TZIC line. */
  illegal_event illegal{};

  /**
   * @brief Guard of a register refused as a whole, raising the illegal access event when a
   *        Non-secure transaction reaches a Secure register.
   * @param secure Whether the register is Secure.
   * @param privileged Whether the register is privileged.
   * @param ctx Attributes of the transaction.
   * @param peers Models of the machine.
   */
  [[nodiscard]] auto
      refused(bool secure, bool privileged, access_ctx const& ctx, peer_directory const& peers)
          -> bus_guard
  {
    if(secure and not ctx.secure)
    {
      if(unit)
      {
        illegal.raise(peers, *unit, ctx.by);
      }
      return bus_guard::both(~std::uint32_t{});
    }
    return guard_of(~std::uint32_t{}, false, privileged, ctx);
  }

  /**
   * @brief Guard of one register word under the shared rules.
   * @param word Offset of the word.
   * @param security Security register as a Secure read sees it.
   * @param ctx Attributes of the transaction.
   * @param peers Models of the machine.
   * @param part Description.
   * @return The guard, empty for a word the rules do not cover.
   */
  [[nodiscard]] auto guard(
      std::uint32_t           word,
      std::uint32_t           security,
      access_ctx const&       ctx,
      peer_directory const&   peers,
      part_description const& part) -> std::optional<bus_guard>
  {
    if(word == secure_lock)
    {
      return refused(true, true, ctx, peers);
    }
    if(word == fpu_mask)
    {
      return refused(fpu.of(security) != 0, true, ctx, peers);
    }
    if(word == nonsecure_lock)
    {
      return refused(false, true, ctx, peers);
    }
    if(word == config2)
    {
      return refused(class_b.of(security) != 0, false, ctx, peers);
    }
    if(std::ranges::contains(compensation, word))
    {
      return refused(system.of(security) != 0, false, ctx, peers);
    }
    if(not pin_config or word != *pin_config)
    {
      return std::nullopt;
    }
    if(ctx.secure)
    {
      return bus_guard{};
    }
    std::uint32_t hidden = 0;
    auto const*   port   = dynamic_cast<trustzone_aware const*>(peers.find("GPIOB"));
    auto const    pins   = port != nullptr ? port->secure_word() : 0;
    for(std::size_t at = 0; at < fast_mode.size(); ++at)
    {
      if((pins >> (at + 6) & 1u) != 0)
      {
        hidden |= fast_mode[at].mask();
      }
    }
    for(auto const& [mask, owner] : owned)
    {
      if(owner and unit_secure(peers, part, *owner))
      {
        hidden |= mask;
      }
    }
    return bus_guard::both(hidden);
  }
};

/**
 * @brief The diagnostics of a configuration unit's storage registers, `SYSCFG` or `SBS`: a
 *        firmware write that lands on one logs one model_warning per register, and raises
 *        model_error under strict_peripherals before it is stored. Debugger writes and writes
 *        the bus rules refuse whole are not noted.
 */
struct storage_notes
{
  /** @brief Words already warned about. */
  std::vector<std::uint32_t> logged{};

  /**
   * @brief Notes a write of a storage register.
   * @param ctx Context of the unit's model.
   * @param desc Description of the unit.
   * @param unit Unit name in the warning, `SYSCFG` or `SBS`.
   * @param word Offset of the word.
   * @param access Attributes of the write.
   * @param applied Guard of the write.
   * @throws model_error under strict_peripherals.
   */
  void written(
      model_context&         ctx,
      peripheral_desc const& desc,
      std::string_view       unit,
      std::uint32_t          word,
      access_ctx const&      access,
      bus_guard const&       applied)
  {
    if(access.by == initiator::debugger or applied.wi == ~std::uint32_t{})
    {
      return;
    }
    auto const found = std::ranges::find_if(desc.registers, [word](register_desc const& r) {
      return not r.alternate_of and (r.offset & ~3u) == word;
    });
    auto const name  = found != desc.registers.end() ? found->name : std::string{};
    if(not std::ranges::contains(logged, word))
    {
      logged.push_back(word);
      ctx.services.log(
          {.kind       = core::log_kind::model_warning,
           .peripheral = desc.name,
           .reg        = name,
           .message    = std::format(
               "{} is storage: the {} functions it selects are not modelled", name, unit)});
    }
    if(ctx.strict_peripherals)
    {
      throw model_error(desc.name, name, word);
    }
  }
};
}
