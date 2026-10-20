#include "pdm.hpp"
#include "machine.hpp"

#include <popcorn/core/errors.hpp>

#include <format>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace popcorn::bind
{
namespace
{
template <typename Queue>
auto live(std::shared_ptr<Queue> const& queue, detail::alive_flag const& alive) -> Queue&
{
  if(not detail::is_alive(alive))
  {
    throw config_error("pdm", 0, "machine", "an MDF or ADF whose machine still exists");
  }
  return *queue;
}
}

pdm_link::pdm_link(
    periph::stm32::pdm_port& port,
    std::size_t              sitf,
    periph::stm32::pdm_edge  edge,
    detail::alive_flag       alive)
    : source_{std::make_shared<detail::pdm_queue_source>()}, port_{&port}, alive_{std::move(alive)}
{
  port.attach(sitf, edge, source_);
}

auto pdm_link::source() const -> detail::pdm_queue_source&
{
  return live(source_, alive_);
}

auto pdm_link::queued_samples() const -> std::uint64_t
{
  auto& queue = source();
  return detail::link_locked(
      queue, [&](core::link_guard const& held) { return queue.queued_samples(held); });
}

auto pdm_link::queued_bits() const -> std::uint64_t
{
  auto& queue = source();
  return detail::link_locked(
      queue, [&](core::link_guard const& held) { return queue.queued_bits(held); });
}

auto pdm_link::idle_bits() const -> std::uint64_t
{
  auto& queue = source();
  return detail::link_locked(
      queue, [&](core::link_guard const& held) { return queue.idle_bits(held); });
}

auto pdm_link::op_queued_samples() const -> access_op
{
  std::ignore = source();
  return detail::link_op([queue = source_, alive = alive_] {
    auto& source = live(queue, alive);
    return source.queued_samples(source.lock());
  });
}

auto pdm_link::op_queued_bits() const -> access_op
{
  std::ignore = source();
  return detail::link_op([queue = source_, alive = alive_] {
    auto& source = live(queue, alive);
    return source.queued_bits(source.lock());
  });
}

auto pdm_link::op_idle_bits() const -> access_op
{
  std::ignore = source();
  return detail::link_op([queue = source_, alive = alive_] {
    auto& source = live(queue, alive);
    return source.idle_bits(source.lock());
  });
}

auto pdm_link::command_write_pcm(std::vector<std::int16_t> samples, std::uint32_t hz) const
    -> access_op
{
  if(hz == 0)
  {
    throw config_error("pdm", 0, "hz", "a sample rate of at least 1 Hz");
  }
  std::ignore = source();
  return detail::link_op(
      [queue = source_, alive = alive_, port = port_, samples = std::move(samples), hz] {
        auto& source = live(queue, alive);
        port->settle();
        source.write_pcm(source.lock(), samples, hz);
      });
}

auto pdm_link::command_write_pdm(reflex::py::nb::bytes const& data, std::uint64_t bits) const
    -> access_op
{
  if(bits > 8 * std::uint64_t{data.size()})
  {
    throw config_error("pdm", 0, "bits", "at most 8 bits per byte of data");
  }
  std::ignore       = source();
  auto const packed = std::span{static_cast<std::uint8_t const*>(data.data()), data.size()};
  return detail::link_op([queue = source_, alive = alive_, port = port_,
                          bytes = std::vector<std::uint8_t>{packed.begin(), packed.end()}, bits] {
    auto& source = live(queue, alive);
    port->settle();
    source.write_pdm(source.lock(), bytes, bits);
  });
}

void pdm_link::discard()
{
  auto& queue = source();
  detail::link_locked(queue, [&](core::link_guard const& held) { queue.discard(held); });
}

pdm_tap_link::pdm_tap_link(
    periph::stm32::pdm_port& port,
    std::size_t              filter,
    detail::reset_count      resets,
    detail::alive_flag       alive,
    detail::host_ref         host)
    : tap_{std::make_shared<detail::pdm_queue_tap>(std::move(resets))}, alive_{std::move(alive)},
      host_{std::move(host)}
{
  port.attach_tap(filter, tap_);
}

auto pdm_tap_link::tap() const -> detail::pdm_queue_tap&
{
  return live(tap_, alive_);
}

auto pdm_tap_link::available() const -> std::uint64_t
{
  auto& queue = tap();
  return detail::link_locked(
      queue, [&](core::link_guard const& held) { return queue.available(held); });
}

auto pdm_tap_link::dropped() const -> std::uint64_t
{
  auto& queue = tap();
  return detail::link_locked(
      queue, [&](core::link_guard const& held) { return queue.dropped(held); });
}

auto pdm_tap_link::op_available() const -> access_op
{
  std::ignore = tap();
  return detail::link_op([queue = tap_, alive = alive_] {
    auto& tap = live(queue, alive);
    return tap.available(tap.lock());
  });
}

auto pdm_tap_link::op_dropped() const -> access_op
{
  std::ignore = tap();
  return detail::link_op([queue = tap_, alive = alive_] {
    auto& tap = live(queue, alive);
    return tap.dropped(tap.lock());
  });
}

auto pdm_tap_link::drain(std::uint64_t count) -> std::vector<pdm_sample_record>
{
  return drain_until(count, std::numeric_limits<std::uint64_t>::max(), std::nullopt);
}

auto pdm_tap_link::drain_until(
    std::uint64_t                count,
    std::uint64_t                until_ps,
    std::optional<std::uint64_t> resets) -> std::vector<pdm_sample_record>
{
  auto&      queue = tap();
  auto const taken = detail::link_locked(queue, [&](core::link_guard const& held) {
    return queue.take(held, count, until_ps, resets);
  });
  return taken
       | std::views::transform([](detail::queued_pdm_sample const& sample) {
           return pdm_sample_record{
               .pcm      = sample.pcm,
               .at_ps    = sample.at_ps,
               .overrun  = sample.overrun,
               .captured = sample.captured,
           };
         })
       | std::ranges::to<std::vector>();
}

auto pdm_tap_link::predicate_available(std::uint64_t count) const -> native_predicate
{
  std::ignore = tap();
  return native_predicate{
      [queue = tap_, alive = alive_, count] {
        return not detail::is_alive(alive)
            or detail::link_locked(*queue, [&](core::link_guard const& held) {
                 return queue->available(held) >= count;
               });
      },
      host_}
      .described("filter samples");
}

void pdm_tap_link::discard()
{
  auto& queue = tap();
  detail::link_locked(queue, [&](core::link_guard const& held) { queue.discard(held); });
}

void pdm_tap_link::set_limit(std::uint64_t limit)
{
  if(limit == 0)
  {
    throw config_error("pdm", 0, "limit", "a sample limit of at least 1");
  }
  auto& queue = tap();
  detail::link_locked(queue, [&](core::link_guard const& held) { queue.set_limit(held, limit); });
}

namespace
{
auto pdm_port_of(core::machine& engine, std::string const& peripheral) -> periph::stm32::pdm_port&
{
  auto* const port = dynamic_cast<periph::stm32::pdm_port*>(engine.model(peripheral));
  if(port == nullptr)
  {
    throw model_error(peripheral, "", 0);
  }
  return *port;
}

void check_index(
    periph::stm32::pdm_port const& port,
    std::string const&             peripheral,
    std::string const&             what,
    std::size_t                    index)
{
  if(index >= port.sitfs())
  {
    throw config_error(
        peripheral, 0, what,
        std::format("a {} from 0 to {}, not {}", what, port.sitfs() - 1, index));
  }
}
}

auto machine_handle::pdm_names() const -> std::vector<std::string>
{
  std::vector<std::string> names;
  for(auto const& unit : engine().description().peripherals)
  {
    if(not unit.alias_of
       and dynamic_cast<periph::stm32::pdm_port*>(engine().model(unit.name)) != nullptr)
    {
      names.push_back(unit.name);
    }
  }
  return names;
}

auto machine_handle::pdm_sitfs(std::string const& peripheral) const -> std::size_t
{
  return pdm_port_of(engine(), peripheral).sitfs();
}

auto machine_handle::attach_pdm(
    std::string const& peripheral,
    std::size_t        sitf,
    std::string const& edge) -> pdm_link
{
  auto& port = pdm_port_of(engine(), peripheral);
  check_index(port, peripheral, "sitf", sitf);
  if(edge != "rising" and edge != "falling")
  {
    throw config_error(
        peripheral, 0, "edge", std::format("a clock edge rising or falling, not {}", edge));
  }
  auto const which =
      edge == "rising" ? periph::stm32::pdm_edge::rising : periph::stm32::pdm_edge::falling;
  return access([&](core::machine&) {
    port.settle();
    return pdm_link{port, sitf, which, alive_};
  });
}

auto machine_handle::attach_pdm_tap(std::string const& peripheral, std::size_t filter)
    -> pdm_tap_link
{
  auto& port = pdm_port_of(engine(), peripheral);
  check_index(port, peripheral, "filter", filter);
  return access([&](core::machine& target) {
    return pdm_tap_link{port, filter, target.reset_counter(), alive_, slot_};
  });
}
}
