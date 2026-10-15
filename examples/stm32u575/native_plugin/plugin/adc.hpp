#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/periph/spi.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace native_plugin
{
/**
 * @brief The two-channel 12-bit SPI ADC of the spi_mock example's `spiadc.Adc`, in C++, with
 *        its default ramp. A conversion is one chip select period of two 16-bit frames: a
 *        command with the start bit 15 and the channel in bit 12, answered 0, then the sample.
 *        Channel 0 is a ramp of `counts_per_ms` counts per millisecond of virtual time, channel
 *        1 its mirror, full scale minus the ramp.
 */
class adc : public popcorn::periph::spi_target
{
public:
  /** @brief Counters of the framing the firmware used. */
  struct framing
  {
    /** @brief Chip select falling edges. */
    std::uint32_t selects;
    /** @brief Chip select rising edges. */
    std::uint32_t deselects;
    /** @brief Frames of another size than 16 bits, and selections without exactly two frames. */
    std::uint32_t bad_frames;
  };

  /**
   * @brief An ADC whose ramp rises @p counts_per_ms counts per millisecond.
   * @param counts_per_ms Slope of the ramp.
   */
  explicit adc(std::uint32_t counts_per_ms);

  void select(bool selected, popcorn::vtime at) override;
  auto exchange(std::uint32_t out, std::uint8_t bits, popcorn::vtime at) -> std::uint32_t override;
  void reset(popcorn::reset_kind kind) override;
  void save(std::vector<std::byte>& out) const override;
  void load(std::span<std::byte const> in) override;

  /** @brief The framing counters so far. */
  [[nodiscard]] auto counted() const noexcept -> framing;

  /** @brief The slope of the ramp. */
  [[nodiscard]] auto counts_per_ms() const noexcept -> std::uint32_t;

private:
  struct state
  {
    framing       counts;
    std::uint32_t frame;
    std::uint32_t channel;
  };

  std::uint32_t counts_per_ms_;
  state         state_{};
};
}
