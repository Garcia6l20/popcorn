#pragma once

/**
 * @file
 * @brief Fundamental types of the C++ API: addresses, instruction and virtual time counts, stop and
 * fault records, and the enums that describe them.
 */

#include <reflex/enum.hpp>

#include <compare>
#include <cstdint>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace popcorn
{
/** @brief Guest memory address. */
using address = std::uint32_t;

/** @brief Count of guest instructions executed. */
struct icount
{
  /** @brief The count. */
  std::uint64_t value;

  /** @brief Member-wise ordering and equality. */
  friend auto operator<=>(icount const&, icount const&) = default;
};

/** @brief Virtual time elapsed in the guest, in picoseconds. */
struct vtime
{
  /** @brief Picoseconds. */
  std::uint64_t ps;

  /** @brief Member-wise ordering and equality. */
  friend auto operator<=>(vtime const&, vtime const&) = default;
};

/**
 * @brief Converts an instruction count run at a fixed clock into virtual time.
 * @param instructions Instructions executed.
 * @param clock_hz Clock frequency in Hz, must not be 0.
 * @return Elapsed virtual time, at 1e12 / @p clock_hz picoseconds per instruction, computed
 *         without intermediate truncation. Wraps past 2^64 ps, about 213 days of virtual time
 *         at 160 MHz.
 */
constexpr auto vtime_from_instructions(std::uint64_t instructions, std::uint64_t clock_hz) -> vtime
    pre(clock_hz != 0)
{
  constexpr unsigned __int128 ps_per_second = 1'000'000'000'000ull;
  unsigned __int128 const     ps            = ps_per_second * instructions / clock_hz;
  return vtime{static_cast<std::uint64_t>(ps)};
}

/**
 * @brief Converts virtual time into the instruction count that reaches it at a fixed clock, the
 *        inverse of vtime_from_instructions().
 * @param elapsed Virtual time elapsed.
 * @param clock_hz Clock frequency in Hz, must not be 0.
 * @return Instructions, rounded up so that the count reaches @p elapsed. The product
 *         `elapsed * clock_hz` is formed in 128 bits and cannot overflow. The result wraps past
 *         2^64 instructions, about 3650 years at 160 MHz.
 */
constexpr auto icount_from_vtime(vtime elapsed, std::uint64_t clock_hz) -> icount pre(clock_hz != 0)
{
  constexpr unsigned __int128 ps_per_second = 1'000'000'000'000ull;
  unsigned __int128 const     scaled        = static_cast<unsigned __int128>(elapsed.ps) * clock_hz;
  return icount{static_cast<std::uint64_t>((scaled + ps_per_second - 1) / ps_per_second)};
}

/** @brief Why a run stopped. */
enum class[[= reflex::derive{reflex::Format}]] stop_reason
{
  returned,
  address,
  budget,
  time,
  idle,
  fault,
  reset_request,
  spin,
  watchpoint,
  user,
  contract,
};

/** @brief Class of processor fault, arch-neutral but for secure: an Arm SecureFault, raised only
 *         by a core with TrustZone on. */
enum class[[= reflex::derive{reflex::Format}]] fault_kind
{
  hard,
  mem_manage,
  bus,
  usage,
  unmapped,
  invalid,
  secure,
};

/** @brief Bus master that initiated a memory or register access. */
enum class[[= reflex::derive{reflex::Format}]] initiator
{
  cpu,
  dma,
  debugger,
};

/**
 * @brief Attributes of one bus transaction: the master that made it, its security attribute,
 *        AHB HNONSEC inverted, and its privilege, AHB HPROT[1]. The defaults are what every
 *        access means when the master names no attribute: Secure and privileged.
 */
struct bus_attrs
{
  /** @brief Bus master that made the access. */
  initiator by{initiator::cpu};
  /** @brief Whether the transaction is Secure. */
  bool secure{true};
  /** @brief Whether the transaction is privileged. */
  bool privileged{true};
  /** @brief For an initiator::debugger access: whether the bus firewall filters it as a DAP
   * access with these attributes. Clear, a debugger access is never filtered. Ignored for the
   * other initiators, which are always filtered. */
  bool filtered{};

  /** @brief Member-wise equality. */
  friend auto operator==(bus_attrs const&, bus_attrs const&) -> bool = default;
};

/**
 * @brief Cause of a reset. Backup-domain state, such as an RTC and its backup registers,
 *        survives every kind except power_on.
 */
enum class[[= reflex::derive{reflex::Format}]] reset_kind
{
  power_on,
  brown_out,
  pin,
  software,
  watchdog,
  window_watchdog,
  low_power,
  option_load,
};

/** @brief Why a guest bus access failed. */
enum class[[= reflex::derive{reflex::Format}]] bus_error
{
  unmapped,
  denied,
};

/**
 * @brief Whether a value of an enumeration equals one of its enumerators, by reflection.
 * @param value Value to check, possibly cast from an out-of-range integer.
 */
template <typename E>
  requires std::is_enum_v<E>
constexpr auto is_enumerator(E value) noexcept -> bool
{
  template for(constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E)))
  {
    if(value == [:e:])
    {
      return true;
    }
  }
  return false;
}

/**
 * @brief Program location: address plus symbol and source info when known. The string views
 *        point into the machine's loaded images and stay valid until the next image load. Use
 *        owned_location to keep one longer.
 */
struct location
{
  /** @brief Address of the instruction. */
  address pc{};
  /** @brief Name of the function or object containing pc, empty when unknown. */
  std::string_view symbol{};
  /** @brief Byte offset of pc from the start of symbol, 0 when symbol is empty. */
  std::uint32_t offset{};
  /** @brief Source file of pc from the line table, empty when unknown. */
  std::string_view file{};
  /** @brief Source line of pc, 0 when unknown. */
  std::uint32_t line{};
};

/** @brief Program location that owns its symbol and file text, safe to outlive the frame it was
 * built in. */
struct owned_location
{
  /** @brief Address of the instruction. */
  address pc{};
  /** @brief Name of the function or object containing pc, empty when unknown. */
  std::string symbol{};
  /** @brief Byte offset of pc from the start of symbol, 0 when symbol is empty. */
  std::uint32_t offset{};
  /** @brief Source file of pc from the line table, empty when unknown. */
  std::string file{};
  /** @brief Source line of pc, 0 when unknown. */
  std::uint32_t line{};
};

/** @brief Copies a location into an owned_location. */
inline auto to_owned(location const& where) -> owned_location
{
  return owned_location{
      where.pc, std::string(where.symbol), where.offset, std::string(where.file), where.line};
}

/**
 * @brief Decoded processor fault. status and detail are the architecture's raw fault registers:
 *        Arm CFSR and HFSR, RISC-V mcause and mtval.
 */
struct fault_info
{
  /** @brief Class of the fault. */
  fault_kind kind{};
  /** @brief Faulting data or instruction address, 0 when the fault has none. */
  address fault_address{};
  /** @brief Primary fault status register: Arm CFSR, SFSR for a SecureFault, RISC-V mcause. */
  std::uint32_t status{};
  /** @brief Secondary fault register: Arm HFSR, RISC-V mtval. */
  std::uint32_t detail{};
};

/** @brief Named range of guest addresses, for scoping coverage and trace collection. */
struct address_range
{
  /** @brief Name for reports, such as a function or section name. */
  std::string name{};
  /** @brief First address. */
  address start{};
  /** @brief Size in bytes. */
  std::uint32_t size{};

  /** @brief Member-wise equality. */
  friend auto operator==(address_range const&, address_range const&) -> bool = default;
};

/** @brief Result of a run, step or wait. */
struct stop
{
  /** @brief Why the run stopped. */
  stop_reason reason{};
  /** @brief Where the core stopped, the next instruction to execute. */
  location where{};
  /** @brief Instructions executed since reset. */
  icount instructions{};
  /** @brief Virtual time since reset. */
  vtime time{};
  /** @brief machine::resets() at the stop, which names the reset @ref time counts from. */
  std::uint64_t resets{};
  /** @brief Decoded fault, set exactly when reason is stop_reason::fault. */
  std::optional<fault_info> fault{};
  /** @brief What caused the stop, beyond the reason: the text of a stop request, the loop and
   * polled register of a spin, the access of a watch hit. Empty when the reason says it all. */
  std::string detail{};
};
}
