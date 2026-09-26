#include "image.hpp"

#include <popcorn/image/raw.hpp>

#include <nanobind/nanobind.h>

#include <ranges>
#include <utility>

namespace popcorn::bind
{
namespace
{
namespace nb = reflex::py::nb;

auto info(popcorn::image::symbol const& sym) -> symbol_info
{
  return {
      .name      = sym.name,
      .value     = sym.value,
      .size      = sym.size,
      .function  = sym.kind == popcorn::image::symbol_kind::function,
      .is_global = sym.global,
  };
}

template <typename Load> auto released(Load load) -> image
{
  nb::gil_scoped_release unlocked;
  return image{load()};
}
}

auto owned(location const& where) -> location_info
{
  return {
      .pc     = where.pc,
      .symbol = std::string{where.symbol},
      .offset = where.offset,
      .file   = std::string{where.file},
      .line   = where.line,
  };
}

image::image(popcorn::image::image loaded) : image_{std::move(loaded)}
{}

auto image::digest() const -> std::string
{
  return image_.digest;
}

auto image::entry() const -> std::optional<address>
{
  return image_.entry;
}

auto image::abi() const -> arch::float_abi
{
  return image_.abi;
}

auto image::segments() const -> std::vector<segment_info>
{
  return image_.segments
       | std::views::transform([](popcorn::image::segment const& seg) {
           return segment_info{
               .name        = seg.name,
               .start       = seg.start,
               .file_size   = static_cast<std::uint32_t>(seg.bytes.size()),
               .memory_size = seg.memory_size,
               .executable  = seg.executable,
               .writable    = seg.writable,
           };
         })
       | std::ranges::to<std::vector>();
}

auto image::symbols() const -> std::vector<symbol_info>
{
  return image_.symbols | std::views::transform(info) | std::ranges::to<std::vector>();
}

auto image::lookup(std::string const& name) const -> symbol_info
{
  return info(image_.lookup(name));
}

auto image::locate(address addr) const -> location_info
{
  return owned(image_.locate(addr));
}

auto image::native() const noexcept -> popcorn::image::image const&
{
  return image_;
}

auto load_elf(std::string const& path) -> image
{
  return released([&] { return popcorn::image::load_elf(path); });
}

auto load_bin(std::string const& path, address start) -> image
{
  return released([&] { return popcorn::image::load_bin(path, start); });
}

auto load_hex(std::string const& path) -> image
{
  return released([&] { return popcorn::image::load_hex(path); });
}
}
