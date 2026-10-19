#pragma once

#include "state.hpp"

#include <popcorn/core/types.hpp>

#include <array>
#include <cstdint>

namespace popcorn::arch::armv8m
{
class cortex_m;

/** @brief Window of the core's system space, each 4 KB. */
enum class window
{
  itm,
  dwt,
  fpb,
  scs,
  scs_nonsecure,
};

/** @brief Base address of the ITM, DWT, FPB and SCS windows, in window order. */
inline constexpr std::array<address, 4> window_bases{
    0xE000'0000, 0xE000'1000, 0xE000'2000, 0xE000'E000};

/** @brief Base address of SCS_NS, the Non-secure alias of the SCS, mapped with TrustZone only. */
inline constexpr address scs_nonsecure_base = 0xE002'E000;

/** @brief Size of each window. */
inline constexpr std::uint32_t window_size = 0x1000;

/**
 * @brief Register model of the ITM, DWT, FPB and SCS windows over the module's state. Accesses
 *        touch only the state and the machine services, so a callback may re-enter. With
 *        TrustZone an SCS access sees the bank of its security attribute, the PPB being exempt
 *        from attribution, B10.2. SCS_NS shows the Non-secure bank to a Secure access and is
 *        RAZ/WI to a Non-secure one, D1.1. A host access carries Secure attributes, so it
 *        reaches SCS_NS, which the architecture makes RES0 to a debugger.
 */
class system_space
{
public:
  /** @brief Creates the model over @p core, which it keeps. */
  explicit system_space(cortex_m& core) noexcept : core_{core}
  {}

  /**
   * @brief Reads @p size bytes at @p offset of a window, right-aligned.
   * @throws emulation_error when a service call fails.
   */
  [[nodiscard]] auto read(window which, std::uint32_t offset, unsigned size) const -> std::uint32_t;

  /**
   * @brief Writes @p size bytes at @p offset of a window, right-aligned in @p value.
   * @throws emulation_error when a service call fails.
   */
  void write(window which, std::uint32_t offset, unsigned size, std::uint32_t value);

private:
  [[nodiscard]] auto read_word(window which, std::uint32_t offset, security view) const
      -> std::uint32_t;
  [[nodiscard]] auto read_scs(std::uint32_t offset, security view) const -> std::uint32_t;
  [[nodiscard]] auto read_icsr(security view) const -> std::uint32_t;
  [[nodiscard]] auto read_shcsr(security view) const -> std::uint32_t;
  void               write_word(
      window        which,
      std::uint32_t offset,
      std::uint32_t value,
      std::uint32_t mask,
      security      view);
  void write_scs(std::uint32_t offset, std::uint32_t value, std::uint32_t mask, security view);
  void write_icsr(std::uint32_t bits, security view);
  void write_shcsr(std::uint32_t value, std::uint32_t mask, security view);
  void write_sau(std::uint32_t offset, std::uint32_t value, std::uint32_t mask);
  void write_scs_byte(std::uint32_t offset, std::uint8_t value, security view);
  void write_pmsav7(std::uint32_t offset, std::uint32_t value, std::uint32_t mask);
  [[nodiscard]] auto priority_byte(std::uint32_t offset, security view) const -> std::uint8_t;
  [[nodiscard]] auto priority_slot(std::uint32_t number, security view) const -> std::uint8_t*;
  [[nodiscard]] auto irq_visible(std::uint32_t number, security view) const noexcept -> bool;

  cortex_m& core_;
};
}
