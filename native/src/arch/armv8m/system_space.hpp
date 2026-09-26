#pragma once

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
};

/** @brief Base address of each window, in window order. */
inline constexpr std::array<address, 4> window_bases{
    0xE000'0000, 0xE000'1000, 0xE000'2000, 0xE000'E000};

/** @brief Size of each window. */
inline constexpr std::uint32_t window_size = 0x1000;

/**
 * @brief Register model of the ITM, DWT, FPB and SCS windows over the module's state. Accesses
 *        touch only the state and the machine services, so a callback may re-enter.
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
  [[nodiscard]] auto read_word(window which, std::uint32_t offset) const -> std::uint32_t;
  [[nodiscard]] auto read_scs(std::uint32_t offset) const -> std::uint32_t;
  void write_word(window which, std::uint32_t offset, std::uint32_t value, std::uint32_t mask);
  void write_scs(std::uint32_t offset, std::uint32_t value, std::uint32_t mask);
  void write_scs_byte(std::uint32_t offset, std::uint8_t value);
  void write_pmsav7(std::uint32_t offset, std::uint32_t value, std::uint32_t mask);
  [[nodiscard]] auto priority_byte(std::uint32_t offset) const -> std::uint8_t;

  cortex_m& core_;
};
}
