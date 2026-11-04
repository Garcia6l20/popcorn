#pragma once

#include <popcorn/export.hpp>

#include <cstdint>

namespace popcorn::periph::stm32
{
/** @brief Channel bank of an ADC input, ADC_CR2.ADC_CFG on the STM32L1 Cat.3 to Cat.6. */
enum class analog_bank : std::uint8_t
{
  a,
  b,
};

/**
 * @brief An ADC external input: channel @p number of bank @p bank, `ADC_IN12b` for 12 of bank B.
 *        An integer converts to the bank A channel of that number.
 */
struct analog_channel
{
  /**
   * @brief Names a channel.
   * @param number Channel number, 0 to 31.
   * @param bank Channel bank, bank A on every ADC with one bank.
   */
  constexpr analog_channel(std::uint8_t number, analog_bank bank = analog_bank::a) noexcept
      : number{number}, bank{bank}
  {}

  /** @brief Channel number, 0 to 31. */
  std::uint8_t number;
  /** @brief Channel bank. */
  analog_bank bank;

  /** @brief Member-wise equality. */
  friend auto operator==(analog_channel, analog_channel) -> bool = default;
};

/**
 * @brief The analog inputs of an ADC as a board sees them, implemented by the ADC model of each
 *        ADC instance, ADC1 on the STM32U5, ADC1 and ADC2 on the STM32H5, ADC on the STM32L1.
 *        Reach it with `dynamic_cast<analog_port*>(machine.model("ADC1"))`. Each input is one
 *        pad: bank B holds the channels with a pad of their own, the STM32L1 Cat.3 to Cat.6
 *        ADC_IN0b to ADC_IN3b and ADC_IN6b to ADC_IN12b, and is empty on every other ADC. A
 *        bank B channel sharing its pad with bank A is driven as that bank A channel.
 */
class POPCORN_API analog_port
{
public:
  /** @brief Creates the interface. */
  analog_port() = default;
  /** @brief Not copyable. */
  analog_port(analog_port const&) = delete;
  /** @brief Not copy-assignable. */
  auto operator=(analog_port const&) -> analog_port& = delete;
  /** @brief Not movable. */
  analog_port(analog_port&&) = delete;
  /** @brief Not move-assignable. */
  auto operator=(analog_port&&) -> analog_port& = delete;

  /**
   * @brief Drives an external channel. The drive is board state: it survives an MCU reset and a
   *        snapshot restores it. A conversion reads the value in force when it ends. The drive
   *        is the board analog slot of the channel's pad, gpio_port::drive_analog(), so every
   *        ADC channel on that pad converts it, and a conversion reads the pad's
   *        gpio_port::analog_level().
   * @param channel External channel, one of external_channels() of its bank.
   * @param microvolts Input voltage. Above VDDA the code saturates.
   * @throws config_error for an internal or absent channel, a bank B channel that shares its
   *         pad with bank A, or a channel without its pad in the description's
   *         `channel_pins`.
   */
  virtual void drive(analog_channel channel, std::uint32_t microvolts) = 0;

  /**
   * @brief The voltage driven on an external channel in microvolts, 0 when never driven: the
   *        board analog slot of its pad, whichever ADC drove it.
   * @param channel External channel, one of external_channels() of its bank.
   * @throws config_error for an internal or absent channel, a bank B channel that shares its
   *         pad with bank A, or a channel without its pad in the description's
   *         `channel_pins`.
   */
  [[nodiscard]] virtual auto input(analog_channel channel) const -> std::uint32_t = 0;

  /**
   * @brief The external channels of a bank: bit n is set when channel n of @p bank takes a drive.
   *        Fixed for the life of the model, so any thread may read it.
   * @param bank Channel bank.
   */
  [[nodiscard]] virtual auto external_channels(analog_bank bank) const -> std::uint32_t = 0;

  /**
   * @brief The external channels of a bank whose pad reaches a package pin, bit n for channel n:
   *        the description's `bonded_channels` for bank A and `bonded_channels_b` for bank B,
   *        else every external channel of the bank. The board API refuses the others,
   *        conversions read every external channel. Fixed for the life of the model, so any
   *        thread may read it.
   * @param bank Channel bank.
   */
  [[nodiscard]] virtual auto bonded_channels(analog_bank bank) const -> std::uint32_t = 0;

  /** @brief VDDA in millivolts, the reference of every conversion. */
  [[nodiscard]] virtual auto vdda_mv() const -> std::uint32_t = 0;

protected:
  /** @brief Destroys the interface, only through its model. */
  virtual ~analog_port();
};
}
