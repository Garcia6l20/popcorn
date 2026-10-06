#pragma once

#include <cstdint>

namespace popcorn::arch::armv8m
{
/** @brief Bytes of a basic exception frame: r0-r3, r12, lr, return address, xPSR. */
inline constexpr std::uint32_t basic_frame_size = 0x20;
/** @brief Bytes of an extended frame: the basic words, s0-s15, FPSCR and a reserved word. */
inline constexpr std::uint32_t extended_frame_size = 0x68;
/** @brief xPSR bit 9 of a stacked xPSR: the frame was aligned down by 4 bytes. */
inline constexpr std::uint32_t stacked_align_bit = 1u << 9;
/** @brief xPSR bit 20 of a stacked xPSR on a Secure core: CONTROL.SFPA at entry. */
inline constexpr std::uint32_t stacked_sfpa_bit = 1u << 20;

/** @brief What an EXC_RETURN value selects. */
struct exc_return
{
  /** @brief Return to Thread mode, Mode bit 3. */
  bool thread{};
  /** @brief Pop from PSP, SPSEL bit 2. */
  bool process_stack{};
  /** @brief Basic frame, FType bit 4. */
  bool basic_frame{};

  /** @brief Member-wise equality. */
  friend constexpr auto operator==(exc_return, exc_return) -> bool = default;
};

/**
 * @brief EXC_RETURN value of an entry.
 * @param frame What the return selects.
 *        Bits 6 and 5 are set on every profile: S and DCRS on the Secure M33 engine, ones on
 *        armv7m. ES, bit 0, is set.
 */
constexpr auto encode_exc_return(exc_return frame) noexcept -> std::uint32_t
{
  return 0xffff'ff80u
       | 1u << 6
       | 1u << 5
       | (frame.basic_frame ? 1u << 4 : 0u)
       | (frame.thread ? 1u << 3 : 0u)
       | (frame.process_stack ? 1u << 2 : 0u)
       | 1u;
}

/** @brief Whether @p value is an EXC_RETURN value the core can return through. */
constexpr auto valid_exc_return(std::uint32_t value) noexcept -> bool
{
  auto const handler_psp = (value & (1u << 3)) == 0 and (value & (1u << 2)) != 0;
  return (value & 0xff00'0000u) == 0xff00'0000u and not handler_psp;
}

/**
 * @brief Whether an EXC_RETURN value is one of the six ARMv7-M defines: Handler MSP, Thread MSP,
 *        Thread PSP, each with a basic or an extended frame.
 */
constexpr auto valid_v7_exc_return(std::uint32_t value) noexcept -> bool
{
  switch(value)
  {
    case 0xFFFF'FFE1:
    case 0xFFFF'FFE9:
    case 0xFFFF'FFED:
    case 0xFFFF'FFF1:
    case 0xFFFF'FFF9:
    case 0xFFFF'FFFD:
      return true;
    default:
      return false;
  }
}

/**
 * @brief What an EXC_RETURN value selects. S, DCRS and ES are ignored: the M33 engine runs
 *        Secure, so firmware built for Non-secure values returns.
 */
constexpr auto decode_exc_return(std::uint32_t value) noexcept -> exc_return
{
  return {
      .thread        = (value & (1u << 3)) != 0,
      .process_stack = (value & (1u << 3)) != 0 and (value & (1u << 2)) != 0,
      .basic_frame   = (value & (1u << 4)) != 0,
  };
}
}
