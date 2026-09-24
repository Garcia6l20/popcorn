#pragma once

#include <reflex/enum.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace popcorn
{
/** @brief Guest memory address. */
using address = std::uint32_t;

/** @brief Count of guest instructions executed. */
struct icount
{
  std::uint64_t value;
};

/** @brief Virtual time elapsed in the guest, in picoseconds. */
struct vtime
{
  std::uint64_t ps;
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

/** @brief Class of processor fault, arch-neutral (no security-state or FP-frame notions). */
enum class[[= reflex::derive{reflex::Format}]] fault_kind
{
  hard,
  mem_manage,
  bus,
  usage,
  unmapped,
  invalid,
};

/** @brief Program location: address plus symbol and source info when known. */
struct location
{
  address          pc;
  std::string_view symbol;
  std::uint32_t    offset;
  std::string_view file;
  std::uint32_t    line;
};

/** @brief Program location that owns its symbol and file text, safe to outlive the frame it was
 * built in. */
struct owned_location
{
  address       pc;
  std::string   symbol;
  std::uint32_t offset;
  std::string   file;
  std::uint32_t line;
};

/** @brief Copies a location into an owned_location. */
inline auto to_owned(location const& where) -> owned_location
{
  return owned_location{
      where.pc, std::string(where.symbol), where.offset, std::string(where.file), where.line};
}

/** @brief Decoded processor fault. */
struct fault_info
{
  fault_kind    kind;
  address       fault_address;
  std::uint32_t cfsr;
  std::uint32_t hfsr;
};

/** @brief Result of a run, step or wait. */
struct stop
{
  stop_reason               reason;
  location                  where;
  icount                    instructions;
  vtime                     time;
  std::optional<fault_info> fault;
};
}
