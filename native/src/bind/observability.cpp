#include "observability.hpp"
#include "machine.hpp"
#include "python_callable.hpp"

#include <popcorn/core/errors.hpp>
#include <popcorn/core/log.hpp>
#include <popcorn/core/trace.hpp>
#include <popcorn/obs/collect.hpp>
#include <popcorn/obs/coverage.hpp>

#include <nanobind/nanobind.h>

#include <bit>
#include <cstddef>
#include <format>
#include <meta>
#include <ranges>
#include <utility>
#include <vector>

namespace popcorn::bind
{
namespace
{
namespace nb = reflex::py::nb;

auto native_ranges(std::vector<detail::range_row> const& rows) -> std::vector<address_range>
{
  std::vector<address_range> out;
  for(auto const& [name, start, size] : rows)
  {
    if(size == 0 or std::uint64_t{start} + size > std::uint64_t{1} << 32)
    {
      throw config_error(
          "ranges", 0, name.empty() ? std::format("{:#010x}", start) : name,
          std::format("a non-empty range within the 32-bit address space, not {} bytes", size));
    }
    out.push_back({.name = name, .start = start, .size = size});
  }
  return out;
}

consteval auto kinds_are_bits() -> bool
{
  template for(constexpr auto e :
               std::define_static_array(std::meta::enumerators_of(^^core::trace_kind)))
  {
    if(std::bit_cast<core::trace_kinds>(std::uint32_t{1} << std::to_underlying([:e:]))
       != core::trace_kinds{[:e:]})
    {
      return false;
    }
  }
  return true;
}

static_assert(sizeof(core::trace_kinds) == sizeof(std::uint32_t));
static_assert(kinds_are_bits(), "trace_kinds holds one bit per trace_kind, at its value");

auto kinds_of(std::vector<std::uint8_t> const& values) -> core::trace_kinds
{
  std::uint32_t bits = 0;
  for(auto const value : values)
  {
    auto selectable = false;
    template for(constexpr auto e :
                 std::define_static_array(std::meta::enumerators_of(^^core::trace_kind)))
    {
      selectable = selectable
                or (value == std::to_underlying([:e:]) and [:e:] != core::trace_kind::str and [:e:]
                    != core::trace_kind::time);
    }
    if(not selectable)
    {
      throw config_error(
          "trace", 0, "kinds", std::format("trace kinds a sink selects, not {}", value));
    }
    bits |= std::uint32_t{1} << value;
  }
  return std::bit_cast<core::trace_kinds>(bits);
}
}

trace_handle::trace_handle(
    std::unique_ptr<obs::pctr_writer> writer,
    core::hook_handle                 sink) noexcept
    : writer_{std::move(writer)}, sink_{std::move(sink)}
{}

void trace_handle::close()
{
  nb::gil_scoped_release released;
  sink_.reset();
  writer_->close();
}

auto trace_handle::records() const noexcept -> std::uint64_t
{
  return writer_->records();
}

auto trace_handle::size() const noexcept -> std::uint64_t
{
  return writer_->bytes();
}

auto machine_handle::function_ranges(std::vector<std::string> const& names) const
    -> std::vector<detail::range_row>
{
  return obs::function_ranges(machine_->loaded_images(), names)
       | std::views::transform([](address_range const& range) {
           return detail::range_row{range.name, range.start, range.size};
         })
       | std::ranges::to<std::vector>();
}

void machine_handle::enable_coverage(std::vector<detail::range_row> const& ranges)
{
  machine_->enable_coverage(native_ranges(ranges));
}

void machine_handle::disable_coverage()
{
  machine_->disable_coverage();
}

auto machine_handle::coverage_enabled() const noexcept -> bool
{
  return machine_->coverage_enabled();
}

void machine_handle::clear_coverage()
{
  machine_->clear_coverage();
}

auto machine_handle::coverage_pcov(std::vector<std::string> const& elf_paths) -> nb::bytes
{
  auto const loaded = machine_->loaded_images().size();
  if(elf_paths.size() != loaded)
  {
    throw config_error(
        "coverage", 0, "sources", std::format("one source per loaded image, {}", loaded));
  }
  auto const sources =
      elf_paths
      | std::views::transform([](std::string const& path) {
          return obs::image_source{
              .kind = path.empty() ? obs::image_kind::raw : obs::image_kind::elf, .path = path};
        })
      | std::ranges::to<std::vector>();
  std::vector<std::byte> bytes;
  {
    nb::gil_scoped_release released;
    bytes = obs::encode_pcov(obs::collect(*machine_, sources));
  }
  return nb::bytes(bytes.data(), bytes.size());
}

auto machine_handle::open_trace(
    std::string const&                    path,
    std::vector<std::uint8_t> const&      kinds,
    std::vector<detail::range_row> const& ranges) -> trace_handle
{
  auto const selected = kinds_of(kinds);
  auto       native   = native_ranges(ranges);
  auto       writer   = obs::pctr_writer_for(*machine_, path);
  auto       sink     = machine_->add_trace_sink(selected, writer->sink(), std::move(native));
  return trace_handle{std::move(writer), std::move(sink)};
}

void machine_handle::write_ring(std::string const& path) const
{
  auto&                    ring    = machine_->trace();
  auto const               records = ring.records();
  auto const               origin  = ring.origin().value;
  auto                     writer  = obs::pctr_writer_for(*machine_, path);
  core::trace_record const start{
      .kind    = core::trace_kind::time,
      .payload = {static_cast<std::uint32_t>(origin), static_cast<std::uint32_t>(origin >> 32), 0},
  };
  nb::gil_scoped_release released;
  writer->write(std::span{&start, 1});
  writer->write(records);
  writer->close();
}

void machine_handle::clear_ring()
{
  machine_->trace().clear(machine_->virtual_clock().now());
}

auto machine_handle::add_log_sink(nb::object sink) -> hook
{
  auto callable = detail::held(std::move(sink));
  return hook{machine_->add_log_sink([callable](core::log_record const& record) {
    nb::gil_scoped_acquire locked;
    callable->fn(record);
  })};
}
}
