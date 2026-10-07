#pragma once

#include <popcorn/core/fwd.hpp>
#include <popcorn/core/services.hpp>
#include <popcorn/core/types.hpp>

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

namespace popcorn::core
{
/** @brief Scalar the guest stores in memory: an integer or floating-point type of 1 to 8 bytes. */
template <typename T>
concept guest_scalar =
    (std::integral<T> or std::floating_point<T>) and (not std::same_as<T, bool>) and sizeof(T) <= 8;

/**
 * @brief Guest memory of a stopped machine, for test code and the Python facade. Every access goes
 * through the machine's bus as `initiator::debugger`, with the same semantics as
 * `machine_services::read` and its siblings: MMIO dispatch and watches see it, a model may treat a
 * debugger access as free of side effects. Bus errors are values.
 *
 * Valid while its machine lives. Every function throws machine_running while a slice runs,
 * except from a handler of that slice: handlers run while the core is paused at an instruction
 * boundary.
 */
class memory_view
{
public:
  /**
   * @brief Creates the view of a machine, as `machine::mem()` does.
   * @param target Machine whose memory the view reads and writes.
   */
  explicit memory_view(machine& target) noexcept : machine_{&target}
  {}

  /**
   * @brief Reads one naturally aligned access.
   * @param addr Guest address, aligned to @p size.
   * @param size Access size in bytes, 1, 2 or 4.
   * @return Value right-aligned to @p size, or why the bus refused the access.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  [[nodiscard]] auto read(address addr, std::uint8_t size) const
      -> std::expected<std::uint32_t, bus_error> pre(size == 1 or size == 2 or size == 4)
          pre(addr % size == 0);

  /**
   * @brief Writes one naturally aligned access.
   * @param addr Guest address, aligned to @p size.
   * @param size Access size in bytes, 1, 2 or 4.
   * @param value Value right-aligned to @p size.
   * @return Nothing, or why the bus refused the access.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  auto write(address addr, std::uint8_t size, std::uint32_t value) const
      -> std::expected<void, bus_error> pre(size == 1 or size == 2 or size == 4)
          pre(addr % size == 0) pre(fits_access(value, size));

  /**
   * @brief Reads a block of bytes, unaligned allowed, split as `machine_services::read_block`.
   * @param addr First guest address.
   * @param out Bytes read, in address order.
   * @return Nothing, or the failing access. The bytes of @p out before `at` are valid.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  auto read_block(address addr, std::span<std::byte> out) const
      -> std::expected<void, block_error> pre(fits_address_space(addr, out.size()));

  /**
   * @brief Writes a block of bytes, unaligned allowed, split as `machine_services::write_block`.
   * @param addr First guest address.
   * @param in Bytes to write, in address order.
   * @return Nothing, or the failing access. The bytes before `at` were written.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  auto write_block(address addr, std::span<std::byte const> in) const
      -> std::expected<void, block_error> pre(fits_address_space(addr, in.size()));

  /**
   * @brief Whether [@p addr, @p addr + @p size) lies inside one mapped memory region, RAM, flash
   *        or backup SRAM, and not in a peripheral window. Regions are fixed at construction, so
   *        it holds for the machine's life and is safe while a slice runs.
   * @param addr First byte.
   * @param size Access size in bytes, 1 to 8.
   * @return True for plain memory, false for a peripheral window or an unmapped byte.
   */
  [[nodiscard]] auto is_plain_memory(address addr, std::uint8_t size) const noexcept
      -> bool pre(size >= 1 and size <= 8);

  /**
   * @brief Reads a little-endian scalar, as `m.mem.u32[addr]` does in Python.
   * @param addr First guest address, unaligned allowed.
   * @return The value, or the failing access.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  template <guest_scalar T>
  [[nodiscard]] auto load(address addr) const
      -> std::expected<T, block_error> pre(fits_address_space(addr, sizeof(T)))
  {
    std::array<std::byte, sizeof(T)> bytes{};
    return read_block(addr, bytes).transform([&bytes] {
      return from_little(std::bit_cast<T>(bytes));
    });
  }

  /**
   * @brief Writes a little-endian scalar.
   * @param addr First guest address, unaligned allowed.
   * @param value Value to write.
   * @return Nothing, or the failing access.
   * @throws machine_running while a slice runs, outside its handlers.
   */
  template <guest_scalar T>
  auto store(address addr, T value) const
      -> std::expected<void, block_error> pre(fits_address_space(addr, sizeof(T)))
  {
    auto const bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(from_little(value));
    return write_block(addr, bytes);
  }

private:
  template <guest_scalar T> static constexpr auto from_little(T value) noexcept -> T
  {
    if constexpr(std::endian::native == std::endian::little or sizeof(T) == 1)
    {
      return value;
    }
    else if constexpr(std::integral<T>)
    {
      return std::byteswap(value);
    }
    else
    {
      using bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
      return std::bit_cast<T>(std::byteswap(std::bit_cast<bits>(value)));
    }
  }

  machine* machine_;
};
}
