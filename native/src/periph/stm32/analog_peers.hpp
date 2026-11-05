#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace popcorn::periph::stm32
{
/** @brief Model whose outputs reach ADC channels inside the chip, the STM32L4 DAC1. */
class internal_analog_source
{
public:
  /** @brief Creates the interface. */
  internal_analog_source() = default;
  /** @brief Not copyable. */
  internal_analog_source(internal_analog_source const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(internal_analog_source const&) -> internal_analog_source& = delete;
  /** @brief Not movable. */
  internal_analog_source(internal_analog_source&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(internal_analog_source&&) -> internal_analog_source& = delete;

  /**
   * @brief Level an ADC channel connected to @p output converts now.
   * @param output Output index, 0 for the first.
   * @return The level in microvolts, empty while the output reaches no on-chip peripheral.
   */
  [[nodiscard]] virtual auto internal_level(std::size_t output) const
      -> std::optional<std::uint32_t> = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~internal_analog_source() = default;
};

/** @brief Model with ADC channels on an internal_analog_source output. */
class internal_analog_listener
{
public:
  /** @brief Creates the interface. */
  internal_analog_listener() = default;
  /** @brief Not copyable. */
  internal_analog_listener(internal_analog_listener const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(internal_analog_listener const&) -> internal_analog_listener& = delete;
  /** @brief Not movable. */
  internal_analog_listener(internal_analog_listener&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(internal_analog_listener&&) -> internal_analog_listener& = delete;

  /**
   * @brief Whether the model converts @p output of @p source on any channel. Fixed for the
   *        model's life, so a source keeps only the listeners that answer true.
   * @param source The source.
   * @param output Output index.
   */
  [[nodiscard]] virtual auto
      reads_internal(internal_analog_source const& source, std::size_t output) const -> bool = 0;

  /**
   * @brief The level of @p output of @p source is about to change. A converting ADC catches up
   *        with the old level first. Never calls back into the source.
   * @param source The source.
   * @param output Output index.
   */
  virtual void internal_changing(internal_analog_source const& source, std::size_t output) = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  ~internal_analog_listener() = default;
};
}
