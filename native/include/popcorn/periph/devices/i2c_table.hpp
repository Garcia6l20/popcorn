#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/periph/stm32/i2c.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace popcorn::periph::devices
{
/** @brief One write transfer a table target received: the register, then the data bytes. */
struct table_write
{
  /** @brief Register address the transfer started with. */
  std::uint32_t reg{};
  /** @brief Data bytes after the register address, in bus order. */
  std::vector<std::uint8_t> bytes;

  /** @brief Equal when register and bytes are. */
  friend auto operator==(table_write const&, table_write const&) -> bool = default;
};

/** @brief Options of a table target. */
struct table_target_options
{
  /** @brief 7-bit bus address. */
  std::uint8_t address{};
  /** @brief Bytes of the register address a write transfer starts with, 1 to 4, most significant
   * first. */
  std::uint8_t register_bytes{1};
  /** @brief Reply of each register: the bytes a read from it returns, in order. */
  std::map<std::uint32_t, std::vector<std::uint8_t>> replies{};
  /** @brief Whether a read from a register without a reply is refused, its address not
   * acknowledged, instead of returning 0xFF. */
  bool nack_unknown{};
};

/**
 * @brief I2C target scripted by a table, for a device whose initialization a firmware writes
 *        and whose answers a board declares. A write transfer starts with the register address,
 *        and each one with data bytes is recorded when the next START or the STOP ends it. A
 *        read transfer returns the reply of the last register address written, then 0xFF past
 *        its end, or is not acknowledged for a register without a reply when @p nack_unknown is
 *        set. Writes never change the replies. Every byte is acknowledged. A repeated START
 *        addressing another target ends nothing here: the pending write is recorded at this
 *        target's own next START or STOP, and writes() misses it until then.
 */
class table_target final : public stm32::i2c_target
{
public:
  /**
   * @brief Creates the target.
   * @param options Address, register address size and replies.
   * @throws config_error when the address is not a 7-bit address or the register address size
   *         is not 1 to 4.
   */
  explicit table_target(table_target_options options);
  /** @brief Destroys the target. */
  ~table_target() override;

  [[nodiscard]] auto address() const -> std::uint8_t override;
  [[nodiscard]] auto start(bool read, vtime at) -> bool override;
  [[nodiscard]] auto write(std::uint8_t byte, vtime at) -> bool override;
  [[nodiscard]] auto read(vtime at) -> std::uint8_t override;
  void               stop(vtime at) override;
  /** @brief Appends the state: the transfer in progress and the recorded writes. */
  void save(std::vector<std::byte>& out) const override;
  /** @brief Restores bytes save() wrote. */
  void load(std::span<std::byte const> in) override;

  /** @brief Write transfers recorded so far, oldest first. */
  [[nodiscard]] auto writes() const -> std::span<table_write const>;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};
}
