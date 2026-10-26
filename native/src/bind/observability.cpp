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

trace_handle::state::state(
    std::unique_ptr<obs::pctr_writer> written,
    core::hook_handle                 feeding,
    detail::host_ref                  owner) noexcept
    : writer{std::move(written)}, sink{std::move(feeding)}, host{std::move(owner)}
{}

trace_handle::state::~state()
{
  if(sink.active())
  {
    detail::retire(host, [feeding = std::move(sink), written = std::move(writer)] mutable {
      feeding.reset();
      written.reset();
    });
  }
}

trace_handle::trace_handle(
    std::unique_ptr<obs::pctr_writer> writer,
    core::hook_handle                 sink,
    detail::host_ref                  host) noexcept
    : state_{std::make_shared<state>(std::move(writer), std::move(sink), std::move(host))}
{}

void trace_handle::close()
{
  contract_assert(state_ != nullptr);
  auto& traced = *state_;
  auto  active = false;
  {
    nb::gil_scoped_release const released;
    std::scoped_lock const       lock{traced.close_mutex};
    if(traced.closed)
    {
      return;
    }
    active = traced.sink.active();
  }
  if(active)
  {
    detail::host_access(traced.host, [&traced] {
      nb::gil_scoped_release const released;
      std::scoped_lock const       lock{traced.close_mutex};
      traced.sink.reset();
    });
  }
  nb::gil_scoped_release const released;
  std::scoped_lock const       lock{traced.close_mutex};
  if(not std::exchange(traced.closed, true))
  {
    traced.sink.reset();
    traced.writer->close();
  }
}

auto trace_handle::command_close() -> access_op
{
  return detail::link_op([traced = state_] {
    std::scoped_lock const lock{traced->close_mutex};
    if(not std::exchange(traced->closed, true))
    {
      traced->sink.reset();
      traced->writer->close();
    }
  });
}

auto trace_handle::records() const noexcept -> std::uint64_t
{
  return state_->writer->records();
}

auto trace_handle::size() const noexcept -> std::uint64_t
{
  return state_->writer->bytes();
}

auto machine_handle::function_ranges(std::vector<std::string> const& names) const
    -> std::vector<detail::range_row>
{
  return access([&](core::machine& target) {
    return obs::function_ranges(target.observed_images(), names)
         | std::views::transform([](address_range const& range) {
             return detail::range_row{range.name, range.start, range.size};
           })
         | std::ranges::to<std::vector>();
  });
}

void machine_handle::enable_coverage(
    std::vector<detail::range_row> const& ranges,
    bool                                  record_unmodelled)
{
  auto native = native_ranges(ranges);
  access(
      [&](core::machine& target) { target.enable_coverage(std::move(native), record_unmodelled); });
}

void machine_handle::disable_coverage()
{
  access([](core::machine& target) { target.disable_coverage(); });
}

auto machine_handle::coverage_enabled() const -> bool
{
  return access([](core::machine& target) { return target.coverage_enabled(); });
}

void machine_handle::clear_coverage()
{
  access([](core::machine& target) { target.clear_coverage(); });
}

auto machine_handle::coverage_pcov(std::vector<std::string> const& elf_paths) -> nb::bytes
{
  auto const sources =
      elf_paths
      | std::views::transform([](std::string const& path) {
          return obs::image_source{
              .kind = path.empty() ? obs::image_kind::raw : obs::image_kind::elf, .path = path};
        })
      | std::ranges::to<std::vector>();
  auto const bytes = access([&](core::machine& target) {
    auto const observed = target.observed_images().size();
    if(sources.size() != observed)
    {
      throw config_error(
          "coverage", 0, "sources",
          std::format("one source per loaded or symbols-only image, {}", observed));
    }
    nb::gil_scoped_release released;
    return obs::encode_pcov(obs::collect(target, sources));
  });
  return nb::bytes(bytes.data(), bytes.size());
}

auto machine_handle::open_trace(
    std::string const&                    path,
    std::vector<std::uint8_t> const&      kinds,
    std::vector<detail::range_row> const& ranges) -> trace_handle
{
  auto const selected = kinds_of(kinds);
  auto       native   = native_ranges(ranges);
  return access([&](core::machine& target) {
    auto writer = obs::pctr_writer_for(target, path);
    auto sink   = target.add_trace_sink(selected, writer->sink(), std::move(native));
    return trace_handle{std::move(writer), std::move(sink), slot_};
  });
}

void machine_handle::write_ring(std::string const& path) const
{
  access([&](core::machine& target) {
    auto&                    ring    = target.trace();
    auto const               records = ring.records();
    auto const               origin  = ring.origin().value;
    auto                     writer  = obs::pctr_writer_for(target, path);
    core::trace_record const start{
        .kind = core::trace_kind::time,
        .payload =
            {static_cast<std::uint32_t>(origin), static_cast<std::uint32_t>(origin >> 32), 0},
    };
    nb::gil_scoped_release released;
    writer->write(std::span{&start, 1});
    writer->write(records);
    writer->close();
  });
}

void machine_handle::clear_ring()
{
  access([](core::machine& target) { target.trace().clear(target.virtual_clock().now()); });
}

void machine_handle::set_ring_blocks(bool on)
{
  access([on](core::machine& target) { target.set_ring_blocks(on); });
}

auto machine_handle::ring_blocks() const -> bool
{
  return access([](core::machine& target) { return target.ring_blocks(); });
}
}
