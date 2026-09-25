#include "refs.hpp"

#include <popcorn/image/image.hpp>
#include <popcorn/obs/coverage.hpp>
#include <popcorn/obs/errors.hpp>

#include <reflex/py.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace py = reflex::py;
namespace nb = py::nb;

namespace popcorn::obs::bind
{
namespace detail
{
using image_row = std::tuple<std::uint32_t, std::string, std::string, std::uint32_t, std::uint32_t>;
using range_row = std::tuple<std::string, std::uint32_t, std::uint32_t>;
using hit_row   = std::tuple<std::uint32_t, std::uint32_t, std::uint64_t>;
using group_row = std::tuple<
    std::vector<image_row>,
    std::vector<range_row>,
    std::vector<hit_row>,
    std::vector<hit_row>,
    std::uint64_t,
    std::uint64_t>;

auto copy(nb::bytes const& bytes) -> std::vector<std::byte>
{
  auto const* first = static_cast<std::byte const*>(bytes.data());
  return {first, first + bytes.size()};
}

auto to_bytes(std::vector<std::byte> const& bytes) -> nb::bytes
{
  return nb::bytes(bytes.data(), bytes.size());
}

auto row(image_ref const& ref) -> image_row
{
  return {std::to_underlying(ref.kind), ref.path, ref.digest, ref.base, ref.end};
}

auto rows(coverage_set const& set) -> std::vector<group_row>
{
  return set.groups
       | std::views::transform([](coverage_group const& group) {
           return group_row{
               group.images | std::views::transform(row) | std::ranges::to<std::vector>(),
               group.ranges | std::views::transform([](address_range const& range) {
                 return range_row{range.name, range.start, range.size};
               }) | std::ranges::to<std::vector>(),
               group.blocks | std::views::transform([](block_hit const& block) {
                 return hit_row{block.start, block.size, block.hits};
               }) | std::ranges::to<std::vector>(),
               group.edges | std::views::transform([](edge_hit const& edge) {
                 return hit_row{edge.from, edge.to, edge.hits};
               }) | std::ranges::to<std::vector>(),
               group.split,
               group.dropped,
           };
         })
       | std::ranges::to<std::vector>();
}

auto set_of(std::vector<group_row> const& groups) -> coverage_set
{
  coverage_set set;
  for(auto const& [images, ranges, blocks, edges, split, dropped] : groups)
  {
    auto& group = set.groups.emplace_back();
    for(auto const& [kind, path, digest, base, end] : images)
    {
      if(kind > std::to_underlying(image_kind::raw))
      {
        throw format_error("", 0, "an image kind of 0 or 1");
      }
      group.images.push_back(
          {.kind   = static_cast<image_kind>(kind),
           .path   = path,
           .digest = digest,
           .base   = base,
           .end    = end});
    }
    for(auto const& [name, start, size] : ranges)
    {
      group.ranges.push_back({.name = name, .start = start, .size = size});
    }
    for(auto const& [start, size, hits] : blocks)
    {
      group.blocks.push_back({.start = start, .size = size, .hits = hits});
    }
    for(auto const& [from, to, hits] : edges)
    {
      group.edges.push_back({.from = from, .to = to, .hits = hits});
    }
    group.split   = split;
    group.dropped = dropped;
  }
  return set;
}
}

[[= py::doc{"Sum .pcov files given as bytes, canonical .pcov bytes back."}]] auto
    merge_pcov(std::vector<nb::bytes> const& files) -> nb::bytes
{
  auto const copies = files | std::views::transform(detail::copy) | std::ranges::to<std::vector>();
  std::vector<std::byte> merged;
  {
    nb::gil_scoped_release released;
    auto const             sets = copies
                                | std::views::transform(
                                      [](std::vector<std::byte> const& file) { return decode_pcov(file); })
                                | std::ranges::to<std::vector>();
    merged                      = obs::encode_pcov(obs::merge(sets));
  }
  return detail::to_bytes(merged);
}

[[= py::doc{"Groups of a .pcov file: images (kind, path, digest, base, end), ranges (name, "
            "start, size), blocks (start, size, hits), edges (from, to, hits), split, "
            "dropped."}]] auto describe_pcov(nb::bytes const& pcov)
    -> std::vector<detail::group_row>
{
  auto const   copy = detail::copy(pcov);
  coverage_set set;
  {
    nb::gil_scoped_release released;
    set = decode_pcov(copy);
  }
  return detail::rows(set);
}

[[= py::doc{"Canonical .pcov bytes of groups shaped as describe_pcov returns them."}]] auto
    encode_pcov(std::vector<detail::group_row> const& groups) -> nb::bytes
{
  auto const             set = detail::set_of(groups);
  std::vector<std::byte> bytes;
  {
    nb::gil_scoped_release released;
    bytes = obs::encode_pcov(decode_pcov(obs::encode_pcov(set)));
  }
  return detail::to_bytes(bytes);
}

[[= py::doc{"lcov text of a .pcov file, its ELF images loaded from their paths."}]] auto
    lcov_text(nb::bytes const& pcov, bool branches, std::string const& test_name) -> std::string
{
  auto const  copy = detail::copy(pcov);
  std::string text;
  {
    nb::gil_scoped_release released;
    auto const             set    = decode_pcov(copy);
    auto const             images = load_images(set);
    text = lcov(set, images, {.branches = branches, .test_name = test_name});
  }
  return text;
}

[[= py::doc{"drcov version 2 bytes of a .pcov file, and the count of blocks left out."}]] auto
    drcov_bytes(nb::bytes const& pcov) -> std::tuple<nb::bytes, std::uint64_t>
{
  auto const copy = detail::copy(pcov);
  drcov_file file;
  {
    nb::gil_scoped_release released;
    file = drcov(decode_pcov(copy));
  }
  return {detail::to_bytes(file.bytes), file.dropped};
}

[[= py::doc{"Image reference of an ELF: (kind, absolute path, digest, base, end)."}]] auto
    image_ref_of(std::string const& path) -> detail::image_row
{
  obs::image_ref ref;
  {
    nb::gil_scoped_release released;
    ref = reference_of(image::load_elf(path), image_kind::elf, path);
  }
  return detail::row(ref);
}
}

REFLEX_PY_MODULE(_obs, m)
{
  nb::exception<popcorn::obs::format_error>(m.handle, "FormatError", PyExc_ValueError);
  m.bind<^^popcorn::obs::bind>();
}
