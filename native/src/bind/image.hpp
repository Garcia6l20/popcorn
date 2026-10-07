#pragma once

#include "casters.hpp"

#include <popcorn/arch/arch.hpp>
#include <popcorn/core/types.hpp>
#include <popcorn/image/image.hpp>

#include <reflex/py.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace popcorn::bind
{
namespace py = reflex::py;

/** @brief One loadable segment of an image, without its bytes. */
struct[[= py::rename{"SegmentInfo"}]] segment_info
{
  /** @brief Name for messages. */
  [[= py::readonly]] std::string name{};
  /** @brief Guest address of the first byte. */
  [[= py::readonly]] address start{};
  /** @brief Bytes in the file. */
  [[= py::readonly]] std::uint32_t file_size{};
  /** @brief Size in guest memory. */
  [[= py::readonly]] std::uint32_t memory_size{};
  /** @brief Whether the segment holds code. */
  [[= py::readonly]] bool executable{};
  /** @brief Whether the guest may write the segment. */
  [[= py::readonly]] bool writable{};
};

/** @brief One symbol of an image. */
struct[[= py::rename{"SymbolInfo"}]] symbol_info
{
  /** @brief Name. */
  [[= py::readonly]] std::string name{};
  /** @brief Address, the Thumb bit cleared for a function. */
  [[= py::readonly]] address value{};
  /** @brief Size in bytes, 0 when unknown. */
  [[= py::readonly]] std::uint32_t size{};
  /** @brief Whether the symbol is a function. */
  [[= py::readonly]] bool function{};
  /** @brief Whether the symbol has global binding. */
  [[= py::readonly]] bool is_global{};
};

/** @brief Source position of an address, owning its text. */
struct[[= py::rename{"Location"}]] location_info
{
  /** @brief Address. */
  [[= py::readonly]] address pc{};
  /** @brief Function or object containing pc, empty when unknown. */
  [[= py::readonly]] std::string symbol{};
  /** @brief Offset of pc from the symbol, 0 when symbol is empty. */
  [[= py::readonly]] std::uint32_t offset{};
  /** @brief Source file, empty when unknown. */
  [[= py::readonly]] std::string file{};
  /** @brief Source line, 0 when unknown. */
  [[= py::readonly]] std::uint32_t line{};
};

/**
 * @brief Converts a location into one that owns its text.
 * @param where Location whose views may not outlive the call.
 */
[[= py::skip]] auto owned(location const& where) -> location_info;

/** @brief Firmware image, immutable once loaded, shared by the machines that load it. */
class[[= py::rename{"Image"}]] image
{
public:
  /**
   * @brief Wraps a loaded image.
   * @param loaded The image.
   */
  [[= py::skip]] explicit image(popcorn::image::image loaded);

  /** @brief SHA-256 of the loadable content, lower-case hex. */
  [[= py::doc{"SHA-256 of the loadable content, lower-case hex."}]] [[nodiscard]] auto
      digest() const -> std::string;

  /** @brief Entry point, empty when the file has none. */
  [[= py::doc{"Entry point, None when the file has none."}]] [[nodiscard]] auto entry() const
      -> std::optional<address>;

  /** @brief Floating-point calling convention. */
  [[= py::doc{"Floating-point calling convention of the image."}]] [[nodiscard]] auto abi() const
      -> arch::float_abi;

  /** @brief Loadable segments, in address order. */
  [[= py::doc{"Loadable segments, in address order."}]] [[nodiscard]] auto segments() const
      -> std::vector<segment_info>;

  /** @brief Symbols, in address order. */
  [[= py::doc{"Symbols, in address order."}]] [[nodiscard]] auto symbols() const
      -> std::vector<symbol_info>;

  /**
   * @brief Looks a symbol up by exact name.
   * @param name Symbol name.
   * @throws symbol_error when none or several match.
   */
  [[= py::doc{
      "Symbol of that exact name, SymbolError with close matches when absent."}]] [[nodiscard]] auto
      lookup(std::string const& name) const -> symbol_info;

  /**
   * @brief Locates an address in this image alone.
   * @param addr Guest address.
   */
  [[= py::doc{"Symbol, offset, file and line of an address in this image."}]] [[nodiscard]] auto
      locate(address addr) const -> location_info;

  /** @brief The native image. */
  [[= py::skip]] [[nodiscard]] auto native() const noexcept -> popcorn::image::image const&;

private:
  popcorn::image::image image_;
};

/**
 * @brief Loads an ELF32 file.
 * @param path Path of the file.
 * @throws image_error when the file is not a loadable ELF32.
 */
[[= py::doc{"Load an ELF32 file."}]] auto load_elf(std::string const& path) -> image;

/**
 * @brief Loads a raw binary at an address.
 * @param path Path of the file.
 * @param start Guest address of its first byte.
 * @throws image_error when the file cannot be read.
 */
[[= py::doc{"Load a raw binary at an address."}]] auto
    load_bin(std::string const& path, address start) -> image;

/**
 * @brief Loads an Intel HEX file.
 * @param path Path of the file.
 * @throws image_error when the file is malformed.
 */
[[= py::doc{"Load an Intel HEX file."}]] auto load_hex(std::string const& path) -> image;
}
