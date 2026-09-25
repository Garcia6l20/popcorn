#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

struct uc_struct;

namespace popcorn::core
{
/** @brief Frees a page-aligned host allocation. */
struct aligned_free
{
  /** @brief Frees @p bytes. */
  void operator()(std::byte* bytes) const noexcept;
};

/** @brief Guest address range through which a host buffer is mapped. */
struct buffer_window
{
  /** @brief Guest address of the window's first byte. */
  address start{};
  /** @brief Buffer offset of the window's first byte. */
  std::size_t offset{};
  /** @brief Bytes in the window. */
  std::size_t size{};
};

/** @brief Host memory behind one region and every alias of it, mapped with `uc_mem_map_ptr`. */
struct host_buffer
{
  /** @brief Bytes, aligned to 4096. */
  std::unique_ptr<std::byte[], aligned_free> bytes;
  /** @brief Size in bytes. */
  std::size_t size{};
  /** @brief Whether any mapping of the buffer has execute access. */
  bool executable{};
  /** @brief Every guest window mapping the buffer, the region's own and each alias's. */
  std::vector<buffer_window> mappings;
};

/** @brief One description region as mapped in the engine. */
struct machine::mapped_region
{
  /** @brief Region of the machine's description. */
  region_desc const* desc{};
  /** @brief Buffer holding the bytes, shared with every alias of the same memory. */
  std::shared_ptr<host_buffer> buffer;
  /** @brief Offset of the region's first byte within the buffer. */
  std::size_t offset{};

  /**
   * @brief Allocates a host buffer per region without `physical`, maps every region in the
   *        engine, aliases onto their target's buffer.
   * @param engine Engine to map into.
   * @param description Description whose regions are mapped, which must outlive the result.
   * @return One entry per region, in description order.
   * @throws config_error naming a region that is not page aligned, or whose `physical` does not
   *         fall inside another region.
   * @throws emulation_error when the engine refuses a mapping.
   */
  static auto build(uc_struct* engine, mcu_description const& description)
      -> std::vector<std::unique_ptr<mapped_region>>;

  /**
   * @brief Region holding every byte of an access.
   * @param memory Mapped regions.
   * @param addr First guest address.
   * @param size Bytes, at least 1.
   * @return The region, null when no single region holds them all.
   */
  static auto
      find(std::span<std::unique_ptr<mapped_region> const> memory, address addr, std::size_t size)
          -> mapped_region*;

  /** @brief Whether the region holds every byte of [@p addr, @p addr + @p size). */
  [[nodiscard]] auto contains(address addr, std::size_t size) const noexcept -> bool;

  /** @brief Bytes left in the region from @p addr. */
  [[nodiscard]] auto remaining(address addr) const noexcept -> std::size_t pre(contains(addr, 1));

  /** @brief Host byte behind guest address @p addr. */
  [[nodiscard]] auto host(address addr) const noexcept -> std::byte* pre(contains(addr, 1));

  /**
   * @brief Removes translated code over bytes the host wrote, at every address the buffer is
   *        mapped, when the buffer is executable. Unicorn only sees guest stores.
   * @param engine Engine holding the translated code.
   * @param addr First written guest address.
   * @param size Bytes written.
   * @throws emulation_error when the engine refuses the removal.
   */
  void host_wrote(uc_struct* engine, address addr, std::size_t size) const
      pre(contains(addr, size));
};
}
