#pragma once

#include <popcorn/core/types.hpp>
#include <popcorn/export.hpp>
#include <popcorn/periph/spi.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace popcorn::periph::devices
{
/** @brief Identification and size of one AT25SF part. */
struct at25sf_part
{
  /** @brief Manufacturer ID, first byte of the JEDEC ID. */
  std::uint8_t manufacturer;
  /** @brief Memory type, second byte of the JEDEC ID. */
  std::uint8_t memory_type;
  /** @brief Capacity, third byte of the JEDEC ID. */
  std::uint8_t capacity;
  /** @brief Device ID returned by the 90h and ABh instructions. */
  std::uint8_t device_id;
  /** @brief Array size in bytes, a multiple of 64 KB. */
  std::uint32_t size;
};

/**
 * @brief The AT25SF641, 64 Mbit, JEDEC ID 1F 32 17, device ID 16h. The device ID follows the
 *        datasheet's Manufacturer and Device Identification table, the 90h instruction text
 *        says 17h and contradicts it.
 */
inline constexpr at25sf_part at25sf641{
    .manufacturer = 0x1F,
    .memory_type  = 0x32,
    .capacity     = 0x17,
    .device_id    = 0x16,
    .size         = 8u << 20};

/**
 * @brief Options of an AT25SF device. Times default to the typical values of the AT25SF641
 *        datasheet AC characteristics.
 */
struct at25sf_options
{
  /** @brief The part: IDs and size. */
  at25sf_part part{at25sf641};
  /** @brief Initial array contents, read at construction. Empty for an erased array, all 0xFF. A
   * shorter file leaves the rest erased. */
  std::filesystem::path image{};
  /** @brief Initial array contents when @p image is empty, at most `part.size` bytes, the rest
   * erased. Empty for an erased array. */
  std::vector<std::byte> initial{};
  /** @brief Page program time, tPP. */
  std::chrono::nanoseconds page_program{std::chrono::microseconds{600}};
  /** @brief 4 KB block erase time, tSE. */
  std::chrono::nanoseconds erase_4k{std::chrono::milliseconds{60}};
  /** @brief 32 KB block erase time, tBE1. */
  std::chrono::nanoseconds erase_32k{std::chrono::milliseconds{200}};
  /** @brief 64 KB block erase time, tBE2. */
  std::chrono::nanoseconds erase_64k{std::chrono::milliseconds{300}};
  /** @brief Chip erase time, tCE. */
  std::chrono::nanoseconds chip_erase{std::chrono::seconds{30}};
  /** @brief Write status register time, tW. */
  std::chrono::nanoseconds write_status{std::chrono::milliseconds{5}};
  /** @brief Chip select high to deep power down, tDP. */
  std::chrono::nanoseconds power_down{std::chrono::microseconds{3}};
  /** @brief Chip select high to standby after a release from deep power down, tRES1. */
  std::chrono::nanoseconds release{std::chrono::microseconds{3}};
  /** @brief Chip select high to the next instruction after a reset, tRST. */
  std::chrono::nanoseconds reset{std::chrono::microseconds{30}};
};

/**
 * @brief AT25SF serial NOR flash on an SPI bus, standard SPI instructions. Instructions start at
 *        the chip select falling edge and a program, erase or status write executes at its rising
 *        edge, at a byte boundary, while WEL is set, then keeps BUSY set for its time in virtual
 *        time. While BUSY only the status reads 05h and 35h are accepted, in deep power down only
 *        ABh. Supported: 06h, 04h, 50h, 05h, 35h, 01h, 31h, 03h, 0Bh, 02h within the 256-byte
 *        page, wrapping, ANDed into the array, 20h, 52h, D8h, 60h, C7h, B9h, ABh, 90h, 9Fh,
 *        66h then 99h, a reset accepted while BUSY, ending it and restoring the volatile status
 *        from the non-volatile one. 01h and 31h after 50h write the volatile status only. Frames of
 * any multiple of 8 bits are split into bytes, most significant first; another frame size ends the
 * instruction. The device drives MISO only with data, all ones otherwise. Not modelled, ignored:
 * dual, quad and QPI instructions, suspend and resume, the security registers, SFDP. Block
 * protection bits are stored, not enforced. A power cut while BUSY keeps the first part of the
 * operation's bytes in proportion to the time elapsed and the old bytes after it, in block order
 * for an erase and in the order the data was sent for a program, from its start address and
 * wrapping inside the page. This prefix rule is the model's deterministic choice, not datasheet
 * behaviour. A program or erase keeps a copy of its range, up to the whole array for a chip erase,
 * until the next selection after BUSY ends, a 66h 99h reset or a power cut. A snapshot taken
 * before then carries it.
 */
class POPCORN_API at25sf final : public spi_target
{
public:
  /**
   * @brief Creates the device, its array erased, read from @p options.image or copied from
   *        @p options.initial.
   * @param options Part, image and times.
   * @throws config_error when the part size is 0 or not a multiple of 64 KB, the image cannot
   *         be read or is larger than the part, or @p options.initial is set together with an
   *         image or is larger than the part.
   */
  explicit at25sf(at25sf_options options = {});
  /** @brief Destroys the device. */
  ~at25sf() override;

  void               select(bool selected, vtime at) override;
  [[nodiscard]] auto exchange(std::uint32_t out, std::uint8_t bits, vtime at)
      -> std::uint32_t override;
  /**
   * @brief On power_on and brown_out: status registers reloaded from their non-volatile
   *        copies with WEL clear, BUSY, the tRST and tRES1 waits and deep power down ended,
   *        the instruction in progress dropped. The array keeps its contents. Other kinds do
   *        nothing.
   */
  void reset(reset_kind kind) override;
  /**
   * @brief Tears a page program or erase still BUSY at @p at: of the `n` bytes of its range in
   *        tear order, the first `floor(n * elapsed / time)` keep the result, the rest return to
   *        their old bytes. Status writes and an operation already ended keep their result.
   * @param at Virtual time since power-on of the cut.
   */
  void power_cut(vtime at) override;
  /**
   * @brief Appends the state: status, timings, the instruction in progress, the pages that
   *        differ from the initial image and, while BUSY, the old bytes of the operation's range.
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
  struct POPCORN_HIDDEN impl;
  std::unique_ptr<impl> impl_;
};
}
