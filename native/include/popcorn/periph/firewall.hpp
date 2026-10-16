#pragma once

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>

#include <cstddef>
#include <cstdint>

namespace popcorn::periph
{
/**
 * @brief What a bus firewall decides for one access to a peripheral window. `allow` lets the
 *        access reach the model or the register layer. `read_zero_write_ignore` makes a read
 *        return 0 and drops a write: neither reaches the model nor the register layer, the trace
 *        and the watches still see the access, with 0 for a read and the written value for a
 *        write. `bus_error` fails the access with bus_error::denied, a BusFault for a CPU access
 *        and a transfer error for a DMA access.
 */
enum class[[= reflex::derive{reflex::Format}]] bus_verdict : std::uint8_t
{
  allow,
  read_zero_write_ignore,
  bus_error,
};

/**
 * @brief Security and privilege filter in front of every peripheral window of a machine, as an
 *        SoC bus firewall such as the STM32 GTZC TZSC. At most one per machine, installed by a
 *        model through model_context::install_firewall while the machine builds its models. It
 *        is called on the machine thread for every CPU, DMA and debugger access to a peripheral
 *        window, before the model and the register layer, so it must not take the GIL.
 */
struct POPCORN_API bus_firewall
{
  /** @brief Creates the firewall. */
  bus_firewall() = default;
  /** @brief Not copyable, the machine holds its address. */
  bus_firewall(bus_firewall const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(bus_firewall const&) -> bus_firewall& = delete;
  /** @brief Not movable. */
  bus_firewall(bus_firewall&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(bus_firewall&&) -> bus_firewall& = delete;
  /** @brief Destroys the firewall. */
  virtual ~bus_firewall() = default;

  /**
   * @brief Decides one access to a peripheral window.
   * @param unit Index in part_description::peripherals of the peripheral the access reaches:
   *        the aliased peripheral for an access through a `SEC_` alias window.
   * @param attrs Attributes of the access. `secure` is false for every access on a machine
   *        without TrustZone, as access_ctx::secure.
   * @param op arch::memory_op::read or arch::memory_op::write, never arch::memory_op::fetch: no
   *        fetch reaches a peripheral window.
   * @return The verdict.
   */
  [[nodiscard]] virtual auto check_peripheral(std::size_t unit, bus_attrs attrs, arch::memory_op op)
      -> bus_verdict = 0;
};
}
