#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/periph/stm32/i2c.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace popcorn::periph::devices
{
/** @brief Options of a 24xx I2C EEPROM. The defaults describe a 24xx256. */
struct eeprom_24xx_options
{
  /** @brief 7-bit bus address, 0x50 with the chip address pins low. */
  std::uint8_t address{0x50};
  /** @brief Array size in bytes, a multiple of @p page_size, at most 256 with one address byte
   * and 65536 with two. */
  std::uint32_t size{32768};
  /** @brief Bytes of the word address sent after the device address, 1 or 2, most significant
   * first. */
  std::uint8_t address_bytes{2};
  /** @brief Page size in bytes, a power of two. */
  std::uint32_t page_size{64};
  /** @brief Write cycle time, tWR, from the STOP ending a write. */
  std::chrono::nanoseconds write_cycle{std::chrono::milliseconds{5}};
  /** @brief Initial array contents, read at construction. Empty for an erased array, all 0xFF. A
   * shorter file leaves the rest erased. */
  std::filesystem::path image{};
};

/**
 * @brief Generic 24xx serial EEPROM on an I2C bus. A write transfer sends the word address,
 *        then data bytes that land in the page of that address, wrapping within the page. The
 *        STOP ending a write with at least one data byte starts the write cycle: for tWR in
 *        virtual time the device acknowledges neither its address nor anything else, the
 *        acknowledge polling of 24xx drivers. A read transfer returns bytes from the address
 *        pointer, which advances and wraps from the last byte to 0. A write of the word address
 *        only, then a repeated START in read, is a random read. Not modelled: write protection,
 *        identification pages, the chip address bits some small parts take from the word
 *        address.
 */
class eeprom_24xx final : public stm32::i2c_target
{
public:
  /**
   * @brief Creates the device, its array erased or read from @p options.image.
   * @param options Address, geometry, timing and image.
   * @throws config_error when the geometry is inconsistent, the address is not a 7-bit address,
   *         or the image cannot be read or is larger than the array.
   */
  explicit eeprom_24xx(eeprom_24xx_options options = {});
  /** @brief Destroys the device. */
  ~eeprom_24xx() override;

  [[nodiscard]] auto address() const -> std::uint8_t override;
  [[nodiscard]] auto start(bool read, vtime at) -> bool override;
  [[nodiscard]] auto write(std::uint8_t byte, vtime at) -> bool override;
  [[nodiscard]] auto read(vtime at) -> std::uint8_t override;
  void               stop(vtime at) override;
  /**
   * @brief On power_on and brown_out: the write cycle ended, the page already written, the
   *        address pointer at 0, the transfer in progress dropped. Other kinds do nothing.
   */
  void reset(reset_kind kind) override;
  /**
   * @brief Appends the state: the transfer in progress, the write cycle and the pages that
   *        differ from the initial image.
   */
  void save(std::vector<std::byte>& out) const override;
  /**
   * @brief Restores bytes save() wrote.
   * @throws config_error when they come from a device of another size or initial image.
   */
  void load(std::span<std::byte const> in) override;

  /** @brief The array. */
  [[nodiscard]] auto contents() const -> std::span<std::byte const>;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};
}
