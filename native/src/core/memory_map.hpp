#pragma once

#include <popcorn/core/description.hpp>
#include <popcorn/core/machine.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct uc_struct;

namespace popcorn::periph
{
class model;
}

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
  /** @brief Whether guest stores may go through the window. */
  bool writable{};
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
  /** @brief Region desc outside the description, the scratch region's, which desc points to. */
  std::unique_ptr<region_desc const> owned{};
  /** @brief Model of the peripheral the description names as the region's controller, null
   * without one or when that peripheral has no model. Set once every model is attached. */
  periph::model* controller{};

  /**
   * @brief Allocates a host buffer per region without `physical`, maps every region in the
   *        engine, aliases onto their target's buffer, then the scratch region as RAM,
   *        read-write, not executable.
   * @param engine Engine to map into.
   * @param description Description whose regions are mapped, which must outlive the result.
   * @param scratch `machine_options::scratch`.
   * @return One entry per region, in description order, then the scratch region.
   * @throws config_error naming a region that is not page aligned, or whose `physical` does not
   *         fall inside another region, or key `scratch` when the scratch region is empty, not
   *         page aligned, overlaps a region or a peripheral, or shares a region's name.
   * @throws emulation_error when the engine refuses a mapping.
   */
  static auto build(
      uc_struct*                          engine,
      mcu_description const&              description,
      std::optional<address_range> const& scratch) -> std::vector<std::unique_ptr<mapped_region>>;

  /**
   * @brief Unmapped entry for the scratch region, its desc owned.
   * @param description Description the region must stay clear of.
   * @param scratch `machine_options::scratch`.
   * @param page Engine page size in bytes.
   * @throws config_error with key `scratch`, as build() documents.
   */
  static auto scratch_region(
      mcu_description const& description,
      address_range const&   scratch,
      std::uint32_t          page) -> std::unique_ptr<mapped_region>;

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
   *        mapped, when the buffer is executable: at once between engine runs, at the next block
   *        boundary from inside one. Unicorn only sees guest stores.
   * @param run Run state of the machine owning the region.
   * @param addr First written guest address.
   * @param size Bytes written.
   * @throws emulation_error when the engine refuses an immediate removal.
   */
  void host_wrote(run_state& run, address addr, std::size_t size) const pre(contains(addr, size));
};

/**
 * @brief Removes translated code over buffer bytes [@p first, @p last) at every window mapping
 *        them.
 * @return The engine removal calls made, one per window overlapping the range.
 * @throws emulation_error when the engine refuses the removal.
 */
auto remove_translated(
    uc_struct*         engine,
    host_buffer const& buffer,
    std::size_t        first,
    std::size_t        last) -> std::size_t;
}
