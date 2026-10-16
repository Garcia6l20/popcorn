#pragma once

#include <cstdint>

namespace popcorn::arch::armv8m
{
/** @brief Bytes of a basic exception frame: r0-r3, r12, lr, return address, xPSR. */
inline constexpr std::uint32_t basic_frame_size = 0x20;
/** @brief Bytes of an extended frame: the basic words, s0-s15, FPSCR and a reserved word. */
inline constexpr std::uint32_t extended_frame_size = 0x68;
/** @brief Bytes of the additional state context: the integrity signature, a reserved word and
 *         r4-r11, B3.19. */
inline constexpr std::uint32_t callee_frame_size = 0x28;
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

/** @brief The security fields of an EXC_RETURN value, B3.19. The defaults are the values of a
 *         Secure exception taken from Secure state, the only ones without TrustZone. */
struct exc_return_security
{
  /** @brief The background state was Secure, S bit 6. */
  bool secure_background{true};
  /** @brief The default rules for the callee registers were followed, DCRS bit 5. */
  bool default_callee{true};
  /** @brief The exception is Secure, ES bit 0. */
  bool secure_exception{true};

  /** @brief Member-wise equality. */
  friend constexpr auto operator==(exc_return_security, exc_return_security) -> bool = default;
};

/**
 * @brief EXC_RETURN value of an entry.
 * @param frame What the return selects.
 * @param security S, DCRS and ES. The defaults set all three, as the Secure M33 engine without
 *        TrustZone and armv7m, whose bits 6, 5 and 0 read as ones, need.
 */
constexpr auto encode_exc_return(exc_return frame, exc_return_security security = {}) noexcept
    -> std::uint32_t
{
  return 0xffff'ff80u
       | (security.secure_background ? 1u << 6 : 0u)
       | (security.default_callee ? 1u << 5 : 0u)
       | (frame.basic_frame ? 1u << 4 : 0u)
       | (frame.thread ? 1u << 3 : 0u)
       | (frame.process_stack ? 1u << 2 : 0u)
       | (security.secure_exception ? 1u : 0u);
}

/** @brief The S, DCRS and ES fields of an EXC_RETURN value. */
constexpr auto decode_exc_security(std::uint32_t value) noexcept -> exc_return_security
{
  return {
      .secure_background = (value & (1u << 6)) != 0,
      .default_callee    = (value & (1u << 5)) != 0,
      .secure_exception  = (value & 1u) != 0,
  };
}

/**
 * @brief Integrity signature at the base of the additional state context, PushCalleeStack():
 *        0xFEFA125A with the FType bit as bit 0 on a core with the FP extension, 0xFEFA125B
 *        without one.
 */
constexpr auto integrity_signature(bool fpu, bool basic_frame) noexcept -> std::uint32_t
{
  return not fpu or basic_frame ? 0xFEFA'125Bu : 0xFEFA'125Au;
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
 * @brief What an EXC_RETURN value selects. S, DCRS and ES are left to decode_exc_security(): the
 *        M33 engine runs Secure without TrustZone, so firmware built for Non-secure values
 *        returns there.
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
